// The shader cache (Graphics/Render/RHI/ShaderCache.h), the derived-data cache's first client: a warm
// pass reads back exactly the DXIL a compile gives, so cached and compiled programs never differ;
// editing an include misses exactly the programs that include it, and reverting it hits again (the
// manifest keeps the older input set); the debug-information variant, which embeds absolute paths,
// never hits across shader roots, while the variant without one is shared by every checkout; a failed
// compile writes nothing; -Ddc:Verify compiles hits again and reports BUILD002 for different bytes;
// without a store every program is compiled. The Shader.Dxil KeyBlob's fields are frozen here (the
// golden key-schema test, design §7.4): changing them without a new builder GUID would let keys of
// two meanings collide in a shared DDC.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "Assets/DerivedDataCache.h"
#include "Assets/KeyWriter.h"
#include "Containers/String.h"
#include "Graphics/Render/RHI/ShaderCache.h"
#include "Graphics/Render/RHI/ShaderCompiler.h"
#include "Hash/StableHash.h"
#include "Log/LogCapture.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace frt::graphics::rhi;


namespace
{
// The include, with a non-ASCII comment: read in the wrong encoding it would fail, not pass unseen.
constexpr FStringLiteral IncludeText =
	"// Спільне: the value the first shader writes\n"
	"static const uint ValueA = 1u;\n";
// The include edited: another value, and another size.
constexpr FStringLiteral EditedIncludeText =
	"// Спільне: the value the first shader writes\n"
	"static const uint ValueA = 12345u;\n";

constexpr FStringLiteral FirstText =
	"#include \"A.hlsli\"\n"
	"RWStructuredBuffer<uint> Output : register(u0);\n"
	"[numthreads(1, 1, 1)]\n"
	"void main () { Output[0] = ValueA; }\n";

constexpr FStringLiteral SecondText =
	"RWStructuredBuffer<uint> Output : register(u0);\n"
	"[numthreads(1, 1, 1)]\n"
	"void main () { Output[0] = 7u; }\n";

std::vector<uint8> GetBytes (const SShaderBinary& Binary)
{
	if (!Binary.Blob)
	{
		return {};
	}
	const uint8* data = static_cast<const uint8*>(Binary.Blob->GetBufferPointer());
	return std::vector<uint8>(data, data + Binary.Blob->GetBufferSize());
}

bool Contains (std::span<const uint8> Haystack, std::span<const uint8> Needle)
{
	return !Needle.empty()
		&& std::search(Haystack.begin(), Haystack.end(), Needle.begin(), Needle.end()) != Haystack.end();
}


// ---- A KeyBlob read back (the format of Assets/KeyWriter.h), for the key-schema test ----

struct SKeyField
{
	FString Path;                  // "Settings.Defines.A"
	assets::EKeyFieldType Type = assets::EKeyFieldType::Str;
	std::vector<uint8> Value;      // the value's bytes after any length prefix; empty for a record
};

FStringLiteral GetTypeName (assets::EKeyFieldType Type)
{
	switch (Type)
	{
	case assets::EKeyFieldType::Str: return "Str";
	case assets::EKeyFieldType::Guid: return "Guid";
	case assets::EKeyFieldType::Record: return "Record";
	case assets::EKeyFieldType::Hash160: return "Hash160";
	case assets::EKeyFieldType::U64: return "U64";
	case assets::EKeyFieldType::I64: return "I64";
	case assets::EKeyFieldType::Bool: return "Bool";
	case assets::EKeyFieldType::F32: return "F32";
	case assets::EKeyFieldType::F64: return "F64";
	case assets::EKeyFieldType::Enum: return "Enum";
	case assets::EKeyFieldType::Bytes: return "Bytes";
	}
	return "?";
}

bool ReadVarint (std::span<const uint8> Bytes, size_t& Position, uint64& Out)
{
	Out = 0u;
	for (uint32 shift = 0u; shift < 64u && Position < Bytes.size(); shift += 7u)
	{
		const uint8 byte = Bytes[Position++];
		Out |= static_cast<uint64>(byte & 0x7Fu) << shift;
		if ((byte & 0x80u) == 0u)
		{
			return true;
		}
	}
	return false;
}

// Appends every field of the record at Position, nested ones after their record, in blob order.
bool DecodeRecord (std::span<const uint8> Bytes, size_t& Position, FStringView Prefix, std::vector<SKeyField>& Out)
{
	uint64 count = 0u;
	if (!ReadVarint(Bytes, Position, count))
	{
		return false;
	}
	for (uint64 i = 0u; i < count; ++i)
	{
		uint64 nameLength = 0u;
		if (Position >= Bytes.size())
		{
			return false;
		}
		const assets::EKeyFieldType type = static_cast<assets::EKeyFieldType>(Bytes[Position++]);
		if (!ReadVarint(Bytes, Position, nameLength) || Position + nameLength > Bytes.size())
		{
			return false;
		}
		const FStringView name(reinterpret_cast<const char*>(Bytes.data() + Position), static_cast<uint32>(nameLength));
		Position += static_cast<size_t>(nameLength);
		SKeyField field;
		field.Path = Prefix.IsEmpty() ? FString(name) : FString(Prefix) + "." + name;
		field.Type = type;

		uint64 size = 0u;
		switch (type)
		{
		case assets::EKeyFieldType::Str:
		case assets::EKeyFieldType::Enum:
		case assets::EKeyFieldType::Bytes:
			if (!ReadVarint(Bytes, Position, size))
			{
				return false;
			}
			break;
		case assets::EKeyFieldType::Guid: size = 16u; break;
		case assets::EKeyFieldType::Hash160: size = 20u; break;
		case assets::EKeyFieldType::U64:
		case assets::EKeyFieldType::I64:
		case assets::EKeyFieldType::F64: size = 8u; break;
		case assets::EKeyFieldType::F32: size = 4u; break;
		case assets::EKeyFieldType::Bool: size = 1u; break;
		case assets::EKeyFieldType::Record:
		{
			const FString path = field.Path;
			Out.push_back(std::move(field));
			if (!DecodeRecord(Bytes, Position, path, Out))
			{
				return false;
			}
			continue;
		}
		default:
			return false;
		}
		if (Position + size > Bytes.size())
		{
			return false;
		}
		const auto begin = Bytes.begin() + static_cast<ptrdiff_t>(Position);
		field.Value.assign(begin, begin + static_cast<ptrdiff_t>(size));
		Position += static_cast<size_t>(size);
		Out.push_back(std::move(field));
	}
	return true;
}

std::vector<SKeyField> DecodeKeyBlob (std::span<const uint8> Blob)
{
	std::vector<SKeyField> fields;
	size_t position = 0u;
	EXPECT_TRUE(DecodeRecord(Blob, position, {}, fields));
	EXPECT_EQ(position, Blob.size());
	return fields;
}

// "<path> <type>" per field, in blob order.
std::vector<FString> DescribeFields (const std::vector<SKeyField>& Fields)
{
	std::vector<FString> lines;
	for (const SKeyField& field : Fields)
	{
		lines.push_back(field.Path + " " + GetTypeName(field.Type));
	}
	return lines;
}

const SKeyField* FindField (const std::vector<SKeyField>& Fields, FStringView Path)
{
	for (const SKeyField& field : Fields)
	{
		if (field.Path == Path)
		{
			return &field;
		}
	}
	return nullptr;
}

FString GetStrValue (const std::vector<SKeyField>& Fields, FStringView Path)
{
	const SKeyField* field = FindField(Fields, Path);
	if (!field)
	{
		return "<missing>";
	}
	const char* text = reinterpret_cast<const char*>(field->Value.data());
	return FString(FStringView(text, static_cast<uint32>(field->Value.size())));
}


/**
 * A shader root with First.hlsl, which includes A.hlsli, and Sub/Second.hlsl, which includes nothing,
 * and a DDC beside it, in a directory with a non-ASCII name of this process's own.
 */
class ShaderCacheTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const FString name = FString::Format("FRT ShaderCache Тест {}", GetCurrentProcessId());
		Dir = std::filesystem::temp_directory_path() / name.ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		Root = Dir / FStringView("Шейдери").ToPath();
		WriteShaders(Root);
		FString error;
		ASSERT_TRUE(Ddc.Open(Dir / FStringView("DDC").ToPath(), &error)) << error.CStr();
	}

	void TearDown () override
	{
		Ddc.Close();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
	}

	static void WriteShaders (const std::filesystem::path& ShaderRoot)
	{
		ASSERT_TRUE(platform::WriteFileText(ShaderRoot / FStringView("A.hlsli").ToPath(), IncludeText));
		ASSERT_TRUE(platform::WriteFileText(ShaderRoot / FStringView("First.hlsl").ToPath(), FirstText));
		ASSERT_TRUE(platform::WriteFileText(ShaderRoot / FStringView("Sub/Second.hlsl").ToPath(), SecondText));
	}

	static SShaderCompileDesc MakeDesc (const std::filesystem::path& ShaderRoot, FStringView RelativePath)
	{
		SShaderCompileDesc desc;
		desc.SourcePath = ShaderRoot / RelativePath.ToPath();
		desc.EntryPoint = "main";
		desc.Profile = "cs_6_6";
		return desc;
	}

	SShaderCompileDesc First () const { return MakeDesc(Root, "First.hlsl"); }
	SShaderCompileDesc Second () const { return MakeDesc(Root, "Sub/Second.hlsl"); }

	// A cache as a new process would make one: its own file-hash memo, the store given.
	void InitCache (CShaderCache& Cache, CShaderCompiler& Compiler, float VerifyPercent = 0.0f)
	{
		SShaderCacheSettings settings;
		settings.Cache = &Ddc;
		settings.VerifyPercent = VerifyPercent;
		Cache.Init(Compiler, settings);
	}

	std::filesystem::path Dir;
	std::filesystem::path Root;
	assets::CDerivedDataCache Ddc;
};
}


TEST_F(ShaderCacheTest, TheFirstPassCompilesAndTheSecondReadsBackTheSameBytes)
{
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));

	CShaderCache cold;
	InitCache(cold, compiler);
	const SShaderBinary firstCold = cold.Compile(First());
	const SShaderBinary secondCold = cold.Compile(Second());
	ASSERT_TRUE(firstCold.bSuccess) << firstCold.Log;
	ASSERT_TRUE(secondCold.bSuccess) << secondCold.Log;
	SShaderCacheCounters counters = cold.GetCounters();
	EXPECT_EQ(counters.Hits, 0u);
	EXPECT_EQ(counters.Misses, 2u);
	EXPECT_EQ(counters.Compiled, 2u);
	EXPECT_EQ(counters.NotStored, 0u);

	CShaderCache warm;
	InitCache(warm, compiler);
	const SShaderBinary firstWarm = warm.Compile(First());
	const SShaderBinary secondWarm = warm.Compile(Second());
	ASSERT_TRUE(firstWarm.bSuccess);
	ASSERT_TRUE(secondWarm.bSuccess);
	counters = warm.GetCounters();
	EXPECT_EQ(counters.Hits, 2u);
	EXPECT_EQ(counters.Compiled, 0u);
	const FString line = CShaderCache::FormatCounters(counters);
	EXPECT_TRUE(line.StartsWith("2 hits, 0 compiled (")) << line;

	// The cached DXIL is the compiled DXIL, and a compile now gives it again.
	EXPECT_EQ(GetBytes(firstWarm), GetBytes(firstCold));
	EXPECT_EQ(GetBytes(secondWarm), GetBytes(secondCold));
	EXPECT_EQ(GetBytes(firstWarm), GetBytes(compiler.Compile(First())));
	EXPECT_NE(GetBytes(firstWarm), GetBytes(secondWarm));

	// A hit watches what the compile watched: the source and the include, for hot reload.
	ASSERT_EQ(firstWarm.Dependencies.size(), 2u);
	ASSERT_EQ(firstCold.Dependencies.size(), 2u);
	std::error_code ec;
	EXPECT_TRUE(std::filesystem::equivalent(firstWarm.Dependencies[0], firstCold.Dependencies[0], ec));
	EXPECT_TRUE(std::filesystem::equivalent(firstWarm.Dependencies[1], Root / FStringView("A.hlsli").ToPath(), ec));
	EXPECT_EQ(secondWarm.Dependencies.size(), 1u);
}

TEST_F(ShaderCacheTest, EditingAnIncludeMissesExactlyItsIncluderAndRevertingItHitsAgain)
{
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	// One cache throughout, as hot reload uses it: its file-hash memo must notice every edit.
	CShaderCache cache;
	InitCache(cache, compiler);

	const std::vector<uint8> original = GetBytes(cache.Compile(First()));
	ASSERT_FALSE(original.empty());
	ASSERT_TRUE(cache.Compile(Second()).bSuccess);
	cache.ResetCounters();

	const std::filesystem::path include = Root / FStringView("A.hlsli").ToPath();
	ASSERT_TRUE(platform::WriteFileText(include, EditedIncludeText));
	const SShaderBinary edited = cache.Compile(First());
	ASSERT_TRUE(edited.bSuccess) << edited.Log;
	ASSERT_TRUE(cache.Compile(Second()).bSuccess);
	SShaderCacheCounters counters = cache.GetCounters();
	EXPECT_EQ(counters.Misses, 1u);
	EXPECT_EQ(counters.Compiled, 1u);
	EXPECT_EQ(counters.Hits, 1u);
	EXPECT_NE(GetBytes(edited), original);

	// Back to the first version: its input set is still in the manifest, behind the edited one.
	cache.ResetCounters();
	ASSERT_TRUE(platform::WriteFileText(include, IncludeText));
	const SShaderBinary reverted = cache.Compile(First());
	ASSERT_TRUE(reverted.bSuccess);
	counters = cache.GetCounters();
	EXPECT_EQ(counters.Hits, 1u);
	EXPECT_EQ(counters.Compiled, 0u);
	EXPECT_EQ(GetBytes(reverted), original);

	const std::optional<SShaderCacheKey> key = cache.MakeKey(First());
	ASSERT_TRUE(key.has_value());
	EXPECT_EQ(Ddc.ReadManifest(key->StaticKey).size(), 2u);
}

TEST_F(ShaderCacheTest, AHeaderCreatedWhereAnIncludeWasLookedForMisses)
{
	// Sub/Third.hlsl includes "B.hlsli", which DXC looks for under the root first and finds in Sub/.
	ASSERT_TRUE(platform::WriteFileText(Root / FStringView("Sub/B.hlsli").ToPath(),
		"static const uint ValueB = 5u;\n"));
	ASSERT_TRUE(platform::WriteFileText(Root / FStringView("Sub/Third.hlsl").ToPath(),
		"#include \"B.hlsli\"\n"
		"RWStructuredBuffer<uint> Output : register(u0);\n"
		"[numthreads(1, 1, 1)]\n"
		"void main () { Output[0] = ValueB; }\n"));
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	CShaderCache cache;
	InitCache(cache, compiler);
	const SShaderCompileDesc third = MakeDesc(Root, "Sub/Third.hlsl");
	const SShaderBinary original = cache.Compile(third);
	ASSERT_TRUE(original.bSuccess) << original.Log;
	EXPECT_FALSE(original.FilesMissing.empty());

	// A B.hlsli at the root now shadows Sub/B.hlsli: the program must be compiled again, not served.
	const std::filesystem::path shadow = Root / FStringView("B.hlsli").ToPath();
	ASSERT_TRUE(platform::WriteFileText(shadow, "static const uint ValueB = 600u;\n"));
	cache.ResetCounters();
	const SShaderBinary shadowed = cache.Compile(third);
	ASSERT_TRUE(shadowed.bSuccess) << shadowed.Log;
	EXPECT_EQ(cache.GetCounters().Misses, 1u);
	EXPECT_EQ(GetBytes(shadowed), GetBytes(compiler.Compile(third)));
	EXPECT_NE(GetBytes(shadowed), GetBytes(original));

	// Gone again: the first input set holds once more.
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::remove(shadow, ec));
	cache.ResetCounters();
	EXPECT_EQ(GetBytes(cache.Compile(third)), GetBytes(original));
	EXPECT_EQ(cache.GetCounters().Hits, 1u);
}

/**
 * Sub/Third.hlsl includes "B.hlsli", which DXC looks for under the root before Sub/. Something at the
 * root that cannot be read - a directory, a file another process holds without read sharing (an
 * editor saving it) - is passed over, and Sub/B.hlsli compiled instead; but it shadows Sub/B.hlsli as
 * soon as it is a readable file. Such a compile is not stored: its input set would name Sub/B.hlsli
 * alone and go on matching after the root's header can be read, serving DXIL a compile no longer gives.
 */
class ShaderCacheUnreadableIncludeTest : public ShaderCacheTest
{
protected:
	void SetUp () override
	{
		ShaderCacheTest::SetUp();
		if (HasFatalFailure())
		{
			return;
		}
		ASSERT_TRUE(platform::WriteFileText(Root / FStringView("Sub/B.hlsli").ToPath(),
			"static const uint ValueB = 5u;\n"));
		ASSERT_TRUE(platform::WriteFileText(Root / FStringView("Sub/Third.hlsl").ToPath(),
			"#include \"B.hlsli\"\n"
			"RWStructuredBuffer<uint> Output : register(u0);\n"
			"[numthreads(1, 1, 1)]\n"
			"void main () { Output[0] = ValueB; }\n"));
		Shadow = Root / FStringView("B.hlsli").ToPath();
		ASSERT_TRUE(Compiler.Init(Root));
		InitCache(Cache, Compiler);
	}

	// Compiles Third while the root's B.hlsli cannot be read: Sub/B.hlsli's program, not stored.
	std::vector<uint8> CompilePassingOver ()
	{
		Cache.ResetCounters();
		const SShaderBinary passedOver = Cache.Compile(Third());
		EXPECT_TRUE(passedOver.bSuccess) << passedOver.Log;
		EXPECT_EQ(Cache.GetCounters().Misses, 1u);
		EXPECT_EQ(Cache.GetCounters().NotStored, 1u);
		EXPECT_FALSE(passedOver.bInputsStable);
		return GetBytes(passedOver);
	}

	// With the root's B.hlsli readable at last: the program a compile gives now, never the stored one.
	void ExpectTheShadowCompiled (const std::vector<uint8>& PassedOver)
	{
		ASSERT_TRUE(platform::WriteFileText(Shadow, "static const uint ValueB = 600u;\n"));
		Cache.ResetCounters();
		const SShaderBinary shadowed = Cache.Compile(Third());
		ASSERT_TRUE(shadowed.bSuccess) << shadowed.Log;
		EXPECT_EQ(Cache.GetCounters().Hits, 0u);
		EXPECT_EQ(GetBytes(shadowed), GetBytes(Compiler.Compile(Third())));
		EXPECT_NE(GetBytes(shadowed), PassedOver);
	}

	SShaderCompileDesc Third () const { return MakeDesc(Root, "Sub/Third.hlsl"); }

	std::filesystem::path Shadow;
	CShaderCompiler Compiler;
	CShaderCache Cache;
};

TEST_F(ShaderCacheUnreadableIncludeTest, ADirectoryWhereAnIncludeWasLookedForKeepsTheCompileUncached)
{
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directory(Shadow, ec));
	const std::vector<uint8> passedOver = CompilePassingOver();
	EXPECT_EQ(Ddc.GetStats().Records.Files, 0u);

	ASSERT_TRUE(std::filesystem::remove(Shadow, ec));
	ExpectTheShadowCompiled(passedOver);
}

TEST_F(ShaderCacheUnreadableIncludeTest, AnIncludeHeldWithoutReadSharingKeepsTheCompileUncached)
{
	// Held as an editor in the middle of a save may hold it: the file is there, and cannot be opened.
	ASSERT_TRUE(platform::WriteFileText(Shadow, "static const uint ValueB = 600u;\n"));
	const HANDLE held = CreateFileW(Shadow.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(held, INVALID_HANDLE_VALUE);
	const std::vector<uint8> passedOver = CompilePassingOver();
	CloseHandle(held);

	ExpectTheShadowCompiled(passedOver);
}

TEST_F(ShaderCacheTest, TheWorkingDirectoryNeverChangesWhatIsIncluded)
{
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	const std::vector<uint8> expected = GetBytes(compiler.Compile(First()));
	ASSERT_FALSE(expected.empty());

	// An A.hlsli of another value in the working directory: DXC asks for "./A.hlsli".
	const std::filesystem::path elsewhere = Dir / FStringView("Деінде").ToPath();
	ASSERT_TRUE(platform::WriteFileText(elsewhere / FStringView("A.hlsli").ToPath(), EditedIncludeText));
	std::error_code ec;
	const std::filesystem::path saved = std::filesystem::current_path(ec);
	std::filesystem::current_path(elsewhere, ec);
	ASSERT_FALSE(ec);
	const SShaderBinary compiled = compiler.Compile(First());
	std::filesystem::current_path(saved, ec);
	ASSERT_TRUE(compiled.bSuccess) << compiled.Log;
	EXPECT_EQ(GetBytes(compiled), expected);
	ASSERT_EQ(compiled.Dependencies.size(), 2u);
	EXPECT_TRUE(std::filesystem::equivalent(compiled.Dependencies[1], Root / FStringView("A.hlsli").ToPath(), ec));
}

TEST_F(ShaderCacheTest, TheDebugInformationVariantNeverHitsAcrossShaderRoots)
{
	// Another checkout: the same files under another root.
	const std::filesystem::path otherRoot = Dir / FStringView("Інший корінь").ToPath();
	WriteShaders(otherRoot);
	const auto passOver = [this] (const std::filesystem::path& ShaderRoot, bool bDebugInfo)
	{
		CShaderCompiler compiler;
		EXPECT_TRUE(compiler.Init(ShaderRoot, bDebugInfo));
		CShaderCache cache;
		InitCache(cache, compiler);
		EXPECT_TRUE(cache.Compile(MakeDesc(ShaderRoot, "First.hlsl")).bSuccess);
		return cache.GetCounters();
	};

	// With debug information, DXIL names the files of its own root: the other root compiles its own.
	EXPECT_EQ(passOver(Root, true).Compiled, 1u);
	EXPECT_EQ(passOver(otherRoot, true).Compiled, 1u);
	EXPECT_EQ(passOver(otherRoot, true).Hits, 1u);

	// Without, keys hold no path: the other root hits what the first one stored.
	EXPECT_EQ(passOver(Root, false).Compiled, 1u);
	EXPECT_EQ(passOver(otherRoot, false).Hits, 1u);

	// The debug variant stays local (bDeterministic false), the other may be shared.
	const auto recordOf = [this] (const std::filesystem::path& ShaderRoot, bool bDebugInfo)
	{
		CShaderCompiler compiler;
		EXPECT_TRUE(compiler.Init(ShaderRoot, bDebugInfo));
		CShaderCache cache;
		InitCache(cache, compiler);
		const std::optional<SShaderCacheKey> key = cache.MakeKey(MakeDesc(ShaderRoot, "First.hlsl"));
		EXPECT_TRUE(key.has_value());
		return key ? Ddc.Get(ShaderCacheBucket, key->StaticKey, [] (const assets::SDdcInput&) { return true; },
			assets::EDdcRead::MetadataOnly) : std::nullopt;
	};
	const std::optional<assets::SDdcRecord> debugRecord = recordOf(Root, true);
	const std::optional<assets::SDdcRecord> plainRecord = recordOf(Root, false);
	ASSERT_TRUE(debugRecord.has_value());
	ASSERT_TRUE(plainRecord.has_value());
	EXPECT_FALSE(debugRecord->bDeterministic);
	EXPECT_TRUE(plainRecord->bDeterministic);
	EXPECT_EQ(debugRecord->Builder, ShaderDxilBuilder);
	EXPECT_EQ(debugRecord->Version, GetShaderDxilBuilderVersion());
	EXPECT_NE(debugRecord->FindValue("Dxil"), nullptr);
}

TEST_F(ShaderCacheTest, AFailedCompileWritesNothingAndCompilesAgainNextTime)
{
	ASSERT_TRUE(platform::WriteFileText(Root / FStringView("A.hlsli").ToPath(), "static const uint ValueA = ;\n"));
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	CShaderCache cache;
	InitCache(cache, compiler);

	EXPECT_FALSE(cache.Compile(First()).bSuccess);
	EXPECT_FALSE(cache.Compile(First()).bSuccess);
	const SShaderCacheCounters counters = cache.GetCounters();
	EXPECT_EQ(counters.Compiled, 2u);
	EXPECT_EQ(counters.Failed, 2u);
	EXPECT_EQ(counters.Hits, 0u);
	const assets::SDdcStats stats = Ddc.GetStats();
	EXPECT_EQ(stats.Records.Files, 0u);
	EXPECT_EQ(stats.Manifests.Files, 0u);
}

TEST_F(ShaderCacheTest, WithoutAStoreEveryProgramIsCompiled)
{
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	CShaderCache cache;
	cache.Init(compiler, SShaderCacheSettings{});   // -Ddc:Shaders=0, or Release
	EXPECT_FALSE(cache.IsCaching());
	EXPECT_TRUE(cache.Compile(First()).bSuccess);
	EXPECT_TRUE(cache.Compile(First()).bSuccess);
	const SShaderCacheCounters counters = cache.GetCounters();
	EXPECT_EQ(counters.Compiled, 2u);
	EXPECT_EQ(counters.Uncached, 2u);
	EXPECT_EQ(counters.Misses, 0u);
	EXPECT_EQ(Ddc.GetStats().Records.Files, 0u);
}

TEST_F(ShaderCacheTest, ASourceOutsideTheShaderRootIsCompiledWithoutTheCache)
{
	const std::filesystem::path outside = Dir / FStringView("Outside").ToPath();
	WriteShaders(outside);
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	CShaderCache cache;
	InitCache(cache, compiler);

	FString whyNot;
	EXPECT_FALSE(cache.MakeKey(MakeDesc(outside, "Sub/Second.hlsl"), &whyNot).has_value());
	EXPECT_TRUE(whyNot.Contains("outside the shader root")) << whyNot.CStr();
	EXPECT_TRUE(cache.Compile(MakeDesc(outside, "Sub/Second.hlsl")).bSuccess);
	EXPECT_EQ(cache.GetCounters().Uncached, 1u);
	EXPECT_EQ(Ddc.GetStats().Records.Files, 0u);
}

TEST_F(ShaderCacheTest, VerifyCompilesHitsAgainAndReportsDifferentBytesAsBuild002)
{
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root));
	{
		CShaderCache cold;
		InitCache(cold, compiler);
		ASSERT_TRUE(cold.Compile(First()).bSuccess);
		ASSERT_TRUE(cold.Compile(Second()).bSuccess);
	}

	// Every hit verified, none different.
	{
		CShaderCache verifying;
		InitCache(verifying, compiler, 100.0f);
		EXPECT_TRUE(verifying.Compile(First()).bSuccess);
		EXPECT_TRUE(verifying.Compile(Second()).bSuccess);
		const SShaderCacheCounters counters = verifying.GetCounters();
		EXPECT_EQ(counters.Hits, 2u);
		EXPECT_EQ(counters.VerifyCompiles, 2u);
		EXPECT_EQ(counters.VerifyMismatches, 0u);
	}

	// A record holding other bytes under First's key: what a nondeterministic builder or a key
	// missing an input would leave behind.
	const SShaderBinary compiled = compiler.Compile(First());
	ASSERT_TRUE(compiled.bSuccess);
	CShaderCache verifying;
	InitCache(verifying, compiler, 100.0f);
	const std::optional<SShaderCacheKey> key = verifying.MakeKey(First());
	ASSERT_TRUE(key.has_value());
	const std::vector<assets::SDdcManifestMatch> sets = Ddc.ReadManifest(key->StaticKey);
	ASSERT_EQ(sets.size(), 1u);
	assets::SDdcRecord bogus;
	bogus.Builder = ShaderDxilBuilder;
	bogus.Version = GetShaderDxilBuilderVersion();
	bogus.Values.push_back({ "Dxil", {}, 0u, false, std::vector<uint8>(64u, uint8{ 0xAB }) });
	ASSERT_TRUE(Ddc.PutRecord(ShaderCacheBucket, sets[0].Key, bogus));

	test::CLogCapture capture;
	const SShaderBinary verified = verifying.Compile(First());
	ASSERT_TRUE(verified.bSuccess);
	EXPECT_EQ(GetBytes(verified), GetBytes(compiled));
	EXPECT_EQ(verifying.GetCounters().VerifyMismatches, 1u);
	bool bReported = false;
	for (const test::CLogCapture::SEntry& entry : capture.GetEntries())
	{
		bReported |= entry.Category == "shader" && entry.Level == log::ELevel::Error
			&& entry.Text.StartsWith("BUILD002 First.hlsl:main");
	}
	EXPECT_TRUE(bReported);

	// The fresh DXIL replaced the bad record.
	CShaderCache warm;
	InitCache(warm, compiler);
	EXPECT_EQ(GetBytes(warm.Compile(First())), GetBytes(compiled));
	EXPECT_EQ(warm.GetCounters().Hits, 1u);
}

// The Shader.Dxil KeyBlob, field by field (design §7.4: a golden key-schema test per builder). A change
// here is a change of what the keys mean: it comes with a new GUID in GetShaderDxilBuilderVersion
// (ShaderCache.cpp) and in ExpectedVersion below, or old records would be served under new meanings.
TEST_F(ShaderCacheTest, TheShaderDxilKeySchemaIsFrozen)
{
	constexpr FStringLiteral ExpectedVersion = "5abfc33aa94748d1b5110d8ee7928913";
	EXPECT_EQ(GetShaderDxilBuilderVersion().ToString(), ExpectedVersion);

	// Every field present: an entry point, defines, extra arguments, debug information.
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root, true));
	CShaderCache cache;
	InitCache(cache, compiler);
	SShaderCompileDesc desc = Second();
	desc.Defines = { { "ZETA", "2" }, { "ALPHA" } };
	desc.ExtraArguments = { "-Wno-unused-value" };
	const std::optional<SShaderCacheKey> key = cache.MakeKey(desc);
	ASSERT_TRUE(key.has_value());
	EXPECT_EQ(key->StaticKey, assets::MakeStaticKey(key->KeyBlob));
	const std::vector<SKeyField> fields = DecodeKeyBlob(key->KeyBlob);

	// (field, type), in the blob's order: ascending byte order of the names at every level.
	const std::vector<FString> expected = {
		"BuildSystem Guid",
		"Builder Str",
		"Compiler Record",
		"Compiler.Dxcompiler Hash160",      // always: without all three hashes there is no key
		"Compiler.Dxil Hash160",
		"Compiler.Name Str",
		"Compiler.VersionText Hash160",
		"Discriminators Record",            // baseline: absent (no debug information)
		"Discriminators.DebugInfoRoot Hash160",
		"Settings Record",
		"Settings.Defines Record",          // baseline: absent (no defines)
		"Settings.Defines.ALPHA Str",
		"Settings.Defines.ZETA Str",
		"Settings.Entry Str",               // baseline: absent (a library)
		"Settings.ExtraArguments Record",   // baseline: absent (none)
		"Settings.ExtraArguments.0000 Str",
		"Settings.FixedArguments Record",
		"Settings.FixedArguments.0000 Str",
		"Settings.FixedArguments.0001 Str",
		"Settings.FixedArguments.0002 Str",
		"Settings.FixedArguments.0003 Str",
		"Settings.FixedArguments.0004 Str",
		"Settings.FixedArguments.0005 Str",
		"Settings.FixedArguments.0006 Str",
		"Settings.FixedArguments.0007 Str",
		"Settings.FixedArguments.0008 Str",
		"Settings.FixedArguments.0009 Str",
		"Settings.FixedArguments.0010 Str",
		"Settings.FixedArguments.0011 Str",
		"Settings.Profile Str",
		"Source Record",
		"Source.Hash Hash160",
		"Source.Size U64",
		"Target Record",
		"Target.Rhi Str",
		"Version Guid",
	};
	EXPECT_EQ(DescribeFields(fields), expected);

	EXPECT_EQ(GetStrValue(fields, "Builder"), "Shader.Dxil");
	EXPECT_EQ(GetStrValue(fields, "Compiler.Name"), "DXC");
	EXPECT_EQ(GetStrValue(fields, "Target.Rhi"), "D3D12");
	EXPECT_EQ(GetStrValue(fields, "Settings.Entry"), "main");
	EXPECT_EQ(GetStrValue(fields, "Settings.Profile"), "cs_6_6");
	EXPECT_EQ(GetStrValue(fields, "Settings.Defines.ALPHA"), "1");
	EXPECT_EQ(GetStrValue(fields, "Settings.Defines.ZETA"), "2");
	EXPECT_EQ(GetStrValue(fields, "Settings.ExtraArguments.0000"), "-Wno-unused-value");
	// The command line with mount paths for directories: no absolute path anywhere in the key.
	const std::vector<FString> fixedArguments = { "Second.hlsl", "-HV", "2021", "-Zpc", "-O3", "-enable-16bit-types",
		"-I", "/Engine/Shaders", "-I", "/Engine/Shaders/Sub", "-Zi", "-Qembed_debug" };
	for (size_t i = 0u; i < fixedArguments.size(); ++i)
	{
		EXPECT_EQ(GetStrValue(fields, FString::Format("Settings.FixedArguments.{:04}", i)), fixedArguments[i]);
	}
	EXPECT_FALSE(Contains(key->KeyBlob, FString::FromPath(Dir).AsBytes()));
	EXPECT_FALSE(Contains(key->KeyBlob, FString::FromPath(Root.filename()).AsBytes()));
	const SKeyField* version = FindField(fields, "Version");
	ASSERT_NE(version, nullptr);
	const assets::SGuid builderVersion = GetShaderDxilBuilderVersion();
	EXPECT_TRUE(std::equal(version->Value.begin(), version->Value.end(), std::begin(builderVersion.Bytes)));

	// The baselines: a library without defines, extra arguments or debug information omits exactly
	// the fields marked so above.
	CShaderCompiler plainCompiler;
	ASSERT_TRUE(plainCompiler.Init(Root, false));
	CShaderCache plainCache;
	InitCache(plainCache, plainCompiler);
	SShaderCompileDesc library = Second();
	library.EntryPoint = FString();
	library.Profile = "lib_6_6";
	const std::optional<SShaderCacheKey> plainKey = plainCache.MakeKey(library);
	ASSERT_TRUE(plainKey.has_value());
	std::vector<FString> plainFields;
	for (const FString& line : DescribeFields(DecodeKeyBlob(plainKey->KeyBlob)))
	{
		if (!line.StartsWith("Settings.FixedArguments."))
		{
			plainFields.push_back(line);
		}
	}
	const std::vector<FString> expectedPlain = {
		"BuildSystem Guid",
		"Builder Str",
		"Compiler Record",
		"Compiler.Dxcompiler Hash160",
		"Compiler.Dxil Hash160",
		"Compiler.Name Str",
		"Compiler.VersionText Hash160",
		"Settings Record",
		"Settings.FixedArguments Record",
		"Settings.Profile Str",
		"Source Record",
		"Source.Hash Hash160",
		"Source.Size U64",
		"Target Record",
		"Target.Rhi Str",
		"Version Guid",
	};
	EXPECT_EQ(plainFields, expectedPlain);
}

TEST_F(ShaderCacheTest, DefinesInAnyOrderShareAKeyAndEveryKeyInputChangesIt)
{
	// With debug information, in every configuration: its DXIL embeds the command line, so it is the
	// variant in which the order of the defines could show.
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root, true));
	CShaderCache cache;
	InitCache(cache, compiler);
	const auto staticKeyOf = [&cache] (const SShaderCompileDesc& Desc)
	{
		const std::optional<SShaderCacheKey> key = cache.MakeKey(Desc);
		EXPECT_TRUE(key.has_value());
		return key ? key->StaticKey : hash::SContentHash{};
	};

	SShaderCompileDesc ab = First();
	ab.Defines = { { "A", "1" }, { "B", "2" } };
	SShaderCompileDesc ba = First();
	ba.Defines = { { "B", "2" }, { "A", "1" } };
	EXPECT_EQ(staticKeyOf(ab), staticKeyOf(ba));
	// One key, one program: the compiler passes the defines in name order, as the key records them,
	// so DXC sees one command line for both orders and embeds the same one.
	const SShaderBinary abCompiled = compiler.Compile(ab);
	const SShaderBinary baCompiled = compiler.Compile(ba);
	ASSERT_TRUE(abCompiled.bSuccess) << abCompiled.Log;
	ASSERT_TRUE(baCompiled.bSuccess) << baCompiled.Log;
	EXPECT_EQ(GetBytes(abCompiled), GetBytes(baCompiled));

	SShaderCompileDesc otherValue = ab;
	otherValue.Defines[1].Value = "3";
	SShaderCompileDesc otherEntry = First();
	otherEntry.EntryPoint = "other";
	SShaderCompileDesc otherProfile = First();
	otherProfile.Profile = "cs_6_8";
	SShaderCompileDesc extra = First();
	extra.ExtraArguments = { "-Wno-unused-value" };
	const hash::SContentHash base = staticKeyOf(First());
	EXPECT_NE(staticKeyOf(ab), base);
	EXPECT_NE(staticKeyOf(otherValue), staticKeyOf(ab));
	EXPECT_NE(staticKeyOf(otherEntry), base);
	EXPECT_NE(staticKeyOf(otherProfile), base);
	EXPECT_NE(staticKeyOf(extra), base);
	EXPECT_NE(staticKeyOf(Second()), base);

	// A define named twice has no key: compiled without the cache rather than keyed ambiguously.
	SShaderCompileDesc twice = First();
	twice.Defines = { { "A", "1" }, { "A", "2" } };
	FString whyNot;
	EXPECT_FALSE(cache.MakeKey(twice, &whyNot).has_value());
	EXPECT_FALSE(whyNot.IsEmpty());

	// The source's content is in the key: an edit of First.hlsl itself changes it.
	ASSERT_TRUE(platform::WriteFileText(Root / FStringView("First.hlsl").ToPath(), FString(FirstText) + "// edited\n"));
	EXPECT_NE(staticKeyOf(First()), base);
}

TEST_F(ShaderCacheTest, AHitForDefinesInAnotherOrderIsTheDxilACompileOfThatOrderGives)
{
	CShaderCompiler compiler;
	ASSERT_TRUE(compiler.Init(Root, true));
	SShaderCompileDesc ab = First();
	ab.Defines = { { "A", "1" }, { "B", "2" } };
	SShaderCompileDesc ba = First();
	ba.Defines = { { "B", "2" }, { "A", "1" } };
	{
		CShaderCache cold;
		InitCache(cold, compiler);
		ASSERT_TRUE(cold.Compile(ab).bSuccess);
	}

	// The other order hits what the first stored, and -Ddc:Verify finds it identical to a compile.
	CShaderCache verifying;
	InitCache(verifying, compiler, 100.0f);
	const SShaderBinary hit = verifying.Compile(ba);
	ASSERT_TRUE(hit.bSuccess) << hit.Log;
	const SShaderCacheCounters counters = verifying.GetCounters();
	EXPECT_EQ(counters.Hits, 1u);
	EXPECT_EQ(counters.VerifyCompiles, 1u);
	EXPECT_EQ(counters.VerifyMismatches, 0u);
	EXPECT_EQ(GetBytes(hit), GetBytes(compiler.Compile(ba)));
}
