// The operation journal (Assets/Meta/OpJournal.h, design §4.6): an op logged with Begin reads back exactly, steps, meta
// bytes, hashes, planned GUIDs and the files to add included, until End empties the log; a log torn anywhere before its
// last step frame, or damaged in a frame, holds no unfinished op, because its writer had not touched the disk yet; a
// finished op is nothing to do; frames of another op are never taken for this one's; a missing log is empty.
#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>
#include <vector>

#include "Assets/Meta/OpJournal.h"
#include "Assets/MountPaths.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

using namespace frt;
using namespace frt::assets;


namespace
{
SOpJournalEntry MakeEntry ()
{
	SOpJournalEntry entry;
	entry.Op.Id = SGuid::NewRandom();
	entry.Op.Verb = "move";
	entry.Op.PlannedGuids = { SGuid::NewRandom(), SGuid::NewRandom() };

	SAssetOpStep move;
	move.Kind = EAssetOpStep::Move;
	move.Path = "/Project/Камінь 石.png";
	move.ToPath = "/Project/Нове/Камінь 石.png";
	SAssetOpStep write;
	write.Kind = EAssetOpStep::WriteFile;
	write.Path = "/Project/Нове/Камінь 石.png.meta";
	const FStringView meta = "FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n";
	write.Bytes.assign(meta.AsBytes().begin(), meta.AsBytes().end());
	write.Hash = hash::HashContent(write.Bytes);
	SAssetOpStep copy;
	copy.Kind = EAssetOpStep::CopyFile;
	copy.Path = "/Project/a.png";
	copy.ToPath = "/Plugin.X/b.png";
	copy.Hash = hash::HashContent(FStringView("a").AsBytes());
	SAssetOpStep trash;
	trash.Kind = EAssetOpStep::Trash;
	trash.Path = "/Project/old.png";
	entry.Op.Steps = { move, write, copy, trash };
	entry.AddFiles = { "/Plugin.X/b.png", "/Plugin.X/b.png.meta" };
	return entry;
}

void ExpectSameEntry (const SOpJournalEntry& Actual, const SOpJournalEntry& Expected)
{
	EXPECT_EQ(Actual.Op.Id, Expected.Op.Id);
	EXPECT_EQ(Actual.Op.Verb, Expected.Op.Verb);
	EXPECT_EQ(Actual.Op.PlannedGuids, Expected.Op.PlannedGuids);
	EXPECT_EQ(Actual.AddFiles, Expected.AddFiles);
	ASSERT_EQ(Actual.Op.Steps.size(), Expected.Op.Steps.size());
	for (size_t i = 0u; i < Expected.Op.Steps.size(); ++i)
	{
		EXPECT_EQ(Actual.Op.Steps[i].Kind, Expected.Op.Steps[i].Kind) << i;
		EXPECT_EQ(Actual.Op.Steps[i].Path, Expected.Op.Steps[i].Path) << i;
		EXPECT_EQ(Actual.Op.Steps[i].ToPath, Expected.Op.Steps[i].ToPath) << i;
		EXPECT_EQ(Actual.Op.Steps[i].Bytes, Expected.Op.Steps[i].Bytes) << i;
		EXPECT_EQ(Actual.Op.Steps[i].Hash, Expected.Op.Steps[i].Hash) << i;
	}
}

class OpJournalTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Журнал 日志 {}", platform::GetCurrentProcessId()).ToPath()
			/ FStringView(test->name()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::create_directories(Dir, ec);
		File = GetOpJournalPath(Dir);
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	SOpJournalContents ReadBack () const
	{
		SOpJournalContents contents;
		FString error;
		EXPECT_TRUE(ReadOpJournal(File, contents, &error)) << error.CStr();
		return contents;
	}

	void WriteLog (std::span<const uint8> Bytes) const
	{
		ASSERT_TRUE(platform::WriteFileBytes(File, Bytes));
	}

	std::filesystem::path Dir;
	std::filesystem::path File;
};
}


TEST_F(OpJournalTest, AMissingLogIsEmpty)
{
	const SOpJournalContents contents = ReadBack();
	EXPECT_TRUE(contents.bEmpty);
	EXPECT_FALSE(contents.bDroppedTail);
	EXPECT_FALSE(contents.Unfinished.has_value());
}

TEST_F(OpJournalTest, AnOpReadsBackExactlyUntilEndEmptiesTheLog)
{
	const SOpJournalEntry entry = MakeEntry();
	COpJournal journal(File);
	FString error;
	ASSERT_TRUE(journal.Begin(entry, &error)) << error.CStr();

	SOpJournalContents contents = ReadBack();
	EXPECT_FALSE(contents.bEmpty);
	EXPECT_FALSE(contents.bDroppedTail);
	ASSERT_TRUE(contents.Unfinished.has_value());
	ExpectSameEntry(*contents.Unfinished, entry);

	ASSERT_TRUE(journal.End(entry.Op.Id, &error)) << error.CStr();
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(File, ec)) << "End empties the log";
	contents = ReadBack();
	EXPECT_TRUE(contents.bEmpty);
	EXPECT_FALSE(contents.Unfinished.has_value());

	// The next op starts a new log.
	ASSERT_TRUE(journal.Begin(entry, &error)) << error.CStr();
	ASSERT_TRUE(ReadBack().Unfinished.has_value());
	ASSERT_TRUE(journal.Clear(&error)) << error.CStr();
	EXPECT_TRUE(ReadBack().bEmpty);
}

// The writer flushes Begin and every step frame before it touches the disk, so a log cut anywhere short of that has no
// op to finish: the op is dropped with its tail.
TEST_F(OpJournalTest, ALogTornBeforeItsLastStepHoldsNoUnfinishedOp)
{
	const std::vector<uint8> bytes = EncodeOpJournalBegin(MakeEntry());
	const size_t stride = bytes.size() / 23u + 1u;   // about two dozen cuts, every frame included
	for (size_t cut = 1u; cut < bytes.size(); cut += stride)
	{
		WriteLog(std::span<const uint8>(bytes).first(cut));
		const SOpJournalContents contents = ReadBack();
		EXPECT_FALSE(contents.Unfinished.has_value()) << cut;
		EXPECT_TRUE(contents.bDroppedTail) << cut;
	}
	WriteLog(bytes);
	EXPECT_TRUE(ReadBack().Unfinished.has_value());
}

TEST_F(OpJournalTest, ADamagedFrameEndsTheReading)
{
	std::vector<uint8> bytes = EncodeOpJournalBegin(MakeEntry());
	bytes[bytes.size() - 12u] ^= 0x5Au;   // inside the last step frame
	WriteLog(bytes);
	const SOpJournalContents contents = ReadBack();
	EXPECT_FALSE(contents.Unfinished.has_value());
	EXPECT_TRUE(contents.bDroppedTail);
}

TEST_F(OpJournalTest, AFinishedOpIsNothingToDoAndALaterOpIsReadAfterIt)
{
	const SOpJournalEntry first = MakeEntry();
	std::vector<uint8> bytes = EncodeOpJournalBegin(first);
	const std::vector<uint8> end = EncodeOpJournalEnd(first.Op.Id);
	bytes.insert(bytes.end(), end.begin(), end.end());
	WriteLog(bytes);
	SOpJournalContents contents = ReadBack();
	EXPECT_FALSE(contents.bEmpty);
	EXPECT_FALSE(contents.bDroppedTail);
	EXPECT_FALSE(contents.Unfinished.has_value());

	const SOpJournalEntry second = MakeEntry();
	const std::vector<uint8> next = EncodeOpJournalBegin(second);
	bytes.insert(bytes.end(), next.begin(), next.end());
	WriteLog(bytes);
	contents = ReadBack();
	ASSERT_TRUE(contents.Unfinished.has_value());
	EXPECT_EQ(contents.Unfinished->Op.Id, second.Op.Id);
}

TEST_F(OpJournalTest, FramesOfAnotherOpAreNeverTakenForThisOnes)
{
	const SOpJournalEntry a = MakeEntry();
	const SOpJournalEntry b = MakeEntry();

	// A's Begin frame alone, then B's whole log: B's step frames do not complete A, and B's Begin starts over.
	SOpJournalEntry aWithoutSteps = a;
	aWithoutSteps.Op.Steps.clear();
	std::vector<uint8> bytes = EncodeOpJournalBegin(a);
	bytes.resize(EncodeOpJournalBegin(aWithoutSteps).size());   // the Begin frame differs only in its step count
	std::vector<uint8> bSteps = EncodeOpJournalBegin(b);
	const size_t bBegin = EncodeOpJournalBegin([&] { SOpJournalEntry e = b; e.Op.Steps.clear(); return e; }()).size();
	bytes.insert(bytes.end(), bSteps.begin() + static_cast<ptrdiff_t>(bBegin), bSteps.end());
	WriteLog(bytes);
	SOpJournalContents contents = ReadBack();
	EXPECT_FALSE(contents.Unfinished.has_value());
	EXPECT_TRUE(contents.bDroppedTail);

	// An End of another op does not finish this one.
	bytes = EncodeOpJournalBegin(a);
	const std::vector<uint8> end = EncodeOpJournalEnd(b.Op.Id);
	bytes.insert(bytes.end(), end.begin(), end.end());
	WriteLog(bytes);
	contents = ReadBack();
	ASSERT_TRUE(contents.Unfinished.has_value());
	EXPECT_EQ(contents.Unfinished->Op.Id, a.Op.Id);
	EXPECT_TRUE(contents.bDroppedTail);
}
