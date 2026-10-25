#include "Graphics/Render/RHI/ShaderCache.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "Assets/BuildSystem.h"
#include "Assets/KeyWriter.h"
#include "Log/Log.h"


namespace frt::graphics::rhi
{
namespace
{
constexpr FStringLiteral LogCategory = "shader";
// The values of a Shader.Dxil record, and the kind of its discovered inputs.
constexpr FStringLiteral DxilValueName = "Dxil";
constexpr FStringLiteral LogValueName = "Log";
constexpr FStringLiteral FileInputKind = "File";
constexpr FStringLiteral AbsentInputKind = "Absent";
// Lists are records of four-digit indices (see the header), so a list may hold this many items.
constexpr size_t MaxKeyListLength = 10000u;

// Microseconds since Start, on the monotonic clock.
uint64 MicrosecondsSince (std::chrono::steady_clock::time_point Start)
{
	return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - Start).count());
}

// A list in a KeyBlob: a record whose fields are named by four-digit indices, so they sort in list
// order. False (nothing written) for a list too long to number so.
bool WriteKeyList (assets::CKeyWriter& Key, FStringView Name, const std::vector<FString>& Items)
{
	if (Items.size() > MaxKeyListLength)
	{
		return false;
	}
	Key.BeginRecord(Name);
	for (size_t i = 0u; i < Items.size(); ++i)
	{
		Key.WriteStr(FString::Format("{:04}", i), Items[i]);
	}
	Key.EndRecord();
	return true;
}

// "SkyBake.hlsl:BakeEnvironment (cs_6_6)", how messages name a program.
FString DescribeProgram (const SShaderCompileDesc& Desc)
{
	FString text = FString::FromPath(Desc.SourcePath.filename());
	if (!Desc.EntryPoint.IsEmpty())
	{
		text << ':' << Desc.EntryPoint;
	}
	text << " (" << Desc.Profile << ')';
	return text;
}

std::span<const uint8> GetBlobBytes (IDxcBlob* Blob)
{
	return Blob ? std::span<const uint8>(static_cast<const uint8*>(Blob->GetBufferPointer()), Blob->GetBufferSize())
				: std::span<const uint8>();
}

// What a compile prints for its warnings, printed again for a hit that recorded some.
void PrintWarnings (const SShaderCompileDesc& Desc, FStringView Log)
{
	const FString name = FString::FromPath(Desc.SourcePath.filename());
	std::fprintf(stdout, "[shader] warnings in %s:\n%.*s\n", name.CStr(), static_cast<int>(Log.Len()), Log.GetData());
}
}


assets::SGuid GetShaderDxilBuilderVersion () noexcept
{
	// A new GUID (FRT.bat assets newguid) whenever a change to ShaderCompiler.* or ShaderCache.* may
	// change the DXIL of a key or what a record means: arguments, the reading of sources, the KeyBlob.
	return assets::SGuid { { 0xa9, 0x0e, 0xfe, 0xc4, 0xd5, 0x9d, 0x4f, 0xda, 0xa2, 0x9b, 0xb9, 0xe0, 0x0d, 0xd6, 0x63,
		0x84 } };
}


void CShaderCache::Init (CShaderCompiler& InCompiler, const SShaderCacheSettings& InSettings)
{
	Compiler = &InCompiler;
	Settings = InSettings;
	if (Settings.Cache && !Settings.Cache->IsOpen())
	{
		Settings.Cache = nullptr;
	}
	std::lock_guard lock(MemoMutex);
	Memo.clear();
}

SShaderCacheCounters CShaderCache::GetCounters () const noexcept
{
	SShaderCacheCounters counters;
	counters.Hits = Counters.Hits.load(std::memory_order_relaxed);
	counters.Misses = Counters.Misses.load(std::memory_order_relaxed);
	counters.Compiled = Counters.Compiled.load(std::memory_order_relaxed);
	counters.Uncached = Counters.Uncached.load(std::memory_order_relaxed);
	counters.Failed = Counters.Failed.load(std::memory_order_relaxed);
	counters.NotStored = Counters.NotStored.load(std::memory_order_relaxed);
	counters.VerifyCompiles = Counters.VerifyCompiles.load(std::memory_order_relaxed);
	counters.VerifyMismatches = Counters.VerifyMismatches.load(std::memory_order_relaxed);
	counters.Microseconds = Counters.Microseconds.load(std::memory_order_relaxed);
	return counters;
}

void CShaderCache::ResetCounters () noexcept
{
	Counters.Hits = 0u;
	Counters.Misses = 0u;
	Counters.Compiled = 0u;
	Counters.Uncached = 0u;
	Counters.Failed = 0u;
	Counters.NotStored = 0u;
	Counters.VerifyCompiles = 0u;
	Counters.VerifyMismatches = 0u;
	Counters.Microseconds = 0u;
}

FString CShaderCache::FormatCounters (const SShaderCacheCounters& Counters)
{
	return FString::Format("{} hits, {} compiled ({} ms)", Counters.Hits, Counters.Compiled,
		Counters.Microseconds / 1000u);
}

SShaderBinary CShaderCache::Compile (const SShaderCompileDesc& Desc)
{
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	SShaderBinary binary = Lookup(Desc);
	Counters.Microseconds.fetch_add(MicrosecondsSince(start), std::memory_order_relaxed);
	return binary;
}

SShaderBinary CShaderCache::Lookup (const SShaderCompileDesc& Desc)
{
	if (!Settings.Cache)
	{
		return CompileUncached(Desc);
	}

	FString whyNot;
	const std::optional<SShaderCacheKey> key = MakeKey(Desc, &whyNot);
	if (!key)
	{
		log::Verbose(LogCategory, "{}: compiled without the cache: {}", DescribeProgram(Desc), whyNot);
		return CompileUncached(Desc);
	}

	assets::SDdcManifestMatch match;
	const auto isCurrent = [this] (const assets::SDdcInput& Recorded) { return IsInputCurrent(Recorded); };
	const std::optional<assets::SDdcRecord> record = Settings.Cache->Get(ShaderCacheBucket, key->StaticKey, isCurrent,
		assets::EDdcRead::Values, &match);
	if (record && record->Builder == ShaderDxilBuilder)
	{
		const assets::SDdcValue* dxil = record->FindValue(DxilValueName);
		const assets::SDdcValue* logValue = record->FindValue(LogValueName);
		const FStringView log = logValue ? FStringView(reinterpret_cast<const char*>(logValue->Bytes.data()),
			static_cast<uint32>(logValue->Bytes.size())) : FStringView();
		std::optional<SShaderBinary> hit = dxil ? MakeHit(*key, dxil->Bytes, log, match.Inputs) : std::nullopt;
		if (hit)
		{
			Counters.Hits.fetch_add(1u, std::memory_order_relaxed);
			if (IsVerified(match.Key))
			{
				return VerifyHit(Desc, *key, std::move(*hit), match, DescribeProgram(Desc));
			}
			if (!hit->Log.IsEmpty())
			{
				PrintWarnings(Desc, hit->Log);
			}
			return std::move(*hit);
		}
	}

	Counters.Misses.fetch_add(1u, std::memory_order_relaxed);
	return CompileAndStore(Desc, *key);
}

SShaderBinary CShaderCache::CompileUncached (const SShaderCompileDesc& Desc)
{
	SShaderBinary binary = Compiler->Compile(Desc);
	Counters.Compiled.fetch_add(1u, std::memory_order_relaxed);
	Counters.Uncached.fetch_add(1u, std::memory_order_relaxed);
	if (!binary.bSuccess)
	{
		Counters.Failed.fetch_add(1u, std::memory_order_relaxed);
	}
	return binary;
}

SShaderBinary CShaderCache::CompileAndStore (const SShaderCompileDesc& Desc, const SShaderCacheKey& Key)
{
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	SShaderBinary binary = Compiler->Compile(Desc);
	Counters.Compiled.fetch_add(1u, std::memory_order_relaxed);
	if (!binary.bSuccess)
	{
		// A failed step writes nothing (§7.9): the next attempt compiles again.
		Counters.Failed.fetch_add(1u, std::memory_order_relaxed);
		return binary;
	}
	Store(Key, binary, MicrosecondsSince(start) / 1000u, DescribeProgram(Desc));
	return binary;
}

std::optional<std::vector<assets::SDdcInput>> CShaderCache::MakeInputs (const SShaderBinary& Binary) const
{
	std::vector<assets::SDdcInput> inputs;
	inputs.reserve(Binary.FilesRead.size() + Binary.FilesMissing.size());
	for (size_t i = 1u; i < Binary.FilesRead.size(); ++i)
	{
		const SShaderFileRead& read = Binary.FilesRead[i];
		std::optional<FString> mountPath = ToMountPath(read.Path);
		if (!mountPath)
		{
			return std::nullopt;
		}
		inputs.push_back({ FString(FileInputKind), std::move(*mountPath), read.Hash, read.Size });
	}
	for (const std::filesystem::path& missing : Binary.FilesMissing)
	{
		std::optional<FString> mountPath = ToMountPath(missing);
		if (!mountPath)
		{
			return std::nullopt;
		}
		inputs.push_back({ FString(AbsentInputKind), std::move(*mountPath), hash::SContentHash{}, 0u });
	}
	return inputs;
}

bool CShaderCache::Store (const SShaderCacheKey& Key, const SShaderBinary& Binary, uint64 BuildMs, FStringView Label)
{
	// Only what the key describes may be stored under it: the source the key hashed, every file read
	// once, every include nameable without a path.
	const bool bSameSource = !Binary.FilesRead.empty() && Binary.FilesRead.front().Hash == Key.Source.Hash
		&& Binary.FilesRead.front().Size == Key.Source.Size;
	const std::optional<std::vector<assets::SDdcInput>> inputs = bSameSource && Binary.bInputsStable
		? MakeInputs(Binary) : std::nullopt;
	if (!inputs)
	{
		Counters.NotStored.fetch_add(1u, std::memory_order_relaxed);
		log::Info(LogCategory, "{}: not cached: {}", Label, !bSameSource || !Binary.bInputsStable
			? "a source changed while it was compiled" : "it looks for an include outside the shader root");
		return false;
	}

	assets::SDdcRecord record;
	record.Builder = ShaderDxilBuilder;
	record.Version = GetShaderDxilBuilderVersion();
	const std::span<const uint8> dxil = GetBlobBytes(Binary.Blob.Get());
	record.Values.push_back({ FString(DxilValueName), {}, 0u, false, std::vector<uint8>(dxil.begin(), dxil.end()) });
	if (!Binary.Log.IsEmpty())
	{
		const std::span<const uint8> log = Binary.Log.AsBytes();
		record.Values.push_back({ FString(LogValueName), {}, 0u, false, std::vector<uint8>(log.begin(), log.end()) });
		record.LogHash = hash::HashContent(log);
	}
	record.BuildMs = BuildMs;
	// Debug information embeds the sources' absolute paths: this machine's, this checkout's (§14.5).
	record.bDeterministic = !Compiler->HasDebugInfo();

	FString error;
	if (!Settings.Cache->Put(ShaderCacheBucket, Key.StaticKey, *inputs, record, &error))
	{
		Counters.NotStored.fetch_add(1u, std::memory_order_relaxed);
		log::Warning(LogCategory, "{}: not cached: {}", Label, error);
		return false;
	}
	return true;
}

SShaderBinary CShaderCache::VerifyHit (const SShaderCompileDesc& Desc, const SShaderCacheKey& Key, SShaderBinary Hit,
	const assets::SDdcManifestMatch& Match, FStringView Label)
{
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	SShaderBinary fresh = Compiler->Compile(Desc);
	Counters.VerifyCompiles.fetch_add(1u, std::memory_order_relaxed);

	// Only a compile of the very sources the hit was keyed on compares: a file saved in between, or a
	// compile that failed, proves nothing about the cache.
	const bool bSameSource = !fresh.FilesRead.empty() && fresh.FilesRead.front().Hash == Key.Source.Hash
		&& fresh.FilesRead.front().Size == Key.Source.Size;
	const std::optional<std::vector<assets::SDdcInput>> inputs = fresh.bSuccess && fresh.bInputsStable && bSameSource
		? MakeInputs(fresh) : std::nullopt;
	if (!inputs || assets::CDerivedDataCache::NormalizeInputs(*inputs)
		!= assets::CDerivedDataCache::NormalizeInputs(Match.Inputs))
	{
		log::Info(LogCategory, "{}: not verified: its sources changed, or the compile failed", Label);
		return Hit;
	}

	const std::span<const uint8> cached = GetBlobBytes(Hit.Blob.Get());
	const std::span<const uint8> compiled = GetBlobBytes(fresh.Blob.Get());
	if (cached.size() == compiled.size() && std::memcmp(cached.data(), compiled.data(), cached.size()) == 0)
	{
		return Hit;
	}

	Counters.VerifyMismatches.fetch_add(1u, std::memory_order_relaxed);
	log::Error(LogCategory, "BUILD002 {}: the cached DXIL ({} bytes, key {}) differs from a fresh compile ({} bytes): "
		"the fresh one is used and stored", Label, cached.size(), Match.Key.ToString(), compiled.size());
	Store(Key, fresh, MicrosecondsSince(start) / 1000u, Label);
	return fresh;
}

std::optional<SShaderBinary> CShaderCache::MakeHit (const SShaderCacheKey& Key, const std::vector<uint8>& Dxil,
	FStringView Log, const std::vector<assets::SDdcInput>& Inputs) const
{
	SShaderBinary binary;
	binary.Dependencies.reserve(Inputs.size() + 1u);
	binary.Dependencies.push_back(Key.Source.Path);
	for (const assets::SDdcInput& input : Inputs)
	{
		std::optional<std::filesystem::path> path = FromMountPath(input.Name);
		if (!path)
		{
			return std::nullopt;
		}
		// What a compile reports: the files it opened (hot reload watches them), not the places it found
		// empty.
		if (input.Kind == FileInputKind)
		{
			binary.Dependencies.push_back(std::move(*path));
		}
	}
	if (Dxil.empty())
	{
		return std::nullopt;
	}
	binary.Blob = Compiler->CreateBlob(Dxil);
	if (!binary.Blob)
	{
		return std::nullopt;
	}
	binary.Log = Log;
	binary.bSuccess = true;
	return binary;
}

bool CShaderCache::IsVerified (const hash::SContentHash& Key) const noexcept
{
	if (Settings.VerifyPercent <= 0.0f)
	{
		return false;
	}
	if (Settings.VerifyPercent >= 100.0f)
	{
		return true;
	}
	// The Key's first bytes are uniform: their value in hundredths of a percent picks the same share of
	// programs, and the same programs, on every run.
	const uint32 value = static_cast<uint32>(Key.Bytes[0]) | (static_cast<uint32>(Key.Bytes[1]) << 8u)
		| (static_cast<uint32>(Key.Bytes[2]) << 16u) | (static_cast<uint32>(Key.Bytes[3]) << 24u);
	return static_cast<float>(value % 10000u) < Settings.VerifyPercent * 100.0f;
}

std::optional<SShaderCacheKey> CShaderCache::MakeKey (const SShaderCompileDesc& Desc, FString* OutWhyNot) const
{
	const auto fail = [OutWhyNot] (FString Why) -> std::optional<SShaderCacheKey>
	{
		if (OutWhyNot)
		{
			*OutWhyNot = std::move(Why);
		}
		return std::nullopt;
	};

	std::error_code ec;
	const std::filesystem::path sourcePath = std::filesystem::weakly_canonical(Desc.SourcePath, ec);
	const std::optional<FString> sourceDir = ToMountPath(sourcePath.parent_path());
	if (!sourceDir || !ToMountPath(sourcePath))
	{
		return fail(FString::FromPath(sourcePath) + " is outside the shader root "
			+ FString::FromPath(Compiler->GetShaderRoot()));
	}
	const std::optional<SShaderFileRead> source = HashCurrentFile(sourcePath);
	if (!source)
	{
		return fail("cannot read " + FString::FromPath(sourcePath));
	}

	assets::CKeyWriter key;
	key.WriteStr("Builder", ShaderDxilBuilder);
	key.WriteGuid("Version", GetShaderDxilBuilderVersion());
	key.WriteGuid("BuildSystem", assets::BuildSystemVersion);

	key.BeginRecord("Target");
	key.WriteStr("Rhi", "D3D12");
	key.EndRecord();

	const SDxcIdentity& dxc = CShaderCompiler::GetDxcIdentity();
	key.BeginRecord("Compiler");
	key.WriteStr("Name", "DXC");
	if (dxc.Dxcompiler.IsValid())
	{
		key.WriteHash160("Dxcompiler", dxc.Dxcompiler);
	}
	if (dxc.Dxil.IsValid())
	{
		key.WriteHash160("Dxil", dxc.Dxil);
	}
	if (dxc.VersionText.IsValid())
	{
		key.WriteHash160("VersionText", dxc.VersionText);
	}
	key.EndRecord();

	key.BeginRecord("Settings");
	if (!Desc.EntryPoint.IsEmpty())
	{
		key.WriteStr("Entry", Desc.EntryPoint);
	}
	key.WriteStr("Profile", Desc.Profile);
	if (!Desc.Defines.empty())
	{
		key.BeginRecord("Defines");
		for (const SShaderDefine& define : Desc.Defines)
		{
			key.WriteStr(define.Name, define.Value);
		}
		key.EndRecord();
	}
	const std::vector<FString> fixedArguments = Compiler->MakeFixedArguments(FString::FromPath(sourcePath.filename()),
		Desc.Profile, Settings.RootMountPath, *sourceDir);
	if ((!Desc.ExtraArguments.empty() && !WriteKeyList(key, "ExtraArguments", Desc.ExtraArguments))
		|| !WriteKeyList(key, "FixedArguments", fixedArguments))
	{
		return fail("too many arguments");
	}
	key.EndRecord();

	key.BeginRecord("Source");
	key.WriteHash160("Hash", source->Hash);
	key.WriteU64("Size", source->Size);
	key.EndRecord();

	if (Compiler->HasDebugInfo())
	{
		key.BeginRecord("Discriminators");
		key.WriteHash160("DebugInfoRoot", hash::HashContent(FString::FromPath(Compiler->GetShaderRoot()).AsBytes()));
		key.EndRecord();
	}

	FString error;
	std::optional<std::vector<uint8>> blob = key.Finish(&error);
	if (!blob)
	{
		return fail("key: " + error);
	}
	SShaderCacheKey result;
	result.KeyBlob = std::move(*blob);
	result.StaticKey = assets::MakeStaticKey(result.KeyBlob);
	result.Source = *source;
	return result;
}

std::optional<FString> CShaderCache::ToMountPath (const std::filesystem::path& Path) const
{
	const std::filesystem::path relative = Path.lexically_relative(Compiler->GetShaderRoot());
	if (relative.empty())
	{
		return std::nullopt;
	}
	FString mountPath = Settings.RootMountPath;
	if (relative == std::filesystem::path(FStringView(".").ToPath()))
	{
		return mountPath;
	}
	for (const std::filesystem::path& part : relative)
	{
		if (part == std::filesystem::path(FStringView("..").ToPath()))
		{
			return std::nullopt;
		}
	}
	mountPath << '/' << FString::FromPath(relative, EPathStyle::Generic);
	return mountPath;
}

std::optional<std::filesystem::path> CShaderCache::FromMountPath (FStringView MountPath) const
{
	if (!MountPath.StartsWith(Settings.RootMountPath))
	{
		return std::nullopt;
	}
	FStringView rest = MountPath.RightChop(Settings.RootMountPath.Len());
	if (!rest.StartsWith('/'))
	{
		return std::nullopt;
	}
	rest.RightChopInline(1u);
	std::filesystem::path path = Compiler->GetShaderRoot();
	while (!rest.IsEmpty())
	{
		const SStringSplit split = rest.SplitFirst('/');
		const FStringView part = split ? split.Left : rest;
		if (part.IsEmpty() || part == "." || part == "..")
		{
			return std::nullopt;
		}
		path /= part.ToPath();
		rest = split ? split.Right : FStringView();
		if (split && rest.IsEmpty())
		{
			return std::nullopt;   // a trailing '/'
		}
	}
	return path;
}

bool CShaderCache::IsInputCurrent (const assets::SDdcInput& Recorded) const
{
	const std::optional<std::filesystem::path> path = FromMountPath(Recorded.Name);
	if (!path)
	{
		return false;
	}
	if (Recorded.Kind == AbsentInputKind)
	{
		// Still nothing there - not even a directory, which DXC would fail to open instead of looking on.
		return !platform::Stat(*path).has_value();
	}
	if (Recorded.Kind != FileInputKind)
	{
		return false;
	}
	const std::optional<SShaderFileRead> current = HashCurrentFile(*path);
	return current && current->Hash == Recorded.Hash && current->Size == Recorded.Size;
}

std::optional<SShaderFileRead> CShaderCache::HashCurrentFile (const std::filesystem::path& Path) const
{
	const std::optional<platform::SFileStat> before = platform::Stat(Path);
	if (!before || before->bDirectory)
	{
		return std::nullopt;
	}
	const auto sameFile = [] (const platform::SFileStat& A, const platform::SFileStat& B)
	{
		return A.Size == B.Size && A.LastWriteTime == B.LastWriteTime && A.Identity == B.Identity && !A.bDirectory
			&& !B.bDirectory;
	};
	{
		std::lock_guard lock(MemoMutex);
		if (const auto it = Memo.find(Path); it != Memo.end() && sameFile(it->second.Stat, *before))
		{
			return SShaderFileRead{ Path, it->second.Hash, before->Size };
		}
	}

	hash::CContentHasher hasher;
	uint64 size = 0u;
	const auto onChunk = [&hasher, &size] (std::span<const uint8> Bytes)
	{
		hasher.Update(Bytes);
		size += Bytes.size();
	};
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, onChunk);
	if (read != platform::EStableRead::Stable)
	{
		return std::nullopt;
	}
	const hash::SContentHash contentHash = hasher.Finalize();
	// Remembered only when the file is still the one the first stat saw: the read checked stats of its
	// own, which a save between that stat and the read would have moved on.
	const std::optional<platform::SFileStat> after = platform::Stat(Path);
	if (after && sameFile(*after, *before) && size == before->Size)
	{
		std::lock_guard lock(MemoMutex);
		Memo.insert_or_assign(Path, SFileHashMemo{ *before, contentHash });
	}
	return SShaderFileRead{ Path, contentHash, size };
}
}
