// The asset writer (Assets/Meta/AssetWriter.h, design §4.6, §4.7, §12.9), against the recording provider: nothing without
// the writer lock; an asset and its meta in the same CheckOut, Move, Delete and Add; metas written before sources; bytes
// that would not change are not rewritten and ask nobody; a refused CheckOut (ReadOnly, LockedByOther, Busy) or Move
// writes, moves and logs nothing; a read-only file under None is refused and keeps its attribute; a Move across two
// providers and a path outside the owner root are refused; a refused Add after the writes leaves the op for roll-forward,
// which a process without the lock never does; a torn log is emptied before the next op is logged; an op in which a path
// plays two roles (a swap, a write then a move of it), a copy and a write past the journal's frame limit are refused; the
// expected writes are the meta bytes in hand. F1: a write is checked against what its planner read (its Prior) right
// before it happens, on the first run and on a replay: a whole op whose file changed since its plan is given back stale
// with nothing written or asked, a meta whose source is gone is not written and no folder is made for it, a replay keeps
// a meta that arrived after the crash and never adds it, a create keeps a meta that lands between plan and write, and an
// op planned before Run finished an earlier one is given back to be planned again.
#include <gtest/gtest.h>

#include "Assets/Meta/AssetWriterTestUtils.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/RecordFrames.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

using namespace frt;
using namespace frt::assets;
using frt::test::CRecordingVcsProvider;
using frt::test::meta::CWriterTree;
using frt::test::meta::MakeMetaText;


namespace
{
// A create (Prior Absent).
SAssetOpStep WriteStep (FStringView Path, FStringView Text)
{
	SAssetOpStep step;
	step.Kind = EAssetOpStep::WriteFile;
	step.Path = FString(Path);
	step.Bytes.assign(Text.AsBytes().begin(), Text.AsBytes().end());
	return step;
}

// A rewrite of a file the planner read as Read.
SAssetOpStep RewriteStep (FStringView Path, FStringView Text, FStringView Read)
{
	SAssetOpStep step = WriteStep(Path, Text);
	step.Prior = EAssetOpPrior::Holds;
	step.PriorHash = hash::HashContent(Read.AsBytes());
	return step;
}

SAssetOpStep PathStep (EAssetOpStep Kind, FStringView Path, FStringView ToPath = FStringView())
{
	SAssetOpStep step;
	step.Kind = Kind;
	step.Path = FString(Path);
	step.ToPath = FString(ToPath);
	return step;
}

SAssetOp MakeOp (std::vector<SAssetOpStep> Steps)
{
	SAssetOp op;
	op.Id = SGuid::NewRandom();
	op.Verb = "test";
	op.Steps = std::move(Steps);
	return op;
}

std::vector<FString> Sorted (std::vector<FString> Files)
{
	std::sort(Files.begin(), Files.end(), [] (const FString& A, const FString& B)
	{
		return std::ranges::lexicographical_compare(A.View().AsBytes(), B.View().AsBytes());
	});
	return Files;
}

void SetReadOnly (const std::filesystem::path& File, bool bReadOnly)
{
	const std::filesystem::perms write = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
	std::filesystem::permissions(File, write, bReadOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add);
}
}


TEST(AssetWriterTest, NothingIsWrittenWithoutTheWriterLock)
{
	CWriterTree tree("Без замка");
	tree.Write("/Project/Камінь.png", "png");
	const std::unique_ptr<CAssetWriter> writer = tree.MakeWriter(false);
	SAssetWriteReport report;
	const SVcsResult result = writer->Run(MakeOp({ WriteStep("/Project/Камінь.png.meta", MakeMetaText(SGuid::NewRandom())) }), report);
	EXPECT_NE(result.Error, EVcsError::None);
	EXPECT_FALSE(report.bChangedDisk);
	EXPECT_FALSE(tree.Exists("/Project/Камінь.png.meta"));
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(writer->GetJournalPath(), ec));
}

TEST(AssetWriterTest, AnAssetAndItsMetaGoThroughEveryCallTogetherMetaFirst)
{
	CWriterTree tree("Пара");
	const SGuid guid = SGuid::NewRandom();
	// Planned source first: the writer puts the meta first.
	const SAssetOp op = MakeOp({ WriteStep("/Project/Б.png", "png bytes"), WriteStep("/Project/Б.png.meta", MakeMetaText(guid)) });

	tree.Writer->SetStopAfterStepForTest(1u);
	SAssetWriteReport report;
	EXPECT_NE(tree.Writer->Run(op, report).Error, EVcsError::None);
	EXPECT_TRUE(report.bLeftUnfinished);
	EXPECT_TRUE(tree.Exists("/Project/Б.png.meta")) << "the meta is written first";
	EXPECT_FALSE(tree.Exists("/Project/Б.png"));

	// Finish it, then look at the calls the whole op made.
	ASSERT_TRUE(tree.Writer->RollForward());
	EXPECT_EQ(tree.Read("/Project/Б.png"), "png bytes");
	EXPECT_EQ(tree.ReadMeta("/Project/Б.png.meta").value_or(SAssetMeta()).Guid, guid);
	EXPECT_FALSE(tree.JournalExists());

	const std::vector<FString> pair = Sorted({ tree.VcsPath("/Project/Б.png"), tree.VcsPath("/Project/Б.png.meta") });
	std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 3u) << "CheckOut by Run, CheckOut again by the roll-forward, Add";
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(Sorted(writes[0].Files), pair);
	EXPECT_EQ(writes[2].Name, "Add");
	EXPECT_EQ(Sorted(writes[2].Files), pair);
}

TEST(AssetWriterTest, BytesThatWouldNotChangeAreNotRewritten)
{
	CWriterTree tree("Без змін");
	const FString meta = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/a.png.meta", meta);
	const platform::SFileStat before = platform::Stat(tree.Abs("/Project/a.png.meta")).value();

	SAssetWriteReport report;
	EXPECT_EQ(tree.Writer->Run(MakeOp({ WriteStep("/Project/a.png.meta", meta) }), report).Error, EVcsError::None);
	EXPECT_EQ(report.StepsSkipped, 1u);
	EXPECT_FALSE(report.bChangedDisk);
	EXPECT_TRUE(tree.Vcs.GetCalls().empty()) << "no check-out for a write that does not happen";
	EXPECT_FALSE(tree.JournalExists());
	EXPECT_EQ(platform::Stat(tree.Abs("/Project/a.png.meta")).value().Identity, before.Identity);
}

TEST(AssetWriterTest, ARefusedCheckOutWritesNothing)
{
	for (const EVcsError refusal : { EVcsError::ReadOnly, EVcsError::LockedByOther, EVcsError::Busy })
	{
		CWriterTree tree("Відмова");
		tree.Write("/Project/a.png", "png");
		tree.Write("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()));
		const FString before = tree.Read("/Project/a.png.meta");
		tree.Vcs.Refuse("CheckOut", refusal);

		SAssetWriteReport report;
		const SVcsResult result = tree.Writer->Run(MakeOp({ PathStep(EAssetOpStep::Move, "/Project/a.png", "/Project/b.png"),
			PathStep(EAssetOpStep::Move, "/Project/a.png.meta", "/Project/b.png.meta"),
			RewriteStep("/Project/b.png.meta", MakeMetaText(SGuid::NewRandom()), before) }), report);
		EXPECT_EQ(result.Error, refusal) << static_cast<int>(refusal);
		EXPECT_TRUE(report.bRefusedByVcs);
		EXPECT_FALSE(report.bChangedDisk);
		EXPECT_EQ(tree.Read("/Project/a.png.meta"), before);
		EXPECT_TRUE(tree.Exists("/Project/a.png"));
		EXPECT_FALSE(tree.Exists("/Project/b.png"));
		EXPECT_FALSE(tree.JournalExists());
		const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
		ASSERT_EQ(writes.size(), 1u) << "nothing after the refusal";
		EXPECT_EQ(Sorted(writes[0].Files), Sorted({ tree.VcsPath("/Project/a.png"), tree.VcsPath("/Project/a.png.meta") }))
			<< "the meta is checked out under its old name, with its source";
	}
}

TEST(AssetWriterTest, AReadOnlyFileUnderNoneIsRefusedAndKeepsItsAttribute)
{
	CWriterTree tree("Лише читання");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()));
	const FString before = tree.Read("/Project/a.png.meta");
	SetReadOnly(tree.Abs("/Project/a.png.meta"), true);

	const std::unique_ptr<CAssetWriter> writer = tree.MakeNoneWriter();
	SAssetWriteReport report;
	const SVcsResult result = writer->Run(MakeOp({ RewriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()), before) }), report);
	EXPECT_EQ(result.Error, EVcsError::ReadOnly);
	EXPECT_TRUE(report.bRefusedByVcs);
	EXPECT_EQ(tree.Read("/Project/a.png.meta"), before);
	EXPECT_TRUE(platform::Stat(tree.Abs("/Project/a.png.meta")).value().bReadOnly);
	SetReadOnly(tree.Abs("/Project/a.png.meta"), false);
}

TEST(AssetWriterTest, ARefusedMoveMovesAndRewritesNothing)
{
	CWriterTree tree("Відмова руху");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()));
	const FString before = tree.Read("/Project/a.png.meta");
	tree.Vcs.Refuse("Move", EVcsError::Busy);

	SAssetWriteReport report;
	const SVcsResult result = tree.Writer->Run(MakeOp({ PathStep(EAssetOpStep::Move, "/Project/a.png", "/Project/b.png"),
		PathStep(EAssetOpStep::Move, "/Project/a.png.meta", "/Project/b.png.meta"),
		RewriteStep("/Project/b.png.meta", MakeMetaText(SGuid::NewRandom()), before) }), report);
	EXPECT_EQ(result.Error, EVcsError::Busy);
	EXPECT_TRUE(report.bRefusedByVcs);
	EXPECT_FALSE(report.bChangedDisk);
	EXPECT_FALSE(report.bLeftUnfinished);
	EXPECT_EQ(tree.Read("/Project/a.png.meta"), before);
	EXPECT_FALSE(tree.Exists("/Project/b.png"));
	EXPECT_FALSE(tree.Exists("/Project/b.png.meta"));
	EXPECT_FALSE(tree.JournalExists()) << "an op that changed nothing is given up";

	const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 2u);
	EXPECT_EQ(writes[1].Name, "Move");
	EXPECT_EQ(writes[1].Files, (std::vector<FString> { tree.VcsPath("/Project/a.png"), tree.VcsPath("/Project/b.png"),
		tree.VcsPath("/Project/a.png.meta"), tree.VcsPath("/Project/b.png.meta") })) << "asset and meta in one call";
}

TEST(AssetWriterTest, AMoveAcrossProvidersAndAPathOutsideTheRootAreRefused)
{
	CWriterTree tree("Два сховища", true);
	CRecordingVcsProvider pluginVcs;
	const std::unique_ptr<CAssetWriter> writer = tree.MakeWriter(true, nullptr, &pluginVcs);
	tree.Write("/Project/a.png", "png");
	const FString plugin = FString::Format("/Plugin.{}/a.png", FStringView(test::meta::PluginName));

	SAssetWriteReport report;
	EXPECT_NE(writer->Run(MakeOp({ PathStep(EAssetOpStep::Move, "/Project/a.png", plugin) }), report).Error, EVcsError::None);
	EXPECT_TRUE(report.bRefusedByVcs) << "VCS002: two repositories cannot move one file";
	EXPECT_TRUE(tree.Exists("/Project/a.png"));
	EXPECT_EQ(writer->FindProvider(plugin), &pluginVcs);
	EXPECT_EQ(writer->FindProvider("/Project/a.png"), &tree.Vcs);

	tree.Write("/Engine/x.png", "png");
	EXPECT_EQ(writer->FindProvider("/Engine/x.png"), nullptr) << "/Engine has its own owner root";
	EXPECT_NE(writer->Run(MakeOp({ WriteStep("/Engine/x.png.meta", MakeMetaText(SGuid::NewRandom())) }), report).Error, EVcsError::None);
	EXPECT_FALSE(tree.Exists("/Engine/x.png.meta"));
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
	EXPECT_TRUE(pluginVcs.GetCalls().empty());
}

// Add comes after the writes, so a refusal there cannot unwrite them: the op stays for roll-forward, which only the lock
// holder runs, and which adds them once the VCS takes them.
TEST(AssetWriterTest, ARefusedAddLeavesTheOpForTheLockHoldersRollForward)
{
	CWriterTree tree("Додати");
	tree.Write("/Project/a.png", "png");
	const SGuid guid = SGuid::NewRandom();
	tree.Vcs.Refuse("Add", EVcsError::Offline);
	SAssetWriteReport report;
	EXPECT_EQ(tree.Writer->Run(MakeOp({ WriteStep("/Project/a.png.meta", MakeMetaText(guid)) }), report).Error, EVcsError::Offline);
	EXPECT_TRUE(report.bRefusedByVcs);
	EXPECT_TRUE(report.bChangedDisk);
	EXPECT_TRUE(report.bLeftUnfinished);
	ASSERT_TRUE(tree.Writer->GetUnfinished().has_value());
	EXPECT_EQ(tree.Writer->GetUnfinished()->AddFiles, (std::vector<FString> { "/Project/a.png.meta", "/Project/a.png" }));

	tree.Vcs.Refuse("Add", EVcsError::None);
	FString error;
	EXPECT_FALSE(tree.MakeWriter(false)->RollForward(&error)) << "a process without the lock reports, never finishes";
	EXPECT_FALSE(error.IsEmpty());
	EXPECT_TRUE(tree.JournalExists());

	tree.Vcs.ClearCalls();
	ASSERT_TRUE(tree.Writer->RollForward(&error)) << error.CStr();
	EXPECT_FALSE(tree.JournalExists());
	const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 2u);
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(writes[1].Name, "Add");
	EXPECT_EQ(Sorted(writes[1].Files), Sorted({ tree.VcsPath("/Project/a.png"), tree.VcsPath("/Project/a.png.meta") }));
	EXPECT_EQ(tree.ReadMeta("/Project/a.png.meta").value_or(SAssetMeta()).Guid, guid);
}

// The op Run is given was planned against the disk before that roll-forward: it is given back stale, never run on top.
TEST(AssetWriterTest, RunFinishesAnUnfinishedOpBeforeItsOwnAndGivesItsOwnBackToBePlannedAgain)
{
	CWriterTree tree("Спершу старе");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/b.png", "png");
	tree.Writer->SetStopAfterStepForTest(0u);
	EXPECT_NE(tree.Writer->Run(MakeOp({ WriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom())) })).Error, EVcsError::None);
	EXPECT_FALSE(tree.Exists("/Project/a.png.meta"));
	ASSERT_TRUE(tree.Writer->GetUnfinished().has_value());

	const SAssetOp op = MakeOp({ WriteStep("/Project/b.png.meta", MakeMetaText(SGuid::NewRandom())) });
	SAssetWriteReport report;
	EXPECT_NE(tree.Writer->Run(op, report).Error, EVcsError::None);
	EXPECT_TRUE(report.bStale);
	EXPECT_FALSE(report.bRefusedByVcs);
	EXPECT_FALSE(report.bChangedDisk) << "this op wrote nothing";
	EXPECT_EQ(report.RolledForward, std::vector<FString> { "/Project/a.png.meta" }) << "what the caller rescans";
	EXPECT_TRUE(tree.Exists("/Project/a.png.meta"));
	EXPECT_FALSE(tree.Exists("/Project/b.png.meta"));
	EXPECT_FALSE(tree.JournalExists());

	EXPECT_EQ(tree.Writer->Run(op, report).Error, EVcsError::None) << "planned again, it runs";
	EXPECT_TRUE(tree.Exists("/Project/b.png.meta"));
}

// Begin appends, and a reader stops at a torn tail, so an op logged after one would be invisible to roll-forward: Run empties
// whatever the log holds that is not an unfinished op before it logs its own.
TEST(AssetWriterTest, ATornLogIsEmptiedBeforeTheNextOpIsLogged)
{
	CWriterTree tree("Рваний журнал");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/b.png", "png");
	SOpJournalEntry torn;
	torn.Op = MakeOp({ WriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom())) });
	std::vector<uint8> bytes = EncodeOpJournalBegin(torn);
	bytes.resize(bytes.size() - 5u);   // a crash during that Begin's append
	std::error_code ec;
	std::filesystem::create_directories(tree.Writer->GetJournalPath().parent_path(), ec);
	ASSERT_TRUE(platform::WriteFileBytes(tree.Writer->GetJournalPath(), bytes));
	ASSERT_FALSE(tree.Writer->GetUnfinished().has_value());

	const SAssetOp op = MakeOp({ WriteStep("/Project/b.png.meta", MakeMetaText(SGuid::NewRandom())) });
	tree.Writer->SetStopAfterStepForTest(0u);
	EXPECT_NE(tree.Writer->Run(op).Error, EVcsError::None);
	const std::optional<SOpJournalEntry> unfinished = tree.Writer->GetUnfinished();
	ASSERT_TRUE(unfinished.has_value()) << "the new op is logged where a reader finds it";
	EXPECT_EQ(unfinished->Op.Id, op.Id);

	ASSERT_TRUE(tree.Writer->RollForward());
	EXPECT_TRUE(tree.Exists("/Project/b.png.meta"));
	EXPECT_FALSE(tree.Exists("/Project/a.png.meta")) << "the torn op had changed nothing and is dropped";
	EXPECT_FALSE(tree.JournalExists());
}

// Each step replays idempotently; the op does only when no path comes back in a second role. A swap or a chain, a write
// followed by a move of that file, and two writes of one file (in any case) are refused before anything is asked or
// written, and so is a copy (M11) and a write past the journal's frame limit.
TEST(AssetWriterTest, AnOpThatAReplayCouldNotRedoIsRefused)
{
	CWriterTree tree("Двічі");
	tree.Write("/Project/x.png", "x");
	tree.Write("/Project/y.png", "y");
	const std::vector<SAssetOp> refused = {
		MakeOp({ PathStep(EAssetOpStep::Move, "/Project/x.png", "/Project/t.png"), PathStep(EAssetOpStep::Move, "/Project/y.png", "/Project/x.png"),
			PathStep(EAssetOpStep::Move, "/Project/t.png", "/Project/y.png") }),
		MakeOp({ WriteStep("/Project/x.png.meta", MakeMetaText(SGuid::NewRandom())),
			PathStep(EAssetOpStep::Move, "/Project/x.png.meta", "/Project/q.png.meta") }),
		MakeOp({ WriteStep("/Project/x.png.meta", MakeMetaText(SGuid::NewRandom())), WriteStep("/Project/X.png.meta", MakeMetaText(SGuid::NewRandom())) }),
		MakeOp({ PathStep(EAssetOpStep::Move, "/Project/x.png", "/Project/t.png"), PathStep(EAssetOpStep::Trash, "/Project/T.png") }),
		MakeOp({ PathStep(EAssetOpStep::CopyFile, "/Project/x.png", "/Project/c.png") }),
		MakeOp({ SAssetOpStep { EAssetOpStep::WriteFile, "/Project/x.png.meta", FString(), std::vector<uint8>(record_frames::MaxPayloadBytes, uint8('a')),
			hash::SContentHash() } }),
	};
	for (size_t index = 0u; index < refused.size(); ++index)
	{
		SAssetWriteReport report;
		EXPECT_NE(tree.Writer->Run(refused[index], report).Error, EVcsError::None) << index;
		EXPECT_FALSE(report.bChangedDisk) << index;
	}
	EXPECT_EQ(tree.Read("/Project/x.png"), "x");
	EXPECT_EQ(tree.Read("/Project/y.png"), "y");
	EXPECT_FALSE(tree.Exists("/Project/t.png"));
	EXPECT_FALSE(tree.Exists("/Project/c.png"));
	EXPECT_FALSE(tree.Exists("/Project/x.png.meta"));
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
	EXPECT_FALSE(tree.JournalExists());

	// A case-only rename, then the rewrite of its meta under the new spelling, is one file in each role.
	const FString meta = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/x.png.meta", meta);
	EXPECT_EQ(tree.Writer->Run(MakeOp({ PathStep(EAssetOpStep::Move, "/Project/x.png", "/Project/X.png"),
		PathStep(EAssetOpStep::Move, "/Project/x.png.meta", "/Project/X.png.meta"),
		RewriteStep("/Project/X.png.meta", MakeMetaText(SGuid::NewRandom(), { "/Project/old.png" }), meta) })).Error, EVcsError::None);
	EXPECT_TRUE(tree.IsSpelled("/Project/X.png"));
	EXPECT_TRUE(tree.IsSpelled("/Project/X.png.meta"));
	EXPECT_EQ(tree.ReadMeta("/Project/X.png.meta").value_or(SAssetMeta()).FormerPaths, std::vector<FString> { "/Project/old.png" });
}

TEST(AssetWriterTest, TheExpectedWritesAreTheBytesInHand)
{
	const FString meta = MakeMetaText(SGuid::NewRandom());
	const SAssetOp op = MakeOp({ PathStep(EAssetOpStep::Move, "/Project/a.png", "/Project/b.png"),
		PathStep(EAssetOpStep::Move, "/Project/a.png.meta", "/Project/b.png.meta"), WriteStep("/Project/b.png.meta", meta),
		PathStep(EAssetOpStep::CopyFile, "/Project/b.png", "/Project/c.png"), PathStep(EAssetOpStep::Trash, "/Project/d.png") });
	std::vector<FString> visited;
	ForEachExpectedWrite(op, [&] (FStringView Path, const hash::SContentHash& Hash, uint64 Size)
	{
		visited.emplace_back(Path);
		EXPECT_EQ(Hash, hash::HashContent(meta.View().AsBytes()));
		EXPECT_EQ(Size, meta.Len());
	});
	EXPECT_EQ(visited, std::vector<FString> { "/Project/b.png.meta" });
}

// ---------------- F1: preconditions ----------------

// The losers of one ReGuid set reference each other, so half a set is never written on the first try: one file that is
// no longer what was planned gives the whole op back, before anything is asked or logged.
TEST(AssetWriterTest, AFirstRunGivesBackAWholeOpWhoseFileChangedSinceItWasPlanned)
{
	CWriterTree tree("Застарілий план");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/b.png", "png");
	const FString a = MakeMetaText(SGuid::NewRandom());
	const FString b = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/a.png.meta", a);
	tree.Write("/Project/b.png.meta", b);
	const SAssetOp op = MakeOp({ RewriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()), a),
		RewriteStep("/Project/b.png.meta", MakeMetaText(SGuid::NewRandom()), b) });
	const FString pulled = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/b.png.meta", pulled);   // a pull, after the plan

	SAssetWriteReport report;
	EXPECT_NE(tree.Writer->Run(op, report).Error, EVcsError::None);
	EXPECT_TRUE(report.bStale);
	EXPECT_FALSE(report.bRefusedByVcs);
	EXPECT_FALSE(report.bChangedDisk);
	EXPECT_EQ(report.Changed, std::vector<FString> { "/Project/b.png.meta" });
	EXPECT_EQ(tree.Read("/Project/a.png.meta"), a) << "not half the set";
	EXPECT_EQ(tree.Read("/Project/b.png.meta"), pulled);
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
	EXPECT_FALSE(tree.JournalExists());

	// A create whose meta exists by now (in another spelling too), and a meta whose source has gone with its folder.
	tree.Write("/Project/c.png", "png");
	tree.Write("/Project/C.PNG.META", MakeMetaText(SGuid::NewRandom()));
	EXPECT_NE(tree.Writer->Run(MakeOp({ WriteStep("/Project/c.png.meta", MakeMetaText(SGuid::NewRandom())) }), report).Error, EVcsError::None);
	EXPECT_TRUE(report.bStale);
	EXPECT_TRUE(tree.IsSpelled("/Project/C.PNG.META"));
	EXPECT_NE(tree.Writer->Run(MakeOp({ WriteStep("/Project/Gone/d.png.meta", MakeMetaText(SGuid::NewRandom())) }), report).Error, EVcsError::None);
	EXPECT_TRUE(report.bStale);
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(tree.Abs("/Project/Gone"), ec)) << "no folder is made for a meta";
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
}

// A crash after Begin, then a pull of the meta the dead process was about to create: the replay keeps the pulled bytes,
// does not add them, and closes the op.
TEST(AssetWriterTest, AReplayKeepsAMetaThatArrivedAfterTheCrashAndNeverAddsIt)
{
	CWriterTree tree("Після збою");
	tree.Write("/Project/a.png", "png");
	tree.Writer->SetStopAfterStepForTest(0u);
	EXPECT_NE(tree.Writer->Run(MakeOp({ WriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom())) })).Error, EVcsError::None);
	ASSERT_TRUE(tree.Writer->GetUnfinished().has_value());
	const FString pulled = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/a.png.meta", pulled);

	tree.Vcs.ClearCalls();
	SAssetWriteReport report;
	FString error;
	ASSERT_TRUE(tree.Writer->RollForward(report, &error)) << error.CStr();
	EXPECT_TRUE(report.bStale);
	EXPECT_FALSE(report.bChangedDisk);
	EXPECT_EQ(report.Changed, std::vector<FString> { "/Project/a.png.meta" });
	EXPECT_EQ(tree.Read("/Project/a.png.meta"), pulled);
	EXPECT_FALSE(tree.JournalExists()) << "the op is finished";
	for (const CRecordingVcsProvider::SCall& call : tree.Vcs.GetWriteCalls())
	{
		EXPECT_NE(call.Name, "Add") << "a meta this op did not write is not its to add";
	}

	// A rewrite whose file changed after the crash is left as it is too, and one whose file is gone is not brought back.
	const FString before = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/a.png.meta", before);
	tree.Write("/Project/b.png", "png");
	tree.Write("/Project/b.png.meta", before);
	tree.Writer->SetStopAfterStepForTest(0u);
	EXPECT_NE(tree.Writer->Run(MakeOp({ RewriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()), before),
		RewriteStep("/Project/b.png.meta", MakeMetaText(SGuid::NewRandom()), before) })).Error, EVcsError::None);
	const FString edited = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/a.png.meta", edited);
	std::error_code ec;
	std::filesystem::remove(tree.Abs("/Project/b.png.meta"), ec);
	ASSERT_TRUE(tree.Writer->RollForward(report, &error)) << error.CStr();
	EXPECT_EQ(tree.Read("/Project/a.png.meta"), edited);
	EXPECT_FALSE(tree.Exists("/Project/b.png.meta"));
	EXPECT_EQ(report.StepsRun, 0u);
}

// What a sync does between an op's plan and its run: the meta it delivers is kept, and the op is given back.
TEST(AssetWriterTest, AMetaThatArrivesBetweenThePlanAndTheRunIsKept)
{
	CWriterTree tree("Між планом і записом");
	tree.Write("/Project/a.png", "png");
	const FString delivered = MakeMetaText(SGuid::NewRandom());
	tree.Writer->SetBeforeRunForTest([&] (const SAssetOp&) { tree.Write("/Project/a.png.meta", delivered); });
	SAssetWriteReport report;
	EXPECT_NE(tree.Writer->Run(MakeOp({ WriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom())) }), report).Error, EVcsError::None);
	EXPECT_TRUE(report.bStale);
	EXPECT_EQ(tree.Read("/Project/a.png.meta"), delivered);
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
	EXPECT_FALSE(tree.JournalExists());
}

TEST(AssetWriterTest, AnOpWithoutAPreconditionIsRefusedOnItsFirstRun)
{
	CWriterTree tree("Без умови");
	tree.Write("/Project/a.png", "png");
	SAssetOpStep step = WriteStep("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()));
	step.Prior = EAssetOpPrior::Unchecked;
	SAssetWriteReport report;
	EXPECT_NE(tree.Writer->Run(MakeOp({ step }), report).Error, EVcsError::None);
	EXPECT_FALSE(report.bChangedDisk);
	EXPECT_FALSE(tree.Exists("/Project/a.png.meta"));
}
