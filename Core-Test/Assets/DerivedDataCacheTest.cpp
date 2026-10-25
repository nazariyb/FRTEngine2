// The derived-data cache (Assets/DerivedDataCache.h): records, manifests and values round-trip in the
// documented layout; small values are inlined and large ones are separate content-addressed files at
// exactly the documented thresholds; the two-step lookup finds the key of the input set that is still
// current; and no damage ever turns into a bad hit - a zero-filled record or a truncated value after
// a simulated power loss, a flipped bit in a value, two processes writing the same keys at once are
// all misses or one valid record, and the damaged files are deleted - while a file that merely cannot
// be read now (another program holding it) is never judged: not FORMAT, not a record, not a value.
// Garbage collection keeps what is pinned and evicts the least recently used first, and keeps a value
// a writer just reused; Verify finds every corrupt file. Manifest updates in one process lose nothing;
// closing the cache under a running call asserts. ComputeKey's bytes and output are frozen here:
// every cached artifact on every machine is named by them.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include "Assets/DerivedDataCache.h"
#include "Config/Config.h"
#include "Containers/String.h"
#include "Containers/WideString.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Project/UserRegistry.h"

#include <Windows.h>

using namespace frt;
using namespace frt::assets;
using namespace std::chrono_literals;


namespace
{
hash::SContentHash FilledHash (uint8 Byte)
{
	hash::SContentHash value;
	std::memset(value.Bytes, Byte, sizeof(value.Bytes));
	return value;
}

hash::SContentHash CountingHash ()
{
	hash::SContentHash value;
	for (uint32 i = 0u; i < hash::SContentHash::Size; ++i)
	{
		value.Bytes[i] = static_cast<uint8>(i + 1u);
	}
	return value;
}

// A distinct StaticKey per number, as a builder's KeyBlob would give.
hash::SContentHash MakeStaticKey (uint32 Number)
{
	const FString text = FString::Format("static key {}", Number);
	return hash::HashContent(text.AsBytes());
}

// Bytes that do not compress (xorshift): their stored form is their raw form.
std::vector<uint8> RandomBytes (size_t Size, uint32 Seed)
{
	std::vector<uint8> bytes(Size);
	uint32 state = Seed * 2654435761u + 0x9E3779B9u;
	for (uint8& byte : bytes)
	{
		state ^= state << 13u;
		state ^= state >> 17u;
		state ^= state << 5u;
		byte = static_cast<uint8>(state >> 24u);
	}
	return bytes;
}

// Bytes that compress very well.
std::vector<uint8> RepetitiveBytes (size_t Size, uint8 Seed)
{
	std::vector<uint8> bytes(Size);
	for (size_t i = 0u; i < Size; ++i)
	{
		bytes[i] = static_cast<uint8>(Seed + (i / 64u) % 4u);
	}
	return bytes;
}

SDdcValue MakeValue (FStringView Name, std::vector<uint8> Bytes)
{
	SDdcValue value;
	value.Name = Name;
	value.Bytes = std::move(Bytes);
	return value;
}

SDdcRecord MakeRecord (std::vector<SDdcValue> Values)
{
	SDdcRecord record;
	record.Builder = "Test.Builder";
	record.Version = SGuid::Parse("9fc0d4010bbf28b4594072e72b8655ab").value_or(SGuid());
	record.Values = std::move(Values);
	record.LogHash = FilledHash(0x5Au);
	record.BuildMs = 1234u;
	record.bDeterministic = true;
	return record;
}

SDdcInput MakeInput (FStringView Kind, FStringView Name, uint8 HashByte, uint64 Size)
{
	return SDdcInput{ FString(Kind), FString(Name), FilledHash(HashByte), Size };
}

std::vector<uint8> ReadBytes (const std::filesystem::path& Path)
{
	std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path);
	return bytes ? std::move(*bytes) : std::vector<uint8>();
}

// Every file below Directory.
std::vector<std::filesystem::path> ListFiles (const std::filesystem::path& Directory)
{
	std::vector<std::filesystem::path> files;
	std::error_code ec;
	for (std::filesystem::recursive_directory_iterator it(Directory, ec), end; !ec && it != end; it.increment(ec))
	{
		if (it->is_regular_file(ec))
		{
			files.push_back(it->path());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

size_t CountFiles (const std::filesystem::path& Directory)
{
	return ListFiles(Directory).size();
}

// The last write time a file had Age ago.
void AgeFile (const std::filesystem::path& Path, std::chrono::hours Age)
{
	std::error_code ec;
	std::filesystem::last_write_time(Path, std::filesystem::file_time_type::clock::now() - Age, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
}

int64 GetWriteTime (const std::filesystem::path& Path)
{
	const std::optional<platform::SFileStat> stat = platform::Stat(Path);
	return stat ? stat->LastWriteTime : 0;
}

constexpr int64 HourFileTicks = 3600ll * 10'000'000ll;

// A file another program (a virus scanner, a backup) holds open for reading while sharing only
// ShareMode: without FILE_SHARE_READ nobody else can read it until it lets go.
class CHeldFile
{
public:
	CHeldFile (const std::filesystem::path& Path, DWORD ShareMode)
		: Handle(CreateFileW(Path.c_str(), GENERIC_READ, ShareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
	{}

	~CHeldFile ()
	{
		if (Handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(Handle);
		}
	}

	CHeldFile (const CHeldFile&) = delete;
	CHeldFile& operator= (const CHeldFile&) = delete;

	[[nodiscard]] bool IsValid () const noexcept { return Handle != INVALID_HANDLE_VALUE; }

private:
	HANDLE Handle;
};

void ExpectSameValues (const SDdcRecord& Actual, const SDdcRecord& Expected)
{
	ASSERT_EQ(Actual.Values.size(), Expected.Values.size());
	for (size_t i = 0u; i < Expected.Values.size(); ++i)
	{
		EXPECT_EQ(Actual.Values[i].Name, Expected.Values[i].Name);
		EXPECT_EQ(Actual.Values[i].Bytes, Expected.Values[i].Bytes) << Expected.Values[i].Name.CStr();
		EXPECT_EQ(Actual.Values[i].RawSize, Expected.Values[i].Bytes.size());
		EXPECT_EQ(Actual.Values[i].RawHash, hash::HashContent(Expected.Values[i].Bytes));
	}
}


// ---------------- The concurrent writer child ----------------

// What the ddc-writer children write for key number I: a StaticKey, one discovered input, and a
// record with an inline value and a large external one - all a pure function of I, so both children
// write identical bytes and the parent knows what to expect.
constexpr uint32 SharedKeyCount = 24u;

hash::SContentHash SharedStaticKey (uint32 I)
{
	return MakeStaticKey(1000u + I);
}

std::vector<SDdcInput> SharedInputs (uint32 I)
{
	return { MakeInput("File", FString::Format("/Shared/Include{}.h", I % 5u), static_cast<uint8>(I), 100u + I) };
}

SDdcRecord SharedRecord (uint32 I)
{
	return MakeRecord({ MakeValue("Small", RandomBytes(100u + I, I)), MakeValue("Large", RandomBytes(200u * 1024u + I * 7u, 500u + I)) });
}

// "Core-Test --frt-test-child=ddc-writer <root> <rounds>": writes the shared keys again and again.
int RunDdcWriter (std::span<const FString> Arguments)
{
	if (Arguments.size() < 2u)
	{
		return 2;
	}
	CDerivedDataCache ddc;
	FString error;
	if (!ddc.Open(Arguments[0].ToPath(), &error))
	{
		return 3;
	}
	const uint32 rounds = Arguments[1].View().TryParse<uint32>().value_or(1u);
	for (uint32 round = 0u; round < rounds; ++round)
	{
		for (uint32 i = 0u; i < SharedKeyCount; ++i)
		{
			if (!ddc.Put("Shared", SharedStaticKey(i), SharedInputs(i), SharedRecord(i), &error))
			{
				std::fprintf(stderr, "ddc-writer: %s\n", error.CStr());
				return 4;
			}
		}
	}
	return 0;
}

const bool bDdcWriterRegistered = test::RegisterChildMode("ddc-writer", &RunDdcWriter);


// Every test has a store of its own in a directory with a non-ASCII name, its own per-user data
// directory, and no FRT_DDC_DIR of the developer's.
class DerivedDataCacheTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Кеш 缓存 😀 {}", GetCurrentProcessId()).ToPath()
			/ FStringView(test->name()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		SavedUserDataDir = platform::GetEnvironmentPath("FRT_USER_DATA_DIR");
		SavedDdcDir = platform::GetEnvironmentPath("FRT_DDC_DIR");
		platform::SetEnvironmentPath("FRT_USER_DATA_DIR", Dir / "User");
		platform::SetEnvironmentPath("FRT_DDC_DIR", {});

		Root = Dir / FStringView("Сховище DDC").ToPath();
		FString error;
		ASSERT_TRUE(Ddc.Open(Root, &error)) << error.CStr();
	}

	void TearDown () override
	{
		Ddc.Close();
		platform::SetEnvironmentPath("FRT_USER_DATA_DIR", SavedUserDataDir);
		platform::SetEnvironmentPath("FRT_DDC_DIR", SavedDdcDir);
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	std::filesystem::path Dir;
	std::filesystem::path Root;
	std::filesystem::path SavedUserDataDir;
	std::filesystem::path SavedDdcDir;
	CDerivedDataCache Ddc;
};
}


// ---------------- Keys ----------------

// The key material, encoded by hand from the format in DerivedDataCache.h, and the key it gives.
TEST(DerivedDataCacheKeyTest, ComputeKeyMatchesTheGoldenBytes)
{
	const std::vector<SDdcInput> inputs =
	{
		MakeInput("File", "/Shaders/b.hlsli", 0x11u, 300u),
		MakeInput("Config", "/Config/Engine.ini", 0x22u, 5u),
	};
	std::vector<uint8> golden =
	{
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,   // StaticKey
		0x02,                                                                                       // two inputs, sorted by name
		0x12, '/', 'C', 'o', 'n', 'f', 'i', 'g', '/', 'E', 'n', 'g', 'i', 'n', 'e', '.', 'i', 'n', 'i',   // Name
		0x06, 'C', 'o', 'n', 'f', 'i', 'g',                                                         // Kind
	};
	golden.insert(golden.end(), 20u, uint8(0x22u));                                                 // Hash
	const uint8 size5[] = { 0x05, 0, 0, 0, 0, 0, 0, 0 };                                            // Size
	golden.insert(golden.end(), std::begin(size5), std::end(size5));
	const uint8 second[] = { 0x10, '/', 'S', 'h', 'a', 'd', 'e', 'r', 's', '/', 'b', '.', 'h', 'l', 's', 'l', 'i', 0x04, 'F', 'i', 'l', 'e' };
	golden.insert(golden.end(), std::begin(second), std::end(second));
	golden.insert(golden.end(), 20u, uint8(0x11u));
	const uint8 size300[] = { 0x2C, 0x01, 0, 0, 0, 0, 0, 0 };
	golden.insert(golden.end(), std::begin(size300), std::end(size300));

	EXPECT_EQ(CDerivedDataCache::EncodeKeyMaterial(CountingHash(), inputs), golden);
	const hash::SContentHash key = CDerivedDataCache::ComputeKey(CountingHash(), inputs);
	EXPECT_EQ(key, hash::DeriveKey("FRT DDC key v1", golden));
	EXPECT_EQ(key.ToString(), "27d27ea9d4b934d305a6c6fd79d13f981de918f9");

	// No inputs: the StaticKey and a zero count.
	const hash::SContentHash counting = CountingHash();
	std::vector<uint8> empty(std::begin(counting.Bytes), std::end(counting.Bytes));
	empty.push_back(0u);
	EXPECT_EQ(CDerivedDataCache::EncodeKeyMaterial(CountingHash(), {}), empty);
}

TEST(DerivedDataCacheKeyTest, InputOrderAndDuplicatesDoNotChangeTheKey)
{
	const SDdcInput a = MakeInput("File", "/A.h", 1u, 10u);
	const SDdcInput b = MakeInput("File", "/B.h", 2u, 20u);
	const SDdcInput c = MakeInput("Config", "/B.h", 3u, 30u);   // same name, another kind
	const hash::SContentHash staticKey = MakeStaticKey(1u);
	const std::vector<SDdcInput> forward = { a, b, c };
	const std::vector<SDdcInput> backward = { c, b, a, b };
	EXPECT_EQ(CDerivedDataCache::ComputeKey(staticKey, forward), CDerivedDataCache::ComputeKey(staticKey, backward));
	EXPECT_EQ(CDerivedDataCache::NormalizeInputs(backward), (std::vector<SDdcInput>{ a, c, b }));   // name, then kind

	// Every field counts, and the length prefixes keep ("ab", "c") apart from ("a", "bc").
	const hash::SContentHash base = CDerivedDataCache::ComputeKey(staticKey, forward);
	EXPECT_NE(CDerivedDataCache::ComputeKey(MakeStaticKey(2u), forward), base);
	EXPECT_NE(CDerivedDataCache::ComputeKey(staticKey, std::vector<SDdcInput>{ a, b }), base);
	EXPECT_NE(CDerivedDataCache::ComputeKey(staticKey, std::vector<SDdcInput>{ a, b, MakeInput("Config", "/B.h", 3u, 31u) }), base);
	EXPECT_NE(CDerivedDataCache::ComputeKey(staticKey, std::vector<SDdcInput>{ MakeInput("ab", "c", 1u, 1u) }),
		CDerivedDataCache::ComputeKey(staticKey, std::vector<SDdcInput>{ MakeInput("a", "bc", 1u, 1u) }));
	// Not a plain content hash of the material: domain-separated by the context.
	EXPECT_NE(base, hash::HashContent(CDerivedDataCache::EncodeKeyMaterial(staticKey, forward)));
}


// ---------------- Opening ----------------

TEST_F(DerivedDataCacheTest, OpenWritesTheFormatAndRefusesAnotherOne)
{
	EXPECT_EQ(platform::ReadFileText(Root / "FORMAT").value_or("<missing>"), "FRT-DDC 1\n");
	Ddc.Close();

	// A FORMAT a power loss left zero-filled is written again.
	ASSERT_TRUE(platform::WriteFileBytes(Root / "FORMAT", std::vector<uint8>(10u, 0u)));
	FString error;
	ASSERT_TRUE(Ddc.Open(Root, &error)) << error.CStr();
	EXPECT_EQ(platform::ReadFileText(Root / "FORMAT").value_or("<missing>"), "FRT-DDC 1\n");
	Ddc.Close();

	// Another engine's store is refused and left alone.
	ASSERT_TRUE(platform::WriteFileText(Root / "FORMAT", "FRT-DDC 2\n"));
	EXPECT_FALSE(Ddc.Open(Root, &error));
	EXPECT_TRUE(error.Contains("FRT-DDC 2")) << error.CStr();
	EXPECT_FALSE(Ddc.IsOpen());
	EXPECT_EQ(platform::ReadFileText(Root / "FORMAT").value_or("<missing>"), "FRT-DDC 2\n");
}

// A FORMAT that is there but cannot be read now may name another engine's format: Open fails and
// leaves it alone. The holder shares delete, so writing over it would have gone through.
TEST_F(DerivedDataCacheTest, AFormatThatCannotBeReadIsNeverWrittenOver)
{
	Ddc.Close();
	ASSERT_TRUE(platform::WriteFileText(Root / "FORMAT", "FRT-DDC 2\n"));
	FString error;
	{
		const CHeldFile held(Root / "FORMAT", FILE_SHARE_DELETE);
		ASSERT_TRUE(held.IsValid());
		EXPECT_FALSE(Ddc.Open(Root, &error));
		EXPECT_TRUE(error.Contains("FORMAT")) << error.CStr();
		EXPECT_FALSE(Ddc.IsOpen());
	}
	EXPECT_EQ(platform::ReadFileText(Root / "FORMAT").value_or("<missing>"), "FRT-DDC 2\n");

	// This engine's own store opens again once its FORMAT can be read.
	ASSERT_TRUE(platform::WriteFileText(Root / "FORMAT", "FRT-DDC 1\n"));
	ASSERT_TRUE(Ddc.Open(Root, &error)) << error.CStr();
}

// Manifests and records carry the revision FORMAT names, not a version of their own: a layout change
// is a new FORMAT, which an engine of the other revision refuses, so two engine revisions never share
// one store and never delete each other's files. Inside this store another revision is damage.
TEST_F(DerivedDataCacheTest, ManifestsAndRecordsCarryTheFormatRevision)
{
	const hash::SContentHash staticKey = MakeStaticKey(1u);
	FString error;
	const std::optional<hash::SContentHash> key = Ddc.Put("Test", staticKey, std::vector<SDdcInput>{ MakeInput("File", "/A.h", 1u, 1u) },
		MakeRecord({ MakeValue("Small", RandomBytes(100u, 1u)) }), &error);
	ASSERT_TRUE(key.has_value()) << error.CStr();
	EXPECT_EQ(platform::ReadFileText(Root / "FORMAT").value_or("<missing>"), "FRT-DDC 1\n");
	for (const std::filesystem::path& path : { Ddc.GetManifestPath(staticKey), Ddc.GetRecordPath("Test", *key) })
	{
		const std::vector<uint8> bytes = ReadBytes(path);
		ASSERT_GE(bytes.size(), 8u);
		EXPECT_EQ(std::vector<uint8>(bytes.begin() + 4, bytes.begin() + 8), (std::vector<uint8>{ 1u, 0u, 0u, 0u }))
			<< FString::FromPath(path).CStr();
	}

	// A record of revision 2 with a valid checksum: never written by an engine that opened this store.
	const std::filesystem::path record = Ddc.GetRecordPath("Test", *key);
	std::vector<uint8> bytes = ReadBytes(record);
	bytes[4] = 2u;
	const uint64 checksum = hash::Xxh3_64(std::span<const uint8>(bytes).first(bytes.size() - 8u));
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[bytes.size() - 8u + i] = static_cast<uint8>(checksum >> (8u * i));
	}
	ASSERT_TRUE(platform::WriteFileBytes(record, bytes));
	EXPECT_FALSE(Ddc.GetRecord("Test", *key).has_value());
	EXPECT_FALSE(std::filesystem::exists(record));
}

TEST_F(DerivedDataCacheTest, DefaultRootIsTheEnvironmentThenTheConfigThenTheUserDataDir)
{
	EXPECT_EQ(CDerivedDataCache::GetDefaultRoot(), project::GetUserDataDir() / "DerivedDataCache");
	EXPECT_EQ(CDerivedDataCache::GetDefaultRoot(), Dir / "User" / "DerivedDataCache");

	config::CConfig config;
	EXPECT_EQ(CDerivedDataCache::GetDefaultRoot(&config), Dir / "User" / "DerivedDataCache");
	const std::filesystem::path configured = Dir / FStringView("Налаштований 配置").ToPath();
	config.SetOverride("DerivedDataCache", "Path", FString::FromPath(configured));
	EXPECT_EQ(CDerivedDataCache::GetDefaultRoot(&config), configured);
	config.SetOverride("DerivedDataCache", "Path", "Відносний");
	EXPECT_EQ(CDerivedDataCache::GetDefaultRoot(&config), Dir / "User" / FStringView("Відносний").ToPath());

	const std::filesystem::path fromEnvironment = Dir / FStringView("Змінна 环境").ToPath();
	platform::SetEnvironmentPath("FRT_DDC_DIR", fromEnvironment);
	EXPECT_EQ(CDerivedDataCache::GetDefaultRoot(&config), fromEnvironment);

	CDerivedDataCache other;
	FString error;
	ASSERT_TRUE(other.OpenDefault(&config, &error)) << error.CStr();
	EXPECT_TRUE(std::filesystem::exists(fromEnvironment / "FORMAT"));
}


// ---------------- Records and values ----------------

TEST_F(DerivedDataCacheTest, RecordsRoundTripWithTheirValues)
{
	const hash::SContentHash key = MakeStaticKey(7u);
	const SDdcRecord record = MakeRecord({
		MakeValue("Empty", {}),
		MakeValue("Small", RandomBytes(1000u, 1u)),
		MakeValue("Compressible", RepetitiveBytes(300u * 1024u, 3u)),
		MakeValue("Large", RandomBytes(400u * 1024u, 2u)),
	});
	FString error;
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();

	const std::optional<SDdcRecord> read = Ddc.GetRecord("Test", key);
	ASSERT_TRUE(read.has_value());
	EXPECT_EQ(read->Builder, record.Builder);
	EXPECT_EQ(read->Version, record.Version);
	EXPECT_EQ(read->LogHash, record.LogHash);
	EXPECT_EQ(read->BuildMs, record.BuildMs);
	EXPECT_TRUE(read->bDeterministic);
	ExpectSameValues(*read, record);
	ASSERT_NE(read->FindValue("Large"), nullptr);
	EXPECT_FALSE(read->FindValue("Large")->bInline);
	EXPECT_TRUE(read->FindValue("Small")->bInline);
	// 300 KiB of repetition compresses to far under 64 KiB: inline.
	EXPECT_TRUE(read->FindValue("Compressible")->bInline);
	EXPECT_EQ(read->FindValue("Missing"), nullptr);

	// Without values: names, hashes and sizes only.
	const std::optional<SDdcRecord> metadata = Ddc.GetRecord("Test", key, EDdcRead::MetadataOnly);
	ASSERT_TRUE(metadata.has_value());
	ASSERT_EQ(metadata->Values.size(), 4u);
	EXPECT_TRUE(metadata->Values[3].Bytes.empty());
	EXPECT_EQ(metadata->Values[3].RawSize, 400u * 1024u);
	EXPECT_EQ(metadata->Values[3].RawHash, hash::HashContent(record.Values[3].Bytes));

	// The layout: the record under its bucket, the one large value under its raw hash.
	EXPECT_TRUE(std::filesystem::exists(Ddc.GetRecordPath("Test", key)));
	EXPECT_EQ(Ddc.GetRecordPath("Test", key), Root / "Records" / "Test" / key.ToString().Left(2u).ToPath() / (key.ToString() + ".rec").ToPath());
	const hash::SContentHash largeHash = hash::HashContent(record.Values[3].Bytes);
	EXPECT_EQ(Ddc.GetValuePath(largeHash), Root / "Values" / largeHash.ToString().Left(2u).ToPath() / largeHash.ToString().Mid(2u, 2u).ToPath()
		/ (largeHash.ToString() + ".val").ToPath());
	EXPECT_EQ(ListFiles(Root / "Values"), std::vector<std::filesystem::path>{ Ddc.GetValuePath(largeHash) });
	EXPECT_EQ(CountFiles(Root / "Tmp"), 0u);   // every write moved into place

	// A miss is a miss, in any bucket.
	EXPECT_FALSE(Ddc.GetRecord("Test", MakeStaticKey(8u)).has_value());
	EXPECT_FALSE(Ddc.GetRecord("Other", key).has_value());

	const SDdcCounters counters = Ddc.GetCounters();
	EXPECT_EQ(counters.RecordWrites, 1u);
	EXPECT_EQ(counters.ValueWrites, 1u);
	EXPECT_EQ(counters.RecordHits, 2u);
	EXPECT_EQ(counters.RecordMisses, 2u);
	EXPECT_EQ(counters.CorruptFilesDeleted, 0u);

	// Writing it again is idempotent: the content-addressed value is not rewritten.
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	EXPECT_EQ(Ddc.GetCounters().ValueWritesSkipped, 1u);
	ExpectSameValues(Ddc.GetRecord("Test", key).value_or(SDdcRecord()), record);
}

TEST_F(DerivedDataCacheTest, InliningFollowsTheValueAndRecordLimits)
{
	FString error;
	// Exactly 64 KiB stored (incompressible) is inline; one byte more is not.
	const hash::SContentHash atLimit = MakeStaticKey(1u);
	ASSERT_TRUE(Ddc.PutRecord("Test", atLimit, MakeRecord({ MakeValue("V", RandomBytes(64u * 1024u, 1u)) }), &error)) << error.CStr();
	EXPECT_TRUE(Ddc.GetRecord("Test", atLimit, EDdcRead::MetadataOnly).value_or(SDdcRecord()).Values.at(0).bInline);
	const hash::SContentHash overLimit = MakeStaticKey(2u);
	ASSERT_TRUE(Ddc.PutRecord("Test", overLimit, MakeRecord({ MakeValue("V", RandomBytes(64u * 1024u + 1u, 2u)) }), &error)) << error.CStr();
	EXPECT_FALSE(Ddc.GetRecord("Test", overLimit, EDdcRead::MetadataOnly).value_or(SDdcRecord()).Values.at(0).bInline);
	EXPECT_EQ(CountFiles(Root / "Values"), 1u);

	// Five values of 60, 61, 62, 63 and 50 KiB would make a 296 KiB record: the largest (63 KiB)
	// moves out, which leaves 233 KiB inline.
	const hash::SContentHash many = MakeStaticKey(3u);
	const SDdcRecord record = MakeRecord({
		MakeValue("A", RandomBytes(60u * 1024u, 10u)),
		MakeValue("B", RandomBytes(61u * 1024u, 11u)),
		MakeValue("C", RandomBytes(62u * 1024u, 12u)),
		MakeValue("D", RandomBytes(63u * 1024u, 13u)),
		MakeValue("E", RandomBytes(50u * 1024u, 14u)),
	});
	ASSERT_TRUE(Ddc.PutRecord("Test", many, record, &error)) << error.CStr();
	const std::optional<SDdcRecord> read = Ddc.GetRecord("Test", many);
	ASSERT_TRUE(read.has_value());
	ExpectSameValues(*read, record);
	std::vector<bool> inlined;
	for (const SDdcValue& value : read->Values)
	{
		inlined.push_back(value.bInline);
	}
	EXPECT_EQ(inlined, (std::vector<bool>{ true, true, true, false, true }));
	const uint64 recordSize = std::filesystem::file_size(Ddc.GetRecordPath("Test", many));
	EXPECT_LE(recordSize, DdcMaxRecordBytes);
	EXPECT_GT(recordSize, 233u * 1024u);
	EXPECT_EQ(CountFiles(Root / "Values"), 2u);
}

TEST_F(DerivedDataCacheTest, ValuesAreCompressedOnlyWhenThatSavesFivePercent)
{
	FString error;
	const std::vector<uint8> repetitive = RepetitiveBytes(512u * 1024u, 1u);
	const std::vector<uint8> random = RandomBytes(512u * 1024u, 2u);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({ MakeValue("R", repetitive), MakeValue("N", random) }), &error))
		<< error.CStr();

	// Both too large to inline as raw bytes; the repetitive one compresses to a small inline frame,
	// the random one is a raw .val file (codec 0) of exactly header + raw size.
	const std::optional<SDdcRecord> read = Ddc.GetRecord("Test", MakeStaticKey(1u), EDdcRead::MetadataOnly);
	ASSERT_TRUE(read.has_value());
	EXPECT_TRUE(read->Values[0].bInline);
	EXPECT_FALSE(read->Values[1].bInline);
	const std::vector<uint8> file = ReadBytes(Ddc.GetValuePath(hash::HashContent(random)));
	ASSERT_EQ(file.size(), 36u + random.size());
	EXPECT_EQ(std::memcmp(file.data(), "FRTV", 4u), 0);
	EXPECT_EQ(file[4], 0u);
	EXPECT_LT(std::filesystem::file_size(Ddc.GetRecordPath("Test", MakeStaticKey(1u))), 64u * 1024u);

	// A large compressible value outside its record is a zstd .val file (codec 1), smaller than raw.
	const std::vector<uint8> mixed = [] ()
	{
		std::vector<uint8> bytes = RandomBytes(400u * 1024u, 3u);
		const std::vector<uint8> zeros(400u * 1024u, 0u);
		bytes.insert(bytes.end(), zeros.begin(), zeros.end());
		return bytes;
	}();
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(2u), MakeRecord({ MakeValue("M", mixed) }), &error)) << error.CStr();
	const std::vector<uint8> mixedFile = ReadBytes(Ddc.GetValuePath(hash::HashContent(mixed)));
	ASSERT_GT(mixedFile.size(), 36u);
	EXPECT_EQ(mixedFile[4], 1u);
	EXPECT_LT(mixedFile.size(), mixed.size() * 95u / 100u);
	ExpectSameValues(Ddc.GetRecord("Test", MakeStaticKey(2u)).value_or(SDdcRecord()), MakeRecord({ MakeValue("M", mixed) }));
}

TEST_F(DerivedDataCacheTest, PutRefusesInvalidBucketsAndValueNames)
{
	FString error;
	EXPECT_FALSE(Ddc.PutRecord("Bad/Bucket", MakeStaticKey(1u), MakeRecord({}), &error));
	EXPECT_FALSE(error.IsEmpty());
	EXPECT_FALSE(Ddc.PutRecord("..", MakeStaticKey(1u), MakeRecord({}), &error));
	EXPECT_FALSE(Ddc.PutRecord("", MakeStaticKey(1u), MakeRecord({}), &error));
	EXPECT_FALSE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({ MakeValue("", { 1u }) }), &error));
	EXPECT_FALSE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({ MakeValue("A", { 1u }), MakeValue("A", { 2u }) }), &error));
	EXPECT_TRUE(error.Contains("'A'")) << error.CStr();
	EXPECT_EQ(CountFiles(Root / "Records"), 0u);

	EXPECT_TRUE(CDerivedDataCache::IsValidBucket("Shader.Dxil"));
	EXPECT_TRUE(CDerivedDataCache::IsValidBucket("Texture_2-x"));
	EXPECT_FALSE(CDerivedDataCache::IsValidBucket("Translate:Model"));
	EXPECT_FALSE(CDerivedDataCache::IsValidBucket("With space"));
	EXPECT_FALSE(CDerivedDataCache::IsValidBucket("Текстура"));
	EXPECT_FALSE(CDerivedDataCache::IsValidBucket(FString::Repeat("a", 65u)));

	// A record without values is fine.
	EXPECT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({}), &error)) << error.CStr();
	EXPECT_TRUE(Ddc.GetRecord("Test", MakeStaticKey(1u)).has_value());
}


// ---------------- The two-step lookup ----------------

TEST_F(DerivedDataCacheTest, TheManifestGivesTheKeyOfTheCurrentInputSet)
{
	const hash::SContentHash staticKey = MakeStaticKey(42u);
	const std::vector<SDdcInput> inputsV1 = { MakeInput("File", "/Shaders/Common/Math.hlsli", 1u, 100u), MakeInput("File", "/Shaders/A.hlsli", 2u, 50u) };
	const std::vector<SDdcInput> inputsV2 = { MakeInput("File", "/Shaders/Common/Math.hlsli", 3u, 120u), MakeInput("File", "/Shaders/A.hlsli", 2u, 50u) };
	const SDdcRecord recordV1 = MakeRecord({ MakeValue("Dxil", RandomBytes(5000u, 1u)) });
	const SDdcRecord recordV2 = MakeRecord({ MakeValue("Dxil", RandomBytes(6000u, 2u)) });

	FString error;
	const std::optional<hash::SContentHash> keyV1 = Ddc.Put("Shader", staticKey, inputsV1, recordV1, &error);
	ASSERT_TRUE(keyV1.has_value()) << error.CStr();
	EXPECT_EQ(*keyV1, CDerivedDataCache::ComputeKey(staticKey, inputsV1));
	const std::optional<hash::SContentHash> keyV2 = Ddc.Put("Shader", staticKey, inputsV2, recordV2, &error);
	ASSERT_TRUE(keyV2.has_value()) << error.CStr();
	EXPECT_NE(*keyV1, *keyV2);

	// What the files on disk say now: Math.hlsli at version 1 (hash 01..), 2 (hash 03..) or 3 (hash
	// 09.., never built), A.hlsli unchanged.
	uint8 mathVersion = 1u;
	const auto isCurrent = [&mathVersion] (const SDdcInput& Recorded)
	{
		if (Recorded.Name == "/Shaders/Common/Math.hlsli")
		{
			const uint8 hashes[] = { 0u, 1u, 3u, 9u };
			return Recorded.Hash == FilledHash(hashes[mathVersion]);
		}
		return Recorded.Hash == FilledHash(2u);
	};

	SDdcManifestMatch match;
	std::optional<SDdcRecord> hit = Ddc.Get("Shader", staticKey, isCurrent, EDdcRead::Values, &match);
	ASSERT_TRUE(hit.has_value());
	ExpectSameValues(*hit, recordV1);
	EXPECT_EQ(match.Key, *keyV1);
	EXPECT_EQ(match.Inputs, CDerivedDataCache::NormalizeInputs(inputsV1));

	mathVersion = 2u;
	hit = Ddc.Get("Shader", staticKey, isCurrent);
	ASSERT_TRUE(hit.has_value());
	ExpectSameValues(*hit, recordV2);

	// A version nobody built: a miss. Switching back to version 1 is a hit again.
	mathVersion = 3u;
	EXPECT_FALSE(Ddc.FindKey(staticKey, isCurrent).has_value());
	EXPECT_FALSE(Ddc.Get("Shader", staticKey, isCurrent).has_value());
	mathVersion = 1u;
	EXPECT_EQ(Ddc.FindKey(staticKey, isCurrent).value_or(SDdcManifestMatch()).Key, *keyV1);

	// The most recent set comes first.
	const std::vector<SDdcManifestMatch> sets = Ddc.ReadManifest(staticKey);
	ASSERT_EQ(sets.size(), 2u);
	EXPECT_EQ(sets[0].Key, *keyV2);
	EXPECT_EQ(sets[1].Key, *keyV1);
	EXPECT_FALSE(Ddc.FindKey(MakeStaticKey(43u), isCurrent).has_value());

	const SDdcCounters counters = Ddc.GetCounters();
	EXPECT_EQ(counters.ManifestWrites, 2u);
	EXPECT_EQ(counters.ManifestHits, 3u);
	EXPECT_EQ(counters.ManifestMisses, 3u);
}

TEST_F(DerivedDataCacheTest, TheManifestKeepsTheFourMostRecentDistinctInputSets)
{
	const hash::SContentHash staticKey = MakeStaticKey(1u);
	std::vector<hash::SContentHash> keys;
	FString error;
	for (uint8 version = 1u; version <= 6u; ++version)
	{
		const std::vector<SDdcInput> inputs = { MakeInput("File", "/Include.h", version, version) };
		const std::optional<hash::SContentHash> key = Ddc.Put("Test", staticKey, inputs, MakeRecord({}), &error);
		ASSERT_TRUE(key.has_value()) << error.CStr();
		keys.push_back(*key);
	}
	const auto manifestKeys = [&] ()
	{
		std::vector<hash::SContentHash> found;
		for (const SDdcManifestMatch& set : Ddc.ReadManifest(staticKey))
		{
			found.push_back(set.Key);
		}
		return found;
	};
	EXPECT_EQ(manifestKeys(), (std::vector<hash::SContentHash>{ keys[5], keys[4], keys[3], keys[2] }));

	// Building version 4 again moves its set to the front without duplicating it.
	ASSERT_TRUE(Ddc.AddToManifest(staticKey, keys[3], std::vector<SDdcInput>{ MakeInput("File", "/Include.h", 4u, 4u) }, &error));
	EXPECT_EQ(manifestKeys(), (std::vector<hash::SContentHash>{ keys[3], keys[5], keys[4], keys[2] }));
	// A version older than the four is gone: a miss, rebuilt and remembered again.
	const auto isVersion1 = [] (const SDdcInput& Recorded) { return Recorded.Hash == FilledHash(1u); };
	EXPECT_FALSE(Ddc.FindKey(staticKey, isVersion1).has_value());
}

// Worker threads add to manifests at once: updates of one manifest are serialized within the process
// (none is lost), while each thread's own manifest goes on beside the others.
TEST_F(DerivedDataCacheTest, ManifestUpdatesFromManyThreadsLoseNothing)
{
	constexpr uint32 ThreadCount = DdcMaxManifestSets;   // one set each in the shared manifest: all fit
	constexpr uint32 Rounds = 12u;
	const hash::SContentHash shared = MakeStaticKey(100u);
	const auto inputsOf = [] (uint32 Thread, uint32 Round)
	{
		const uint8 hashByte = static_cast<uint8>(Thread * Rounds + Round);
		return std::vector<SDdcInput>{ MakeInput("File", FString::Format("/Include{}.h", Thread), hashByte, Round) };
	};

	std::atomic<uint32> failures = 0u;
	std::vector<std::thread> threads;
	for (uint32 thread = 0u; thread < ThreadCount; ++thread)
	{
		threads.emplace_back([&, thread] ()
		{
			for (uint32 round = 0u; round < Rounds; ++round)
			{
				const std::vector<SDdcInput> inputs = inputsOf(thread, round);
				const hash::SContentHash own = MakeStaticKey(thread);
				if (!Ddc.AddToManifest(own, CDerivedDataCache::ComputeKey(own, inputs), inputs))
				{
					++failures;
				}
				if (round == Rounds / 2u && !Ddc.AddToManifest(shared, CDerivedDataCache::ComputeKey(shared, inputs), inputs))
				{
					++failures;
				}
			}
		});
	}
	for (std::thread& thread : threads)
	{
		thread.join();
	}
	EXPECT_EQ(failures.load(), 0u);

	std::vector<hash::SContentHash> sharedKeys;
	for (const SDdcManifestMatch& set : Ddc.ReadManifest(shared))
	{
		sharedKeys.push_back(set.Key);
	}
	std::vector<hash::SContentHash> expected;
	for (uint32 thread = 0u; thread < ThreadCount; ++thread)
	{
		expected.push_back(CDerivedDataCache::ComputeKey(shared, inputsOf(thread, Rounds / 2u)));
	}
	std::sort(sharedKeys.begin(), sharedKeys.end());
	std::sort(expected.begin(), expected.end());
	EXPECT_EQ(sharedKeys, expected);

	// Each thread's own manifest: its last four rounds, the most recent first.
	for (uint32 thread = 0u; thread < ThreadCount; ++thread)
	{
		const hash::SContentHash own = MakeStaticKey(thread);
		std::vector<hash::SContentHash> keys;
		for (const SDdcManifestMatch& set : Ddc.ReadManifest(own))
		{
			keys.push_back(set.Key);
		}
		std::vector<hash::SContentHash> latest;
		for (uint32 round = Rounds; round > Rounds - DdcMaxManifestSets; --round)
		{
			latest.push_back(CDerivedDataCache::ComputeKey(own, inputsOf(thread, round - 1u)));
		}
		EXPECT_EQ(keys, latest) << "thread " << thread;
	}
	EXPECT_EQ(CountFiles(Root / "Tmp"), 0u);
}


// ---------------- Damage ----------------

// A power loss after the rename but before the data reached the disk leaves a file of the right
// size full of zeros: its checksum fails, so it is a miss, and it is deleted. The next build writes
// it again.
TEST_F(DerivedDataCacheTest, AZeroFilledRecordIsAMissAndIsDeleted)
{
	const hash::SContentHash key = MakeStaticKey(1u);
	const SDdcRecord record = MakeRecord({ MakeValue("Small", RandomBytes(500u, 1u)) });
	FString error;
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	const std::filesystem::path path = Ddc.GetRecordPath("Test", key);
	const uint64 size = std::filesystem::file_size(path);
	ASSERT_TRUE(platform::WriteFileBytes(path, std::vector<uint8>(static_cast<size_t>(size), 0u)));

	EXPECT_FALSE(Ddc.GetRecord("Test", key).has_value());
	EXPECT_FALSE(std::filesystem::exists(path));
	EXPECT_EQ(Ddc.GetCounters().CorruptFilesDeleted, 1u);

	// Truncated or empty: the same.
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	std::vector<uint8> bytes = ReadBytes(path);
	bytes.resize(bytes.size() - 3u);
	ASSERT_TRUE(platform::WriteFileBytes(path, bytes));
	EXPECT_FALSE(Ddc.GetRecord("Test", key, EDdcRead::MetadataOnly).has_value());
	EXPECT_FALSE(std::filesystem::exists(path));
	ASSERT_TRUE(platform::WriteFileBytes(path, {}));
	EXPECT_FALSE(Ddc.GetRecord("Test", key).has_value());
	EXPECT_FALSE(std::filesystem::exists(path));

	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	ExpectSameValues(Ddc.GetRecord("Test", key).value_or(SDdcRecord()), record);
}

TEST_F(DerivedDataCacheTest, ATruncatedValueIsAMissAndIsDeleted)
{
	const hash::SContentHash key = MakeStaticKey(1u);
	const SDdcRecord record = MakeRecord({ MakeValue("Large", RandomBytes(300u * 1024u, 1u)) });
	FString error;
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	const std::filesystem::path value = Ddc.GetValuePath(hash::HashContent(record.Values[0].Bytes));
	std::vector<uint8> bytes = ReadBytes(value);
	bytes.resize(bytes.size() / 2u);
	ASSERT_TRUE(platform::WriteFileBytes(value, bytes));

	// The record is fine; the value it needs is not: a miss, and only the value is deleted.
	EXPECT_FALSE(Ddc.GetRecord("Test", key).has_value());
	EXPECT_FALSE(std::filesystem::exists(value));
	EXPECT_TRUE(std::filesystem::exists(Ddc.GetRecordPath("Test", key)));
	EXPECT_TRUE(Ddc.GetRecord("Test", key, EDdcRead::MetadataOnly).has_value());

	// A zero-filled value of the right size is caught by its header.
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	ASSERT_TRUE(std::filesystem::exists(value));
	ASSERT_TRUE(platform::WriteFileBytes(value, std::vector<uint8>(static_cast<size_t>(std::filesystem::file_size(value)), 0u)));
	EXPECT_FALSE(Ddc.GetRecord("Test", key).has_value());
	EXPECT_FALSE(std::filesystem::exists(value));

	// The next build rewrites the value (its size gave it away) and the record hits again.
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	ExpectSameValues(Ddc.GetRecord("Test", key).value_or(SDdcRecord()), record);
}

// One flipped bit in a raw value changes nothing a header or a size check can see: only the raw hash
// catches it - on every read where values are verified, and always in Verify.
TEST_F(DerivedDataCacheTest, ABitFlipInAValueIsDetected)
{
	const hash::SContentHash key = MakeStaticKey(1u);
	const SDdcRecord record = MakeRecord({ MakeValue("Large", RandomBytes(300u * 1024u, 1u)) });
	FString error;
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	const std::filesystem::path value = Ddc.GetValuePath(hash::HashContent(record.Values[0].Bytes));
	const auto flip = [&value] ()
	{
		std::vector<uint8> bytes = ReadBytes(value);
		ASSERT_GT(bytes.size(), 1000u);
		bytes[1000] ^= 0x10u;
		ASSERT_TRUE(platform::WriteFileBytes(value, bytes));
	};
	flip();

	Ddc.SetVerifyValues(true);
	EXPECT_FALSE(Ddc.GetRecord("Test", key).has_value());
	EXPECT_FALSE(std::filesystem::exists(value));

	// With read verification off (Release), Verify still finds it, and the record that needs it.
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	flip();
	Ddc.SetVerifyValues(false);
	const SDdcVerifyResult result = Ddc.Verify();
	EXPECT_EQ(result.CorruptValues, 1u);
	EXPECT_EQ(result.DanglingRecords, 1u);
	EXPECT_FALSE(std::filesystem::exists(value));
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetRecordPath("Test", key)));

	// A flipped bit in a record is its checksum's business, verified or not.
	ASSERT_TRUE(Ddc.PutRecord("Test", key, MakeRecord({ MakeValue("Small", RandomBytes(2000u, 2u)) }), &error)) << error.CStr();
	std::vector<uint8> bytes = ReadBytes(Ddc.GetRecordPath("Test", key));
	bytes[bytes.size() / 2u] ^= 0x01u;
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetRecordPath("Test", key), bytes));
	EXPECT_FALSE(Ddc.GetRecord("Test", key).has_value());
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetRecordPath("Test", key)));
}

TEST_F(DerivedDataCacheTest, ADamagedManifestIsAMissAndIsDeleted)
{
	const hash::SContentHash staticKey = MakeStaticKey(1u);
	const std::vector<SDdcInput> inputs = { MakeInput("File", "/A.h", 1u, 1u) };
	FString error;
	ASSERT_TRUE(Ddc.Put("Test", staticKey, inputs, MakeRecord({}), &error).has_value()) << error.CStr();
	const std::filesystem::path manifest = Ddc.GetManifestPath(staticKey);
	EXPECT_EQ(manifest, Root / "Manifests" / staticKey.ToString().Left(2u).ToPath() / (staticKey.ToString() + ".man").ToPath());
	ASSERT_TRUE(platform::WriteFileBytes(manifest, std::vector<uint8>(static_cast<size_t>(std::filesystem::file_size(manifest)), 0u)));

	EXPECT_FALSE(Ddc.FindKey(staticKey, [] (const SDdcInput&) { return true; }).has_value());
	EXPECT_FALSE(std::filesystem::exists(manifest));

	// A manifest copied under another StaticKey's name is refused too.
	ASSERT_TRUE(Ddc.Put("Test", staticKey, inputs, MakeRecord({}), &error).has_value()) << error.CStr();
	const std::filesystem::path other = Ddc.GetManifestPath(MakeStaticKey(2u));
	std::error_code ec;
	std::filesystem::create_directories(other.parent_path(), ec);
	ASSERT_TRUE(std::filesystem::copy_file(manifest, other, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	EXPECT_TRUE(Ddc.ReadManifest(MakeStaticKey(2u)).empty());
	EXPECT_FALSE(std::filesystem::exists(other));
	EXPECT_EQ(Ddc.ReadManifest(staticKey).size(), 1u);
}

// Two processes building the same steps at the same moment (the editor and AssetTool, two editors)
// write the same files through Tmp/ and atomic renames, with no lock: each key ends with one valid
// record and its values, and nothing torn or left over.
TEST_F(DerivedDataCacheTest, TwoProcessesWritingTheSameKeysEndWithOneValidRecord)
{
	Ddc.Close();
	const FString commandLine = FString::Format("\"{}\" {}ddc-writer \"{}\" 6", FString::FromPath(platform::GetExecutablePath()),
		test::ChildModeSwitch, FString::FromPath(Root));

	PROCESS_INFORMATION processes[2] = {};
	for (PROCESS_INFORMATION& process : processes)
	{
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		FWideString mutableLine(commandLine.View());   // CreateProcessW may write to it
		ASSERT_TRUE(CreateProcessW(nullptr, mutableLine.GetData(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
			nullptr, &startup, &process)) << platform::GetSystemErrorMessage(GetLastError()).CStr();
	}
	for (PROCESS_INFORMATION& process : processes)
	{
		ResumeThread(process.hThread);
	}
	for (PROCESS_INFORMATION& process : processes)
	{
		const DWORD waited = WaitForSingleObject(process.hProcess, 120'000u);
		if (waited != WAIT_OBJECT_0)
		{
			TerminateProcess(process.hProcess, 9u);
			WaitForSingleObject(process.hProcess, 10'000u);
		}
		DWORD exitCode = 99u;
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		EXPECT_EQ(waited, WAIT_OBJECT_0);
		EXPECT_EQ(exitCode, 0u);
	}

	FString error;
	ASSERT_TRUE(Ddc.Open(Root, &error)) << error.CStr();
	Ddc.SetVerifyValues(true);
	for (uint32 i = 0u; i < SharedKeyCount; ++i)
	{
		const std::vector<SDdcInput> inputs = SharedInputs(i);
		const std::optional<SDdcRecord> record = Ddc.Get("Shared", SharedStaticKey(i), [&inputs] (const SDdcInput& Recorded)
		{
			return std::find(inputs.begin(), inputs.end(), Recorded) != inputs.end();
		});
		ASSERT_TRUE(record.has_value()) << "key " << i;
		ExpectSameValues(*record, SharedRecord(i));
		EXPECT_EQ(Ddc.ReadManifest(SharedStaticKey(i)).size(), 1u);
	}
	EXPECT_EQ(CountFiles(Root / "Records" / "Shared"), SharedKeyCount);
	EXPECT_EQ(CountFiles(Root / "Values"), SharedKeyCount);
	EXPECT_EQ(CountFiles(Root / "Manifests"), SharedKeyCount);
	EXPECT_EQ(CountFiles(Root / "Tmp"), 0u);
	EXPECT_EQ(Ddc.Verify().GetProblemCount(), 0u);
	EXPECT_EQ(Ddc.GetCounters().CorruptFilesDeleted, 0u);
}


// ---------------- Maintenance ----------------

TEST_F(DerivedDataCacheTest, GarbageCollectionKeepsPinnedRecordsAndEvictsTheLeastRecentlyUsedFirst)
{
	FString error;
	// Four records, each with a 100 KiB external value of its own, written in order 1, 2, 3, 4.
	std::vector<hash::SContentHash> keys;
	std::vector<SDdcRecord> records;
	for (uint32 i = 1u; i <= 4u; ++i)
	{
		keys.push_back(MakeStaticKey(i));
		records.push_back(MakeRecord({ MakeValue("V", RandomBytes(100u * 1024u, i)) }));
		ASSERT_TRUE(Ddc.PutRecord("Test", keys.back(), records.back(), &error)) << error.CStr();
		std::this_thread::sleep_for(2ms);
	}
	// A later session uses record 1: the order of use is now 2, 3, 4, 1.
	{
		CDerivedDataCache later;
		ASSERT_TRUE(later.Open(Root, &error)) << error.CStr();
		ASSERT_TRUE(later.GetRecord("Test", keys[0]).has_value());
	}

	const uint64 recordBytes = std::filesystem::file_size(Ddc.GetRecordPath("Test", keys[0]));
	const uint64 valueBytes = std::filesystem::file_size(Ddc.GetValuePath(hash::HashContent(records[0].Values[0].Bytes)));
	const uint64 each = recordBytes + valueBytes;

	// Room for two and a half: record 2 is the oldest but pinned, so 3 and then 4 go.
	const hash::SContentHash pinned[] = { keys[1] };
	const SDdcGcResult result = Ddc.CollectGarbage(each * 5u / 2u, pinned);
	EXPECT_EQ(result.RecordsEvicted, 2u);
	EXPECT_EQ(result.ValuesEvicted, 2u);
	EXPECT_TRUE(result.bWithinBudget);
	EXPECT_EQ(result.ContentBytesBefore, 4u * each);
	EXPECT_EQ(result.ContentBytesAfter, 2u * each);
	EXPECT_EQ(result.PinnedBytes, each);
	EXPECT_TRUE(Ddc.GetRecord("Test", keys[0]).has_value());
	EXPECT_TRUE(Ddc.GetRecord("Test", keys[1]).has_value());
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetRecordPath("Test", keys[2])));
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetRecordPath("Test", keys[3])));
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetValuePath(hash::HashContent(records[2].Values[0].Bytes))));
	EXPECT_EQ(Ddc.GetStats().GetContentBytes(), 2u * each);

	// Nothing fits: everything unpinned goes, the pinned record and its value stay.
	const SDdcGcResult all = Ddc.CollectGarbage(0u, pinned);
	EXPECT_EQ(all.RecordsEvicted, 1u);
	EXPECT_FALSE(all.bWithinBudget);
	EXPECT_EQ(all.ContentBytesAfter, each);
	ExpectSameValues(Ddc.GetRecord("Test", keys[1]).value_or(SDdcRecord()), records[1]);
}

TEST_F(DerivedDataCacheTest, GarbageCollectionKeepsValuesOtherRecordsStillUse)
{
	FString error;
	const std::vector<uint8> shared = RandomBytes(100u * 1024u, 9u);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({ MakeValue("V", shared) }), &error)) << error.CStr();
	std::this_thread::sleep_for(2ms);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(2u), MakeRecord({ MakeValue("Same", shared) }), &error)) << error.CStr();
	EXPECT_EQ(CountFiles(Root / "Values"), 1u);

	// Room for one record and the value: the older record goes, the value it shares stays.
	const uint64 limit = std::filesystem::file_size(Ddc.GetValuePath(hash::HashContent(shared)))
		+ std::filesystem::file_size(Ddc.GetRecordPath("Test", MakeStaticKey(2u)));
	const SDdcGcResult result = Ddc.CollectGarbage(limit);
	EXPECT_EQ(result.RecordsEvicted, 1u);
	EXPECT_EQ(result.ValuesEvicted, 0u);
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetRecordPath("Test", MakeStaticKey(1u))));
	EXPECT_TRUE(Ddc.GetRecord("Test", MakeStaticKey(2u)).has_value());

	// A manifest whose only key lost its record goes with it.
	ASSERT_TRUE(Ddc.Put("Test", MakeStaticKey(3u), std::vector<SDdcInput>{ MakeInput("File", "/A.h", 1u, 1u) }, MakeRecord({}), &error))
		<< error.CStr();
	const SDdcGcResult manifests = Ddc.CollectGarbage(0u);
	EXPECT_EQ(manifests.ManifestsEvicted, 1u);
	EXPECT_EQ(CountFiles(Root / "Manifests"), 0u);
	EXPECT_EQ(Ddc.GetStats().GetContentBytes(), 0u);
}

TEST_F(DerivedDataCacheTest, GarbageCollectionDeletesOnlyOldLeftovers)
{
	FString error;
	// Leftovers of crashed writers: Tmp/ files and values no record names, an hour old or fresh.
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetTmpDir() / "old.tmp", RandomBytes(10u, 1u)));
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetTmpDir() / "new.tmp", RandomBytes(10u, 2u)));
	AgeFile(Ddc.GetTmpDir() / "old.tmp", 2h);

	const std::vector<uint8> oldValue = RandomBytes(100u * 1024u, 3u);
	const std::vector<uint8> newValue = RandomBytes(100u * 1024u, 4u);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({ MakeValue("V", oldValue) }), &error)) << error.CStr();
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(2u), MakeRecord({ MakeValue("V", newValue) }), &error)) << error.CStr();
	ASSERT_TRUE(std::filesystem::remove(Ddc.GetRecordPath("Test", MakeStaticKey(1u))));
	ASSERT_TRUE(std::filesystem::remove(Ddc.GetRecordPath("Test", MakeStaticKey(2u))));
	AgeFile(Ddc.GetValuePath(hash::HashContent(oldValue)), 2h);

	const SDdcGcResult result = Ddc.CollectGarbage(1ull << 40u);
	EXPECT_EQ(result.TmpFilesDeleted, 1u);
	EXPECT_EQ(result.ValuesEvicted, 1u);
	EXPECT_EQ(result.RecordsEvicted, 0u);
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetTmpDir() / "old.tmp"));
	EXPECT_TRUE(std::filesystem::exists(Ddc.GetTmpDir() / "new.tmp"));
	EXPECT_FALSE(std::filesystem::exists(Ddc.GetValuePath(hash::HashContent(oldValue))));
	EXPECT_TRUE(std::filesystem::exists(Ddc.GetValuePath(hash::HashContent(newValue))));
}

// `assets ddc gc --ci`: a dry run deletes nothing - no record, value, leftover or even a corrupt file - and
// reports exactly what the same collection then deletes.
TEST_F(DerivedDataCacheTest, ADryRunReportsWhatACollectionWouldDeleteAndDeletesNothing)
{
	FString error;
	for (uint32 i = 1u; i <= 3u; ++i)
	{
		ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(i), MakeRecord({ MakeValue("V", RandomBytes(100u * 1024u, i)) }), &error))
			<< error.CStr();
		std::this_thread::sleep_for(2ms);
	}
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetTmpDir() / "old.tmp", RandomBytes(10u, 1u)));
	AgeFile(Ddc.GetTmpDir() / "old.tmp", 2h);
	const std::filesystem::path corrupt = Ddc.GetRecordPath("Test", MakeStaticKey(9u));
	ASSERT_TRUE(platform::WriteFileBytes(corrupt, std::vector<uint8>(64u, 0u)));

	const uint64 each = std::filesystem::file_size(Ddc.GetRecordPath("Test", MakeStaticKey(1u)))
		+ std::filesystem::file_size(Ddc.GetValuePath(hash::HashContent(RandomBytes(100u * 1024u, 1u))));
	const SDdcStats before = Ddc.GetStats();
	const SDdcGcResult dry = Ddc.CollectGarbage(each, {}, EDdcGcMode::DryRun);
	EXPECT_TRUE(dry.bDryRun);
	EXPECT_EQ(dry.RecordsEvicted, 2u);
	EXPECT_EQ(dry.ValuesEvicted, 2u);
	EXPECT_EQ(dry.TmpFilesDeleted, 1u);
	EXPECT_EQ(dry.ContentBytesAfter, each);
	EXPECT_TRUE(dry.bWithinBudget);

	const SDdcStats after = Ddc.GetStats();
	EXPECT_EQ(after.GetContentBytes(), before.GetContentBytes());
	EXPECT_EQ(after.Records.Files, before.Records.Files);
	EXPECT_EQ(after.Values.Files, before.Values.Files);
	EXPECT_EQ(after.Tmp.Files, before.Tmp.Files);
	EXPECT_TRUE(std::filesystem::exists(corrupt));
	EXPECT_EQ(Ddc.GetCounters().CorruptFilesDeleted, 0u);

	const SDdcGcResult real = Ddc.CollectGarbage(each);
	EXPECT_FALSE(real.bDryRun);
	EXPECT_EQ(real.RecordsEvicted, dry.RecordsEvicted);
	EXPECT_EQ(real.ValuesEvicted, dry.ValuesEvicted);
	EXPECT_EQ(real.TmpFilesDeleted, dry.TmpFilesDeleted);
	EXPECT_EQ(real.ContentBytesBefore, dry.ContentBytesBefore);
	EXPECT_EQ(real.ContentBytesAfter, dry.ContentBytesAfter);
	EXPECT_EQ(Ddc.GetStats().GetContentBytes(), each);
	EXPECT_FALSE(std::filesystem::exists(corrupt));
	EXPECT_TRUE(Ddc.GetRecord("Test", MakeStaticKey(3u)).has_value());
}

// A writer that finds a value already there - an old leftover of a record since lost - writes the
// record naming it without rewriting it. It marks the value as just written first: a collection in
// another process that found it unreferenced and an hour old a moment ago keeps it (the time moved
// since its walk), and later collections count its hour from now.
TEST_F(DerivedDataCacheTest, AReusedValueCountsAsJustWritten)
{
	FString error;
	const std::vector<uint8> bytes = RandomBytes(100u * 1024u, 7u);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(1u), MakeRecord({ MakeValue("V", bytes) }), &error)) << error.CStr();
	ASSERT_TRUE(std::filesystem::remove(Ddc.GetRecordPath("Test", MakeStaticKey(1u))));
	const std::filesystem::path value = Ddc.GetValuePath(hash::HashContent(bytes));
	AgeFile(value, 2h);
	const int64 aged = GetWriteTime(value);

	Ddc.ResetCounters();
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(2u), MakeRecord({ MakeValue("Same", bytes) }), &error)) << error.CStr();
	EXPECT_EQ(Ddc.GetCounters().ValueWritesSkipped, 1u);
	EXPECT_EQ(Ddc.GetCounters().ValueWrites, 0u);
	EXPECT_GT(GetWriteTime(value), aged + HourFileTicks);

	// Even with that record lost as well, the value is no old leftover yet.
	ASSERT_TRUE(std::filesystem::remove(Ddc.GetRecordPath("Test", MakeStaticKey(2u))));
	const SDdcGcResult result = Ddc.CollectGarbage(1ull << 40u);
	EXPECT_TRUE(result.bComplete);
	EXPECT_EQ(result.ValuesEvicted, 0u);
	EXPECT_TRUE(std::filesystem::exists(value));
}

// Which values are referenced is known only when every record was read. A record that another
// program holds without read sharing while the collection runs - a pinned one here - makes the
// collection stop at the leftovers: its values, and any value that merely looks unreferenced, stay,
// and no record goes for a budget that evicting records alone could not reach.
TEST_F(DerivedDataCacheTest, GarbageCollectionThatCannotReadEveryRecordDeletesNoValues)
{
	FString error;
	const hash::SContentHash pinnedKey = MakeStaticKey(1u);
	const SDdcRecord pinnedRecord = MakeRecord({ MakeValue("V", RandomBytes(100u * 1024u, 1u)) });
	ASSERT_TRUE(Ddc.PutRecord("Test", pinnedKey, pinnedRecord, &error)) << error.CStr();
	const SDdcRecord otherRecord = MakeRecord({ MakeValue("V", RandomBytes(100u * 1024u, 2u)) });
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(2u), otherRecord, &error)) << error.CStr();
	const std::vector<uint8> orphan = RandomBytes(100u * 1024u, 3u);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(3u), MakeRecord({ MakeValue("V", orphan) }), &error)) << error.CStr();
	ASSERT_TRUE(std::filesystem::remove(Ddc.GetRecordPath("Test", MakeStaticKey(3u))));
	const std::filesystem::path pinnedValue = Ddc.GetValuePath(hash::HashContent(pinnedRecord.Values[0].Bytes));
	const std::filesystem::path orphanValue = Ddc.GetValuePath(hash::HashContent(orphan));
	AgeFile(pinnedValue, 2h);
	AgeFile(orphanValue, 2h);

	const hash::SContentHash pinned[] = { pinnedKey };
	const size_t filesBefore = CountFiles(Root / "Values") + CountFiles(Root / "Records");
	{
		const CHeldFile held(Ddc.GetRecordPath("Test", pinnedKey), 0u);
		ASSERT_TRUE(held.IsValid());
		const SDdcGcResult result = Ddc.CollectGarbage(0u, pinned);   // a budget of nothing: all unpinned would go
		EXPECT_FALSE(result.bComplete);
		EXPECT_EQ(result.RecordsUnreadable, 1u);
		EXPECT_EQ(result.ValuesEvicted, 0u);
		EXPECT_EQ(result.RecordsEvicted, 0u);
		EXPECT_EQ(result.ManifestsEvicted, 0u);
		EXPECT_FALSE(result.bWithinBudget);
	}
	EXPECT_EQ(CountFiles(Root / "Values") + CountFiles(Root / "Records"), filesBefore);
	ExpectSameValues(Ddc.GetRecord("Test", pinnedKey).value_or(SDdcRecord()), pinnedRecord);

	// Readable again: the next collection does the rest, around the pinned record and its value.
	const SDdcGcResult result = Ddc.CollectGarbage(0u, pinned);
	EXPECT_TRUE(result.bComplete);
	EXPECT_EQ(result.RecordsUnreadable, 0u);
	EXPECT_EQ(result.RecordsEvicted, 1u);
	EXPECT_EQ(result.ValuesEvicted, 2u);   // the orphan, and the evicted record's own value
	EXPECT_FALSE(std::filesystem::exists(orphanValue));
	EXPECT_TRUE(std::filesystem::exists(pinnedValue));
	ExpectSameValues(Ddc.GetRecord("Test", pinnedKey).value_or(SDdcRecord()), pinnedRecord);
}

TEST_F(DerivedDataCacheTest, VerifyDeletesCorruptFilesAndDanglingRecords)
{
	FString error;
	const SDdcRecord good = MakeRecord({ MakeValue("Small", RandomBytes(100u, 1u)), MakeValue("Large", RandomBytes(100u * 1024u, 2u)) });
	ASSERT_TRUE(Ddc.Put("Test", MakeStaticKey(1u), std::vector<SDdcInput>{ MakeInput("File", "/A.h", 1u, 1u) }, good, &error)) << error.CStr();

	// A record with a flipped bit, a record whose value is gone, a zero-filled manifest, a value
	// with a flipped bit (whose record dangles then).
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(2u), MakeRecord({ MakeValue("S", RandomBytes(300u, 3u)) }), &error));
	std::vector<uint8> bytes = ReadBytes(Ddc.GetRecordPath("Test", MakeStaticKey(2u)));
	bytes[20] ^= 0x04u;
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetRecordPath("Test", MakeStaticKey(2u)), bytes));

	const std::vector<uint8> lost = RandomBytes(100u * 1024u, 4u);
	ASSERT_TRUE(Ddc.PutRecord("Other", MakeStaticKey(3u), MakeRecord({ MakeValue("Lost", lost) }), &error));
	ASSERT_TRUE(std::filesystem::remove(Ddc.GetValuePath(hash::HashContent(lost))));

	ASSERT_TRUE(Ddc.Put("Test", MakeStaticKey(4u), std::vector<SDdcInput>{}, MakeRecord({}), &error)) << error.CStr();
	const std::filesystem::path manifest = Ddc.GetManifestPath(MakeStaticKey(4u));
	ASSERT_TRUE(platform::WriteFileBytes(manifest, std::vector<uint8>(static_cast<size_t>(std::filesystem::file_size(manifest)), 0u)));

	const std::vector<uint8> flipped = RandomBytes(100u * 1024u, 5u);
	ASSERT_TRUE(Ddc.PutRecord("Test", MakeStaticKey(5u), MakeRecord({ MakeValue("F", flipped) }), &error));
	std::vector<uint8> valueBytes = ReadBytes(Ddc.GetValuePath(hash::HashContent(flipped)));
	valueBytes.back() ^= 0x80u;
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetValuePath(hash::HashContent(flipped)), valueBytes));

	const SDdcVerifyResult result = Ddc.Verify();
	EXPECT_EQ(result.ManifestsChecked, 2u);
	EXPECT_EQ(result.RecordsChecked, 5u);
	EXPECT_EQ(result.ValuesChecked, 2u);
	EXPECT_EQ(result.CorruptRecords, 1u);
	EXPECT_EQ(result.CorruptValues, 1u);
	EXPECT_EQ(result.CorruptManifests, 1u);
	EXPECT_EQ(result.DanglingRecords, 2u);
	EXPECT_EQ(result.GetProblemCount(), 5u);
	EXPECT_GT(result.BytesDeleted, 100u * 1024u);

	// What was sound is untouched, and a second pass finds nothing.
	ExpectSameValues(Ddc.GetRecord("Test", CDerivedDataCache::ComputeKey(MakeStaticKey(1u), std::vector<SDdcInput>{ MakeInput("File", "/A.h", 1u, 1u) }))
		.value_or(SDdcRecord()), good);
	EXPECT_EQ(Ddc.Verify().GetProblemCount(), 0u);
	EXPECT_EQ(CountFiles(Root / "Records"), 2u);
}

// A value another program holds without read sharing cannot be checked now, which says nothing
// about it or the record that names it: Verify deletes neither.
TEST_F(DerivedDataCacheTest, VerifyLeavesAValueThatCannotBeReadNowAndItsRecord)
{
	FString error;
	const hash::SContentHash key = MakeStaticKey(1u);
	const SDdcRecord record = MakeRecord({ MakeValue("Large", RandomBytes(100u * 1024u, 1u)) });
	ASSERT_TRUE(Ddc.PutRecord("Test", key, record, &error)) << error.CStr();
	const std::filesystem::path value = Ddc.GetValuePath(hash::HashContent(record.Values[0].Bytes));
	{
		const CHeldFile held(value, 0u);
		ASSERT_TRUE(held.IsValid());
		const SDdcVerifyResult result = Ddc.Verify();
		EXPECT_EQ(result.RecordsChecked, 1u);
		EXPECT_EQ(result.GetProblemCount(), 0u);
	}
	EXPECT_TRUE(std::filesystem::exists(value));
	EXPECT_TRUE(std::filesystem::exists(Ddc.GetRecordPath("Test", key)));
	EXPECT_EQ(Ddc.Verify().GetProblemCount(), 0u);
	ExpectSameValues(Ddc.GetRecord("Test", key).value_or(SDdcRecord()), record);
}

TEST_F(DerivedDataCacheTest, StatsCountFilesAndBytesPerKind)
{
	FString error;
	ASSERT_TRUE(Ddc.Put("Test", MakeStaticKey(1u), std::vector<SDdcInput>{ MakeInput("File", "/A.h", 1u, 1u) },
		MakeRecord({ MakeValue("Large", RandomBytes(100u * 1024u, 1u)) }), &error)) << error.CStr();
	ASSERT_TRUE(platform::WriteFileBytes(Ddc.GetTmpDir() / "left.tmp", RandomBytes(7u, 1u)));
	ASSERT_TRUE(platform::WriteFileText(Root / "ToolVerdicts.txt", "# tools that failed a determinism check\nBlender 4.2.3\n\n"));

	const SDdcStats stats = Ddc.GetStats();
	EXPECT_EQ(stats.Manifests.Files, 1u);
	EXPECT_EQ(stats.Records.Files, 1u);
	EXPECT_EQ(stats.Values.Files, 1u);
	EXPECT_EQ(stats.Values.Bytes, 36u + 100u * 1024u);
	EXPECT_EQ(stats.Tmp.Files, 1u);
	EXPECT_EQ(stats.Tmp.Bytes, 7u);
	EXPECT_EQ(stats.Access.Files, 1u);
	EXPECT_EQ(stats.Other.Files, 2u);   // FORMAT and ToolVerdicts.txt
	EXPECT_EQ(stats.GetContentBytes(), stats.Manifests.Bytes + stats.Records.Bytes + stats.Values.Bytes);
	EXPECT_EQ(Ddc.ReadToolVerdicts(), std::vector<FString>{ FString("Blender 4.2.3") });
}

// The access log is the collector's only record of use (NTFS last-access times are unreliable): one
// line per record and day per instance, "<Unix microseconds> <bucket> <key>".
TEST_F(DerivedDataCacheTest, TheAccessLogNotesEachRecordOncePerInstanceAndDay)
{
	FString error;
	const hash::SContentHash key = MakeStaticKey(1u);
	ASSERT_TRUE(Ddc.PutRecord("Test", key, MakeRecord({}), &error)) << error.CStr();
	ASSERT_TRUE(Ddc.GetRecord("Test", key).has_value());
	ASSERT_TRUE(Ddc.GetRecord("Test", key).has_value());

	const std::vector<std::filesystem::path> logs = ListFiles(Root / "Access");
	ASSERT_EQ(logs.size(), 1u);
	const FString name = FString::FromPath(logs[0].filename());
	EXPECT_EQ(name.Len(), 12u) << name.CStr();   // yyyymmdd.log
	EXPECT_TRUE(name.EndsWith(".log"));
	const FString text = platform::ReadFileText(logs[0]).value_or("");
	std::vector<FStringView> lines;
	for (const FStringView line : text.Lines())
	{
		lines.push_back(line);
	}
	ASSERT_EQ(lines.size(), 1u) << text.CStr();
	EXPECT_TRUE(lines[0].EndsWith(FString(" Test ") + key.ToString())) << text.CStr();

	CDerivedDataCache other;
	ASSERT_TRUE(other.Open(Root, &error)) << error.CStr();
	ASSERT_TRUE(other.GetRecord("Test", key).has_value());
	const FString after = platform::ReadFileText(logs[0]).value_or("");
	EXPECT_EQ(after.Lines().Collect<std::vector<FStringView>>().size(), 2u) << after.CStr();
}


// ---------------- Open and Close ----------------

#if FRT_ASSERTIONS_ENABLED
// Open, Close and the destructor change the root every other call reads, so the owner drains the
// calls first. One still running - here a lookup whose IsCurrent never returns - makes Close assert
// instead of racing with it. The dying process removes its store before it closes.
TEST(DerivedDataCacheDeathTest, ClosingWhileACallIsRunningAsserts)
{
	const auto closeDuringACall = [] ()
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path()
			/ FString::Format("FRT Кеш закриття 缓存 {}", GetCurrentProcessId()).ToPath();
		CDerivedDataCache* ddc = new CDerivedDataCache();   // never deleted: the process dies in Close
		const hash::SContentHash staticKey = MakeStaticKey(1u);
		const std::vector<SDdcInput> inputs = { MakeInput("File", "/A.h", 1u, 1u) };
		if (!ddc->Open(root) || !ddc->AddToManifest(staticKey, CDerivedDataCache::ComputeKey(staticKey, inputs), inputs))
		{
			std::fputs("the store could not be set up\n", stderr);
			return;
		}
		std::atomic<bool> bInCall = false;
		std::thread caller([ddc, staticKey, &bInCall] ()
		{
			(void)ddc->FindKey(staticKey, [&bInCall] (const SDdcInput&)
			{
				bInCall = true;
				std::this_thread::sleep_for(1h);
				return true;
			});
		});
		caller.detach();
		while (!bInCall)
		{
			std::this_thread::sleep_for(1ms);
		}
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		ddc->Close();
	};
	EXPECT_DEATH(closeDuringACall(), "ASSERT FAILED");
}
#endif
