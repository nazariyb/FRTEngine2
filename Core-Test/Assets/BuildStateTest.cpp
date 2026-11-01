// Per-project build state (Assets/BuildState.h). The log: entries round-trip and the latest record
// of a step wins, KeyBlobs are stored once, and a torn tail or a record that fails its checksum is a
// miss - the step's previous record or nothing - never a hit, with later records still found;
// compaction keeps exactly the latest records; a step whose diagnostics outgrow a record keeps the
// first lines and stays findable, and a KeyBlob or inputs no record can hold are refused; the index
// load and an append stay within the design's budgets (50 ms at 100k records, 20 us; twice that
// unless FRT_PERF_STRICT=1). The journal: a step that began and never ended - its
// process died - is reported crashed, stays so across restarts, and is cleared by its end or Retry.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "Assets/BuildState.h"
#include "Assets/KeyWriter.h"
#include "BuildConfig.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"
#include "Platform/Timestamp.h"

using namespace frt;
using namespace frt::assets;


namespace
{
hash::SContentHash HashOf (FStringView Text)
{
	return hash::HashContent(Text.AsBytes());
}

SBuildStepId MakeStep (uint32 Number, FStringView Builder = "Texture")
{
	return SBuildStepId{ FString::Format("9fc0d4010bbf28b4594072e72b86{:04x}", Number), FString(Builder), FString(), "D3D12" };
}

// A KeyBlob as a builder writes one; Settings varies it.
std::vector<uint8> MakeKeyBlob (FStringView Builder, uint32 Settings)
{
	CKeyWriter key;
	key.WriteStr("Builder", Builder);
	key.BeginRecord("Settings");
	key.WriteU64("Quality", Settings);
	key.EndRecord();
	return key.Finish().value_or(std::vector<uint8>());
}

SBuildStateEntry MakeEntry (const SBuildStepId& Step, uint32 Settings, uint32 Version)
{
	SBuildStateEntry entry;
	entry.Step = Step;
	entry.KeyBlob = MakeKeyBlob(Step.Builder, Settings);
	entry.Inputs = { SDdcInput{ "File", FString::Format("/Project/Include{}.h", Version), HashOf(FString::Format("v{}", Version)), 100u + Version } };
	entry.Key = HashOf(FString::Format("key {} {}", Step.Asset, Version));
	entry.Status = EBuildStatus::Ok;
	return entry;
}

void ExpectSameEntry (const SBuildStateEntry& Actual, const SBuildStateEntry& Expected, bool bWithKeyBlob)
{
	EXPECT_EQ(Actual.Step, Expected.Step);
	EXPECT_EQ(Actual.StaticKey, Expected.KeyBlob.empty() ? Expected.StaticKey : MakeStaticKey(Expected.KeyBlob));
	EXPECT_EQ(Actual.Inputs, Expected.Inputs);
	EXPECT_EQ(Actual.Key, Expected.Key);
	EXPECT_EQ(Actual.Status, Expected.Status);
	EXPECT_EQ(Actual.Diagnostics, Expected.Diagnostics);
	if (bWithKeyBlob)
	{
		EXPECT_EQ(Actual.KeyBlob, Expected.KeyBlob);
	}
	else
	{
		EXPECT_TRUE(Actual.KeyBlob.empty());
	}
}

std::vector<uint8> ReadBytes (const std::filesystem::path& Path)
{
	std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path);
	return bytes ? std::move(*bytes) : std::vector<uint8>();
}


// ---------------- The crashing child ----------------

// "Core-Test --frt-test-child=journal-crash <journal> <step>": begins the step, then dies on the spot,
// as a crashing importer would take its process down - no destructor, no atexit handler, no E line.
int RunJournalCrash (std::span<const FString> Arguments)
{
	if (Arguments.size() < 2u)
	{
		return 2;
	}
	const std::optional<SBuildStepId> step = SBuildStepId::Parse(Arguments[1]);
	CBuildJournal* journal = new CBuildJournal();   // never deleted: the process dies holding it
	if (!step || !journal->Open(Arguments[0].ToPath()) || !journal->BeginStep(*step))
	{
		return 3;
	}
	std::_Exit(7);
}

const bool bJournalCrashRegistered = test::RegisterChildMode("journal-crash", &RunJournalCrash);


class BuildStateTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Стан збірки 构建 😀 {}", platform::GetCurrentProcessId()).ToPath()
			/ FStringView(test->name()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		LogPath = GetBuildStatePath(Dir);
		JournalPath = GetBuildJournalPath(Dir);
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	std::filesystem::path Dir;
	std::filesystem::path LogPath;
	std::filesystem::path JournalPath;
};
}


// ---------------- Step ids ----------------

TEST(BuildStepIdTest, TextFormIsOneTokenAndRoundTrips)
{
	const SBuildStepId plain{ "9fc0d4010bbf28b4594072e72b8655ab", "Texture", "", "D3D12" };
	EXPECT_EQ(plain.ToString(), "9fc0d4010bbf28b4594072e72b8655ab|Texture||D3D12");
	EXPECT_EQ(SBuildStepId::Parse(plain.ToString()), plain);

	const SBuildStepId odd{ "id", "Translate:Model.Gltf", "mesh:Body 01|%x", "Win64\tD3D12 Ш" };
	const FString text = odd.ToString();
	EXPECT_EQ(text, "id|Translate:Model.Gltf|mesh:Body%2001%7C%25x|Win64%09D3D12%20Ш");
	EXPECT_FALSE(text.Contains(' '));
	EXPECT_EQ(SBuildStepId::Parse(text), odd);

	EXPECT_FALSE(SBuildStepId::Parse("a|b|c").has_value());
	EXPECT_FALSE(SBuildStepId::Parse("a|b|c|d|e").has_value());
	EXPECT_FALSE(SBuildStepId::Parse("a|b|c|%G0").has_value());
	EXPECT_FALSE(SBuildStepId::Parse("a|b|c|%2").has_value());
	EXPECT_FALSE(SBuildStepId::Parse("a b|b|c|d").has_value());
	EXPECT_EQ(SBuildStepId::Parse("|||"), SBuildStepId());

	EXPECT_EQ(ParseBuildStatus(GetBuildStatusName(EBuildStatus::Crashed)), EBuildStatus::Crashed);
	EXPECT_EQ(ParseBuildStatus("Ok"), EBuildStatus::Ok);
	EXPECT_FALSE(ParseBuildStatus("ok").has_value());
}


// ---------------- The build state log ----------------

TEST_F(BuildStateTest, EntriesRoundTripAndTheLatestRecordWins)
{
	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	EXPECT_TRUE(std::filesystem::exists(LogPath));

	SBuildStateEntry first = MakeEntry(MakeStep(1u), 1u, 1u);
	first.Diagnostics = { "warning: the alpha channel is unused", "info: 3 mips" };
	SBuildStateEntry second = MakeEntry(MakeStep(2u, "DataTable"), 2u, 1u);
	second.Status = EBuildStatus::Failed;
	second.Inputs.clear();
	ASSERT_TRUE(log.Append(first, &error)) << error.CStr();
	ASSERT_TRUE(log.Append(second, &error)) << error.CStr();

	ExpectSameEntry(log.Find(MakeStep(1u)).value_or(SBuildStateEntry()), first, false);
	ExpectSameEntry(log.Find(MakeStep(1u), EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()), first, true);
	ExpectSameEntry(log.Find(MakeStep(2u, "DataTable")).value_or(SBuildStateEntry()), second, false);
	EXPECT_FALSE(log.Find(MakeStep(3u)).has_value());
	EXPECT_FALSE(log.Find(MakeStep(1u, "DataTable")).has_value());   // another builder, another step

	// A newer build of step 1 replaces it, here and after a reopen.
	SBuildStateEntry newer = MakeEntry(MakeStep(1u), 3u, 2u);
	ASSERT_TRUE(log.Append(newer, &error)) << error.CStr();
	ExpectSameEntry(log.Find(MakeStep(1u), EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()), newer, true);
	log.Close();

	CBuildStateLog reopened;
	ASSERT_TRUE(reopened.Open(LogPath, &error)) << error.CStr();
	ExpectSameEntry(reopened.Find(MakeStep(1u), EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()), newer, true);
	ExpectSameEntry(reopened.Find(MakeStep(2u, "DataTable")).value_or(SBuildStateEntry()), second, false);
	EXPECT_EQ(reopened.FindKeyBlob(MakeStaticKey(first.KeyBlob)), first.KeyBlob);   // older blobs stay until Compact

	std::vector<SBuildStepId> visited;
	reopened.ForEachStep([&visited] (const SBuildStateEntry& Entry) { visited.push_back(Entry.Step); });
	EXPECT_EQ(visited.size(), 2u);
	std::vector<hash::SContentHash> expectedKeys = { newer.Key, second.Key };
	std::sort(expectedKeys.begin(), expectedKeys.end());
	EXPECT_EQ(reopened.CollectKeys(), expectedKeys);

	const SBuildStateLogStats stats = reopened.GetStats();
	EXPECT_EQ(stats.Steps, 2u);
	EXPECT_EQ(stats.KeyBlobs, 3u);
	EXPECT_EQ(stats.Records, 6u);
	EXPECT_EQ(stats.BadRanges, 0u);
	EXPECT_GT(stats.DeadBytes, 0u);
	EXPECT_EQ(stats.FileBytes, std::filesystem::file_size(LogPath));
}

TEST_F(BuildStateTest, KeyBlobsAreStoredOncePerDistinctBlob)
{
	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	ASSERT_TRUE(log.Append(MakeEntry(MakeStep(1u), 7u, 1u), &error)) << error.CStr();
	const uint64 afterFirst = std::filesystem::file_size(LogPath);
	ASSERT_TRUE(log.Append(MakeEntry(MakeStep(2u), 7u, 1u), &error)) << error.CStr();
	const uint64 afterSecond = std::filesystem::file_size(LogPath);
	ASSERT_TRUE(log.Append(MakeEntry(MakeStep(3u), 8u, 1u), &error)) << error.CStr();
	const uint64 afterThird = std::filesystem::file_size(LogPath);

	// The second entry shares the first one's blob: only its step record was written.
	EXPECT_LT(afterSecond - afterFirst, afterThird - afterSecond);
	EXPECT_EQ(log.GetStats().KeyBlobs, 2u);
	EXPECT_EQ(log.Find(MakeStep(2u), EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()).KeyBlob, MakeKeyBlob("Texture", 7u));

	// An entry without a blob keeps its StaticKey as given.
	SBuildStateEntry bare = MakeEntry(MakeStep(4u), 9u, 1u);
	bare.KeyBlob.clear();
	bare.StaticKey = HashOf("given");
	ASSERT_TRUE(log.Append(bare, &error)) << error.CStr();
	const std::optional<SBuildStateEntry> found = log.Find(MakeStep(4u), EBuildStateRead::WithKeyBlob);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->StaticKey, HashOf("given"));
	EXPECT_TRUE(found->KeyBlob.empty());
}

// The log's bytes are a format other processes and older builds read: a log of two steps (one with a
// KeyBlob and diagnostics, one without a blob) has exactly these bytes. Recorded before the record frames
// moved to Assets/RecordFrames.h (shared with the registry journal), so the move provably changed nothing.
TEST_F(BuildStateTest, ALogOfKnownStepsHasKnownBytes)
{
	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	SBuildStateEntry first = MakeEntry(MakeStep(1u), 1u, 1u);
	first.Diagnostics = { "warning: the alpha channel is unused" };
	SBuildStateEntry second = MakeEntry(MakeStep(2u, "DataTable"), 2u, 2u);
	second.KeyBlob.clear();
	second.StaticKey = HashOf("given");
	second.Status = EBuildStatus::Failed;
	ASSERT_TRUE(log.Append(first, &error)) << error.CStr();
	ASSERT_TRUE(log.Append(second, &error)) << error.CStr();
	log.Close();

	const std::vector<uint8> bytes = ReadBytes(LogPath);
	EXPECT_EQ(bytes.size(), 438u);
	EXPECT_EQ(hash::Xxh3_64(bytes), 0xE50519D1D940C059ull);
}

// A crash in the middle of an append leaves a torn last record: the reader skips it (the step falls
// back to its previous record), and records appended after it are found.
TEST_F(BuildStateTest, ATornTailIsIgnoredAndLaterAppendsAreFound)
{
	FString error;
	const SBuildStateEntry v1 = MakeEntry(MakeStep(1u), 1u, 1u);
	const SBuildStateEntry v2 = MakeEntry(MakeStep(1u), 1u, 2u);
	{
		CBuildStateLog log;
		ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
		ASSERT_TRUE(log.Append(v1, &error)) << error.CStr();
		ASSERT_TRUE(log.Append(v2, &error)) << error.CStr();
	}
	std::vector<uint8> bytes = ReadBytes(LogPath);
	bytes.resize(bytes.size() - 5u);
	ASSERT_TRUE(platform::WriteFileBytes(LogPath, bytes));

	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	ExpectSameEntry(log.Find(MakeStep(1u)).value_or(SBuildStateEntry()), v1, false);
	EXPECT_EQ(log.GetStats().BadRanges, 1u);

	const SBuildStateEntry other = MakeEntry(MakeStep(2u), 2u, 1u);
	ASSERT_TRUE(log.Append(other, &error)) << error.CStr();
	log.Close();

	CBuildStateLog reopened;
	ASSERT_TRUE(reopened.Open(LogPath, &error)) << error.CStr();
	ExpectSameEntry(reopened.Find(MakeStep(1u)).value_or(SBuildStateEntry()), v1, false);
	ExpectSameEntry(reopened.Find(MakeStep(2u), EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()), other, true);

	// The only record of a step torn away: a miss.
	bytes = ReadBytes(LogPath);
	reopened.Close();
	bytes.resize(bytes.size() - 1u);
	ASSERT_TRUE(platform::WriteFileBytes(LogPath, bytes));
	ASSERT_TRUE(reopened.Open(LogPath, &error)) << error.CStr();
	EXPECT_FALSE(reopened.Find(MakeStep(2u)).has_value());
	ExpectSameEntry(reopened.Find(MakeStep(1u)).value_or(SBuildStateEntry()), v1, false);
}

TEST_F(BuildStateTest, ARecordThatFailsItsChecksumIsSkipped)
{
	FString error;
	std::vector<SBuildStateEntry> entries;
	std::vector<uint64> ends;
	{
		CBuildStateLog log;
		ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
		for (uint32 i = 1u; i <= 3u; ++i)
		{
			entries.push_back(MakeEntry(MakeStep(i), 1u, i));
			ASSERT_TRUE(log.Append(entries.back(), &error)) << error.CStr();
			ends.push_back(std::filesystem::file_size(LogPath));
		}
	}
	// A flipped bit in the middle of the second step's record: it may not be read, and it may not hide
	// the record after it.
	std::vector<uint8> bytes = ReadBytes(LogPath);
	bytes[static_cast<size_t>((ends[0] + ends[1]) / 2u)] ^= 0x20u;
	ASSERT_TRUE(platform::WriteFileBytes(LogPath, bytes));

	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	ExpectSameEntry(log.Find(MakeStep(1u)).value_or(SBuildStateEntry()), entries[0], false);
	EXPECT_FALSE(log.Find(MakeStep(2u)).has_value());
	ExpectSameEntry(log.Find(MakeStep(3u)).value_or(SBuildStateEntry()), entries[2], false);
	EXPECT_EQ(log.GetStats().BadRanges, 1u);

	// A record of zeros where a power loss left the end of the file unwritten.
	log.Close();
	bytes = ReadBytes(LogPath);
	std::fill(bytes.begin() + static_cast<ptrdiff_t>(ends[1]), bytes.end(), uint8(0u));
	ASSERT_TRUE(platform::WriteFileBytes(LogPath, bytes));
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	EXPECT_FALSE(log.Find(MakeStep(3u)).has_value());
	ExpectSameEntry(log.Find(MakeStep(1u)).value_or(SBuildStateEntry()), entries[0], false);
}

TEST_F(BuildStateTest, CompactKeepsOnlyTheLatestRecordOfEachStep)
{
	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	std::vector<SBuildStateEntry> latest;
	for (uint32 step = 1u; step <= 20u; ++step)
	{
		for (uint32 version = 1u; version <= 5u; ++version)
		{
			SBuildStateEntry entry = MakeEntry(MakeStep(step), version, version);   // a new blob per version
			ASSERT_TRUE(log.Append(entry, &error)) << error.CStr();
			if (version == 5u)
			{
				latest.push_back(std::move(entry));
			}
		}
	}
	// Garbage at the end too, as a torn append would leave.
	{
		platform::CAppendFile garbage;
		ASSERT_TRUE(garbage.Open(LogPath));
		const uint8 junk[] = { 0xFEu, 0x42u, 0x53u, 0xA7u, 0xFFu, 0xFFu, 0x00u };
		ASSERT_TRUE(garbage.Append(junk));
	}
	log.Close();
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	const uint64 before = std::filesystem::file_size(LogPath);
	const SBuildStateLogStats statsBefore = log.GetStats();
	EXPECT_EQ(statsBefore.Records, 100u + 5u);   // step records, and the 5 distinct blobs
	EXPECT_EQ(statsBefore.BadRanges, 1u);

	ASSERT_TRUE(log.Compact(&error)) << error.CStr();
	const uint64 after = std::filesystem::file_size(LogPath);
	EXPECT_LT(after, before / 3u);
	EXPECT_EQ(before - after, statsBefore.DeadBytes);
	const SBuildStateLogStats statsAfter = log.GetStats();
	EXPECT_EQ(statsAfter.Steps, 20u);
	EXPECT_EQ(statsAfter.KeyBlobs, 1u);   // every latest entry has settings 5
	EXPECT_EQ(statsAfter.DeadBytes, 0u);
	EXPECT_EQ(statsAfter.BadRanges, 0u);
	for (const SBuildStateEntry& entry : latest)
	{
		ExpectSameEntry(log.Find(entry.Step, EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()), entry, true);
	}

	// Appends go on after the compacted records, and everything survives a reopen.
	const SBuildStateEntry more = MakeEntry(MakeStep(99u), 6u, 1u);
	ASSERT_TRUE(log.Append(more, &error)) << error.CStr();
	log.Close();
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	ExpectSameEntry(log.Find(MakeStep(99u), EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()), more, true);
	ExpectSameEntry(log.Find(MakeStep(7u)).value_or(SBuildStateEntry()), latest[6], false);
	EXPECT_EQ(log.GetStats().Steps, 21u);
}

TEST_F(BuildStateTest, AFileOfAnotherKindIsStartedAnew)
{
	ASSERT_TRUE(platform::WriteFileText(LogPath, "not a build state log at all"));
	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	EXPECT_EQ(std::filesystem::file_size(LogPath), 8u);
	EXPECT_FALSE(log.Find(MakeStep(1u)).has_value());
	ASSERT_TRUE(log.Append(MakeEntry(MakeStep(1u), 1u, 1u), &error)) << error.CStr();
	EXPECT_TRUE(log.Find(MakeStep(1u)).has_value());
}

// ReadKeys, for ddc gc reading the logs of other projects: the keys CollectKeys gives, read while the
// owner still holds its log open and appends to it, and nothing written - a missing file stays missing and
// a foreign one stays as it is, where Open would start both anew.
TEST_F(BuildStateTest, ReadKeysReadsAnotherProcesssLogWithoutWritingIt)
{
	FString error;
	CBuildStateLog owner;
	ASSERT_TRUE(owner.Open(LogPath, &error)) << error.CStr();
	const SBuildStateEntry first = MakeEntry(MakeStep(1u), 1u, 1u);
	const SBuildStateEntry second = MakeEntry(MakeStep(2u), 2u, 1u);
	const SBuildStateEntry newer = MakeEntry(MakeStep(1u), 3u, 2u);
	ASSERT_TRUE(owner.Append(first, &error)) << error.CStr();
	ASSERT_TRUE(owner.Append(second, &error)) << error.CStr();
	ASSERT_TRUE(owner.Append(newer, &error)) << error.CStr();

	const std::optional<platform::SFileStat> before = platform::Stat(LogPath);
	ASSERT_TRUE(before.has_value());
	const std::optional<std::vector<hash::SContentHash>> keys = CBuildStateLog::ReadKeys(LogPath, &error);
	ASSERT_TRUE(keys.has_value()) << error.CStr();
	EXPECT_EQ(*keys, owner.CollectKeys());   // the latest record of each step: newer's key, not first's
	EXPECT_EQ(keys->size(), 2u);
	EXPECT_EQ(std::count(keys->begin(), keys->end(), first.Key), 0);
	const std::optional<platform::SFileStat> after = platform::Stat(LogPath);
	ASSERT_TRUE(after.has_value());
	EXPECT_EQ(after->Size, before->Size);
	EXPECT_EQ(after->LastWriteTime, before->LastWriteTime);
	// The owner goes on appending: nothing of the read is in its way.
	ASSERT_TRUE(owner.Append(MakeEntry(MakeStep(3u), 4u, 1u), &error)) << error.CStr();
	EXPECT_EQ(CBuildStateLog::ReadKeys(LogPath)->size(), 3u);
	owner.Close();

	const std::filesystem::path missing = GetBuildStatePath(Dir / "Elsewhere");
	EXPECT_FALSE(CBuildStateLog::ReadKeys(missing, &error).has_value());
	EXPECT_FALSE(platform::Stat(missing).has_value());
	EXPECT_FALSE(platform::Stat(missing.parent_path()).has_value());

	const std::filesystem::path foreign = Dir / "Foreign.log";
	ASSERT_TRUE(platform::WriteFileText(foreign, "not a build state log at all"));
	error.Clear();
	EXPECT_FALSE(CBuildStateLog::ReadKeys(foreign, &error).has_value());
	EXPECT_TRUE(error.Contains("not a build state log")) << error.CStr();
	EXPECT_EQ(platform::ReadFileText(foreign), FString("not a build state log at all"));
}

// A reader takes a record past 16 MiB for garbage, so Append never writes one: a step whose
// diagnostics run past it keeps its first lines and a note of how many went - and stays a hit, here
// and after a reopen - while a KeyBlob or discovered inputs that large are refused with nothing
// written, the step's earlier record still found.
TEST_F(BuildStateTest, AStepTooLargeForARecordKeepsItsFirstDiagnosticsOrIsRefused)
{
	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();

	SBuildStateEntry chatty = MakeEntry(MakeStep(1u), 1u, 1u);
	const FString megabyte = FString::Repeat("x", 1024u * 1024u);
	chatty.Diagnostics.assign(20u, megabyte);   // 20 MiB of log
	chatty.Diagnostics[0] = "error: the first line says what went wrong";
	ASSERT_TRUE(log.Append(chatty, &error)) << error.CStr();

	const auto expectFirstLines = [&] (const std::optional<SBuildStateEntry>& Found)
	{
		ASSERT_TRUE(Found.has_value());
		EXPECT_EQ(Found->Key, chatty.Key);
		ASSERT_GE(Found->Diagnostics.size(), 3u);
		EXPECT_LT(Found->Diagnostics.size(), chatty.Diagnostics.size());
		EXPECT_EQ(Found->Diagnostics.front(), chatty.Diagnostics.front());
		for (size_t i = 1u; i + 1u < Found->Diagnostics.size(); ++i)
		{
			EXPECT_EQ(Found->Diagnostics[i].Len(), megabyte.Len()) << "line " << i;
		}
		const size_t leftOut = chatty.Diagnostics.size() - (Found->Diagnostics.size() - 1u);
		EXPECT_TRUE(Found->Diagnostics.back().StartsWith(FString::Format("... {} more diagnostic lines left out", leftOut)))
			<< Found->Diagnostics.back().CStr();
	};
	expectFirstLines(log.Find(MakeStep(1u)));
	log.Close();
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	expectFirstLines(log.Find(MakeStep(1u)));
	EXPECT_EQ(log.GetStats().BadRanges, 0u);

	// A KeyBlob no record can hold: refused, nothing written, the step's last record still the one found.
	const SBuildStateEntry before = MakeEntry(MakeStep(2u), 2u, 1u);
	ASSERT_TRUE(log.Append(before, &error)) << error.CStr();
	const uint64 size = std::filesystem::file_size(LogPath);
	SBuildStateEntry hugeBlob = MakeEntry(MakeStep(2u), 2u, 2u);
	hugeBlob.KeyBlob.assign(17u * 1024u * 1024u, uint8(0x5Au));
	error.Clear();
	EXPECT_FALSE(log.Append(hugeBlob, &error));
	EXPECT_TRUE(error.Contains("KeyBlob")) << error.CStr();

	// Discovered inputs no record can hold: refused the same way.
	SBuildStateEntry manyInputs = MakeEntry(MakeStep(2u), 2u, 3u);
	for (uint32 i = 0u; i < 17u; ++i)
	{
		manyInputs.Inputs.push_back(SDdcInput{ "File", FString::Format("/Project/{}/", i) + megabyte, HashOf("input"), 1u });
	}
	error.Clear();
	EXPECT_FALSE(log.Append(manyInputs, &error));
	EXPECT_TRUE(error.Contains("discovered inputs")) << error.CStr();

	EXPECT_EQ(std::filesystem::file_size(LogPath), size);
	ExpectSameEntry(log.Find(MakeStep(2u)).value_or(SBuildStateEntry()), before, false);
	log.Close();
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	ExpectSameEntry(log.Find(MakeStep(2u)).value_or(SBuildStateEntry()), before, false);
	EXPECT_EQ(log.GetStats().BadRanges, 0u);
}

// The design's budgets (§7.4): the index of 100k steps loads in at most 50 ms, an append takes at
// most 20 us. Timings mean something in optimized builds only; Debug runs a smaller log and checks
// correctness alone. The measurements are always recorded (RecordProperty). `FRT.bat verify` runs this
// inside the whole suite, on whatever else the machine is doing, where a load of 36 ms has been seen
// against some 20 ms alone: so the test fails on a clear regression - twice a budget - and holds the
// exact budgets only with FRT_PERF_STRICT=1 (run it alone on a quiet machine).
TEST_F(BuildStateTest, IndexLoadAndAppendStayWithinTheirBudgets)
{
	constexpr bool bOptimized = !FRT_BUILD_DEBUG;
	const bool bStrict = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1";
	const double slack = bStrict ? 1.0 : 2.0;
	const uint32 count = bOptimized ? 100'000u : 20'000u;

	// Ten shared KeyBlobs, as steps of one builder and preset share theirs.
	std::vector<std::vector<uint8>> blobs;
	for (uint32 i = 0u; i < 10u; ++i)
	{
		blobs.push_back(MakeKeyBlob("Texture", i));
	}
	std::vector<SBuildStateEntry> entries(count);
	for (uint32 i = 0u; i < count; ++i)
	{
		SBuildStateEntry& entry = entries[i];
		entry.Step = SBuildStepId{ FString::Format("{:032x}", i * 2654435761u), "Texture", FString(), "D3D12" };
		entry.KeyBlob = blobs[i % blobs.size()];
		entry.Inputs = { SDdcInput{ "File", FString::Format("/Project/Textures/T_{}.png", i), HashOf(FString::Format("{}", i)), i } };
		entry.Key = HashOf(FString::Format("key {}", i));
	}

	FString error;
	CBuildStateLog log;
	ASSERT_TRUE(log.Open(LogPath, &error)) << error.CStr();
	ASSERT_TRUE(log.LoadIndex(&error)) << error.CStr();
	const uint64 start = platform::ReadTimestamp();
	for (const SBuildStateEntry& entry : entries)
	{
		ASSERT_TRUE(log.Append(entry, &error)) << error.CStr();
	}
	const double appendUs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start) * 1000.0 / count;
	log.Close();

	// The best of three loads: the first may pay for reading the file into the OS cache.
	double loadMs = 1e9;
	for (uint32 attempt = 0u; attempt < 3u; ++attempt)
	{
		CBuildStateLog reopened;
		ASSERT_TRUE(reopened.Open(LogPath, &error)) << error.CStr();
		ASSERT_TRUE(reopened.LoadIndex(&error)) << error.CStr();
		const SBuildStateLogStats stats = reopened.GetStats();
		EXPECT_EQ(stats.Steps, count);
		EXPECT_EQ(stats.KeyBlobs, 10u);
		loadMs = (std::min)(loadMs, stats.IndexLoadMs);
		if (attempt == 0u)
		{
			ExpectSameEntry(reopened.Find(entries[count / 2u].Step, EBuildStateRead::WithKeyBlob).value_or(SBuildStateEntry()),
				entries[count / 2u], true);
		}
	}
	RecordProperty("Records", static_cast<int>(count));
	RecordProperty("AppendMicroseconds", FString::Format("{:.2f}", appendUs).CStr());
	RecordProperty("IndexLoadMilliseconds", FString::Format("{:.2f}", loadMs).CStr());
	RecordProperty("FileBytes", static_cast<int>(std::filesystem::file_size(LogPath)));
	if (bOptimized)
	{
		EXPECT_LE(appendUs, 20.0 * slack) << (bStrict ? "the budget" : "twice the budget");
		EXPECT_LE(loadMs, 50.0 * slack) << (bStrict ? "the budget" : "twice the budget");
	}
}


// ---------------- The journal ----------------

TEST_F(BuildStateTest, AStepThatBeganAndNeverEndedIsReportedCrashed)
{
	FString error;
	{
		CBuildJournal journal;
		ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
		EXPECT_TRUE(journal.GetCrashedSteps().empty());
		ASSERT_TRUE(journal.BeginStep(MakeStep(1u), &error)) << error.CStr();
		ASSERT_TRUE(journal.EndStep(MakeStep(1u), EBuildStatus::Ok, &error)) << error.CStr();
		ASSERT_TRUE(journal.BeginStep(MakeStep(2u), &error)) << error.CStr();
		ASSERT_TRUE(journal.EndStep(MakeStep(2u), EBuildStatus::Failed, &error)) << error.CStr();
		ASSERT_TRUE(journal.BeginStep(MakeStep(3u), &error)) << error.CStr();
		// ... and the process dies here: no E for step 3.
	}
	const FString text = platform::ReadFileText(JournalPath).value_or("");
	EXPECT_TRUE(text.StartsWith(FString("B ") + MakeStep(1u).ToString() + " ")) << text.CStr();
	EXPECT_TRUE(text.Contains(FString("E ") + MakeStep(2u).ToString() + " Failed\n")) << text.CStr();

	CBuildJournal journal;
	ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
	EXPECT_EQ(journal.GetCrashedSteps(), std::vector<SBuildStepId>{ MakeStep(3u) });
	EXPECT_TRUE(journal.IsCrashed(MakeStep(3u)));
	EXPECT_FALSE(journal.IsCrashed(MakeStep(1u)));

	// The journal now holds only step 3's B line: it never grows past the steps in flight.
	const FString kept = platform::ReadFileText(JournalPath).value_or("");
	EXPECT_TRUE(kept.StartsWith(FString("B ") + MakeStep(3u).ToString() + " ")) << kept.CStr();
	EXPECT_EQ(kept.Lines().Collect<std::vector<FStringView>>().size(), 1u) << kept.CStr();
}

TEST_F(BuildStateTest, ACrashedStepStaysCrashedUntilRetriedOrEnded)
{
	FString error;
	{
		CBuildJournal journal;
		ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
		ASSERT_TRUE(journal.BeginStep(MakeStep(1u), &error));
		ASSERT_TRUE(journal.BeginStep(MakeStep(2u), &error));
	}
	for (uint32 restart = 0u; restart < 2u; ++restart)
	{
		CBuildJournal journal;
		ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
		EXPECT_EQ(journal.GetCrashedSteps(), (std::vector<SBuildStepId>{ MakeStep(1u), MakeStep(2u) })) << "restart " << restart;
	}
	{
		CBuildJournal journal;
		ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
		ASSERT_TRUE(journal.Retry(MakeStep(1u), &error)) << error.CStr();
		EXPECT_FALSE(journal.IsCrashed(MakeStep(1u)));
		EXPECT_TRUE(journal.IsCrashed(MakeStep(2u)));
	}
	{
		CBuildJournal journal;
		ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
		EXPECT_EQ(journal.GetCrashedSteps(), std::vector<SBuildStepId>{ MakeStep(2u) });
		// Its source changed and it built to its end: no longer crashed either.
		ASSERT_TRUE(journal.BeginStep(MakeStep(2u), &error));
		ASSERT_TRUE(journal.EndStep(MakeStep(2u), EBuildStatus::Ok, &error));
		EXPECT_FALSE(journal.IsCrashed(MakeStep(2u)));
	}
	CBuildJournal journal;
	ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
	EXPECT_TRUE(journal.GetCrashedSteps().empty());
	EXPECT_EQ(std::filesystem::file_size(JournalPath), 0u);
}

TEST_F(BuildStateTest, ATornJournalLineIsIgnored)
{
	const FString step1 = MakeStep(1u).ToString();
	const FString step2 = MakeStep(2u).ToString();
	// Step 1 began; step 2's B line was cut by the crash in the middle of its write; junk lines too.
	ASSERT_TRUE(platform::WriteFileText(JournalPath, FString::Format("B {} 2026-10-24T10:00:00Z\nX junk\nE\nB not|a step\nB {} 2026-10",
		step1, step2)));
	FString error;
	CBuildJournal journal;
	ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
	EXPECT_EQ(journal.GetCrashedSteps(), std::vector<SBuildStepId>{ MakeStep(1u) });
}

// The real thing: a process that begins a step and dies without a chance to clean up.
TEST_F(BuildStateTest, AProcessThatDiesInAStepLeavesItCrashed)
{
	const SBuildStepId step{ "9fc0d4010bbf28b4594072e72b8655ab", "Translate:Model.Gltf", "mesh:Body 01", "" };
	const std::vector<FString> arguments = { FString(test::ChildModeSwitch) + "journal-crash", FString::FromPath(JournalPath),
		step.ToString() };
	const test::SProcessResult child = test::RunProcess(platform::GetExecutablePath(), arguments);
	ASSERT_EQ(child.ExitCode, 7) << child.Errors.CStr();

	FString error;
	CBuildJournal journal;
	ASSERT_TRUE(journal.Open(JournalPath, &error)) << error.CStr();
	EXPECT_EQ(journal.GetCrashedSteps(), std::vector<SBuildStepId>{ step });
}
