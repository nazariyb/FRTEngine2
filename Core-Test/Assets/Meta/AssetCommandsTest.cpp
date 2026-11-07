// M4's journalled commands (Assets/Meta/AssetCommands.h, design §4.6, §12.6) against the recording provider: create meta
// writes one meta with its pre-allocated GUID and checks out and adds it with its source, never writes a source's
// provisional id, and never remints an existing meta in any spelling; a move carries the meta and declared companions in
// one provider call, records the old path in FormerPaths (and drops the new one from it), records nothing for a case-only rename, renames a meta spelled in another
// case to the asset's case, moves a broken meta and a newer engine's meta unchanged (nor does a dedupe rewrite the latter),
// and refuses an occupied target, another file's meta, a
// non-portable name and two files landing on one name before anything moves; a delete hands the source and its meta to one
// Delete, and deletes a meta alone only when it is an orphan; a refusal is VCS002 on the asset with nothing written. Kill-mid-command: every crash point of each multi-file
// command stops in-process and start-up roll-forward leaves a consistent pair with its planned GUID; then one real child
// process per command is killed mid-op (three launches in all).
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"
#include "Assets/Meta/AssetWriterTestUtils.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Meta/AssetCommands.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Registry/RegistryStore.h"
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
constexpr FStringLiteral RockPath = "/Project/Скеля 岩.png";
constexpr FStringLiteral RockMeta = "/Project/Скеля 岩.png.meta";
constexpr FStringLiteral RockBin = "/Project/Скеля 岩.bin";
constexpr FStringLiteral MovedPath = "/Project/Нова/b.png";
constexpr FStringLiteral MovedMeta = "/Project/Нова/b.png.meta";
constexpr FStringLiteral MovedBin = "/Project/Нова/Скеля 岩.bin";

// The move every kill test makes: an asset with its meta and one companion into a new folder.
std::vector<SAssetMove> MakeRockMove ()
{
	SAssetMove move;
	move.From = FString(RockPath);
	move.To = FString(MovedPath);
	move.Companions = { SVcsMove { FString(RockBin), FString(MovedBin) } };
	return { move };
}

void WriteRock (const CWriterTree& Tree, const SGuid& Guid)
{
	Tree.Write(RockPath, "png");
	Tree.Write(RockMeta, MakeMetaText(Guid));
	Tree.Write(RockBin, "bin");
}

enum class ECommand : uint8 { Create, Move, Delete };

SAssetCommandResult RunCommand (CAssetWriter& Writer, ECommand Command, const SGuid& Guid)
{
	switch (Command)
	{
	case ECommand::Create:
	{
		SCreateMetaRequest request;
		request.Source = FString(RockPath);
		request.Meta.Guid = Guid;
		return CreateMeta(Writer, request);
	}
	case ECommand::Move:
		return MoveAssets(Writer, MakeRockMove());
	case ECommand::Delete:
	{
		const std::vector<SAssetDelete> deletes = { SAssetDelete { FString(RockPath), { FString(RockBin) } } };
		return DeleteAssets(Writer, deletes);
	}
	}
	return {};
}

void PrepareFor (const CWriterTree& Tree, ECommand Command, const SGuid& Guid)
{
	if (Command == ECommand::Create)
	{
		Tree.Write(RockPath, "png");
	}
	else
	{
		WriteRock(Tree, Guid);
	}
}

// What each command leaves once it, or the roll-forward after a crash, is done: a consistent pair and its identity.
void ExpectDone (const CWriterTree& Tree, ECommand Command, const SGuid& Guid, FStringView Context)
{
	switch (Command)
	{
	case ECommand::Create:
		EXPECT_TRUE(Tree.Exists(RockPath)) << Context.GetData();
		EXPECT_EQ(Tree.ReadMeta(RockMeta).value_or(SAssetMeta()).Guid, Guid) << Context.GetData();
		break;
	case ECommand::Move:
	{
		EXPECT_FALSE(Tree.Exists(RockPath)) << Context.GetData();
		EXPECT_FALSE(Tree.Exists(RockMeta)) << Context.GetData();
		EXPECT_FALSE(Tree.Exists(RockBin)) << Context.GetData();
		EXPECT_EQ(Tree.Read(MovedPath), "png") << Context.GetData();
		EXPECT_EQ(Tree.Read(MovedBin), "bin") << Context.GetData();
		const SAssetMeta meta = Tree.ReadMeta(MovedMeta).value_or(SAssetMeta());
		EXPECT_EQ(meta.Guid, Guid) << Context.GetData();
		EXPECT_EQ(meta.FormerPaths, std::vector<FString> { FString(RockPath) }) << Context.GetData();
		break;
	}
	case ECommand::Delete:
		EXPECT_FALSE(Tree.Exists(RockPath)) << Context.GetData();
		EXPECT_FALSE(Tree.Exists(RockMeta)) << Context.GetData();
		EXPECT_FALSE(Tree.Exists(RockBin)) << Context.GetData();
		break;
	}
	EXPECT_FALSE(Tree.JournalExists()) << Context.GetData();
}


// ---------------- The killed child ----------------

// "Core-Test --frt-test-child=asset-command-kill <engine dir> <engine content> <engine config> <project dir> <command>
// <stop after> <guid>": runs the command through a writer that holds the lock and stops after that many steps, says so,
// and waits there to be killed, as a process that crashes in the middle of a command.
int RunAssetCommandKill (std::span<const FString> Arguments)
{
	if (Arguments.size() < 7u)
	{
		return 2;
	}
	const CMountTable mounts = test::meta::ResolveMounts(Arguments[0].ToPath(), Arguments[1].ToPath(), Arguments[2].ToPath(), Arguments[3].ToPath());
	CRecordingVcsProvider vcs;
	SAssetWriterDesc desc;
	desc.Mounts = &mounts;
	desc.OwnerRoot = Arguments[3].ToPath();
	desc.Providers = { { ProjectMountId, &vcs } };
	desc.bHoldsLock = true;
	CAssetWriter writer(std::move(desc));
	const uint32 stopAfter = Arguments[5].ToUInt32().value_or(0u);
	writer.SetStopAfterStepForTest(stopAfter, []
	{
		std::fputs("stopped\n", stdout);
		std::fflush(stdout);
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	});
	const ECommand command = Arguments[4] == "create" ? ECommand::Create : (Arguments[4] == "move" ? ECommand::Move : ECommand::Delete);
	(void)RunCommand(writer, command, SGuid::Parse(Arguments[6]).value_or(SGuid()));
	std::fputs("returned\n", stdout);
	return 3;
}

const bool bAssetCommandKillRegistered = test::RegisterChildMode("asset-command-kill", &RunAssetCommandKill);

void KillMidCommand (ECommand Command, FStringView Name, uint32 StopAfter)
{
	CWriterTree tree(FString::Format("Вбитий {}", Name));
	const SGuid guid = SGuid::NewRandom();
	PrepareFor(tree, Command, guid);
	const std::vector<FString> arguments = { FString(test::ChildModeSwitch) + "asset-command-kill", FString::FromPath(tree.Tree.EngineDir()),
		FString::FromPath(tree.Tree.EngineContent()), FString::FromPath(tree.Tree.EngineConfig()), FString::FromPath(tree.Tree.ProjectDir()),
		FString(Name), FString::Format("{}", StopAfter), guid.ToString() };
	test::CTestProcess child;
	FString error;
	ASSERT_TRUE(child.Start(platform::GetExecutablePath(), arguments, false, {}, &error)) << error.CStr();
	ASSERT_TRUE(test::WaitUntil([&child] { return child.ReadOutput().Contains("stopped") || !child.IsRunning(); }))
		<< child.ReadErrors().CStr();
	ASSERT_TRUE(child.ReadOutput().Contains("stopped")) << child.ReadOutput().CStr() << child.ReadErrors().CStr();
	child.Kill();
	child.Wait();

	ASSERT_TRUE(tree.Writer->GetUnfinished().has_value()) << "the killed process left its op in the journal";
	EXPECT_EQ(tree.Writer->GetUnfinished()->Op.PlannedGuids.empty(), Command == ECommand::Delete);
	ASSERT_TRUE(tree.Writer->RollForward(&error)) << error.CStr();
	ExpectDone(tree, Command, guid, Name);
}
}


// ---------------- Create meta ----------------

TEST(AssetCommandsTest, CreateMetaWritesThePlannedGuidAndChecksOutAndAddsThePair)
{
	CWriterTree tree("Створити");
	tree.Write(RockPath, "png");
	const SGuid guid = SGuid::NewRandom();
	SCreateMetaRequest request;
	request.Source = FString(RockPath);
	request.Meta.Guid = guid;
	request.Meta.Labels = { "Ландшафт" };
	const SAssetCommandResult result = CreateMeta(*tree.Writer, request);
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	EXPECT_EQ(result.Op.PlannedGuids, std::vector<SGuid> { guid });
	const SAssetMeta meta = tree.ReadMeta(RockMeta).value_or(SAssetMeta());
	EXPECT_EQ(meta.Guid, guid);
	EXPECT_EQ(meta.Labels, std::vector<FString> { "Ландшафт" });

	const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 2u);
	const std::vector<FString> pair = { tree.VcsPath(RockMeta), tree.VcsPath(RockPath) };
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(writes[0].Files, pair);
	EXPECT_EQ(writes[1].Name, "Add");
	EXPECT_EQ(writes[1].Files, pair);

	// No GUID given: a random one, pre-allocated in the plan.
	tree.Write("/Project/b.png", "png");
	SCreateMetaRequest random;
	random.Source = "/Project/b.png";
	const SAssetCommandResult second = CreateMeta(*tree.Writer, random);
	ASSERT_TRUE(second.IsOk());
	ASSERT_EQ(second.Op.PlannedGuids.size(), 1u);
	EXPECT_TRUE(second.Op.PlannedGuids[0].IsValid());
	EXPECT_EQ(tree.ReadMeta("/Project/b.png.meta").value_or(SAssetMeta()).Guid, second.Op.PlannedGuids[0]);

	// The source's provisional id, whoever asks for it, is never written: a random one instead (F19).
	tree.Write("/Project/c.png", "png");
	SCreateMetaRequest provisional;
	provisional.Source = "/Project/c.png";
	provisional.Meta.Guid = MakeProvisionalGuid("/Project/c.png");
	const SAssetCommandResult third = CreateMeta(*tree.Writer, provisional);
	ASSERT_TRUE(third.IsOk());
	ASSERT_EQ(third.Op.PlannedGuids.size(), 1u);
	EXPECT_TRUE(third.Op.PlannedGuids[0].IsValid());
	EXPECT_NE(third.Op.PlannedGuids[0], provisional.Meta.Guid);
	EXPECT_EQ(tree.ReadMeta("/Project/c.png.meta").value_or(SAssetMeta()).Guid, third.Op.PlannedGuids[0]);
}

TEST(AssetCommandsTest, CreateMetaNeverRemintsAMetaInAnySpellingOrMakesOneForNothing)
{
	CWriterTree tree("Не вдруге");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/A.PNG.meta", MakeMetaText(SGuid::NewRandom()));
	const FString before = tree.Read("/Project/A.PNG.meta");
	SCreateMetaRequest request;
	request.Source = "/Project/a.png";
	SAssetCommandResult result = CreateMeta(*tree.Writer, request);
	EXPECT_FALSE(result.IsOk());
	ASSERT_FALSE(result.Problems.empty());
	EXPECT_EQ(result.Problems[0].Subject, "/Project/a.png");
	EXPECT_EQ(result.Problems[0].File, "Content/a.png");
	EXPECT_EQ(tree.Read("/Project/A.PNG.meta"), before);
	EXPECT_TRUE(tree.IsSpelled("/Project/A.PNG.meta"));

	request.Source = "/Project/missing.png";
	result = CreateMeta(*tree.Writer, request);
	EXPECT_FALSE(result.IsOk());
	EXPECT_FALSE(tree.Exists("/Project/missing.png.meta"));
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
}

TEST(AssetCommandsTest, ARefusalIsVcs002OnTheAssetWithNothingWritten)
{
	CWriterTree tree("VCS002");
	tree.Write(RockPath, "png");
	tree.Vcs.Refuse("CheckOut", EVcsError::LockedByOther, "locked by Олена");
	SCreateMetaRequest request;
	request.Source = FString(RockPath);
	const SAssetCommandResult result = CreateMeta(*tree.Writer, request);
	EXPECT_FALSE(result.IsOk());
	EXPECT_EQ(result.Vcs.Error, EVcsError::LockedByOther);
	ASSERT_EQ(result.Problems.size(), 1u);
	EXPECT_EQ(result.Problems[0].Code, Vcs002);
	EXPECT_EQ(result.Problems[0].Severity, ESeverity::Error);
	EXPECT_EQ(result.Problems[0].Subject, FStringView(RockPath));
	EXPECT_TRUE(result.Problems[0].Message.Contains("Олена"));
	EXPECT_FALSE(tree.Exists(RockMeta));
	EXPECT_FALSE(tree.JournalExists());
}


// ---------------- Move ----------------

TEST(AssetCommandsTest, AMoveCarriesMetaAndCompanionsInOneCallAndRecordsTheFormerPath)
{
	CWriterTree tree("Перенести");
	const SGuid guid = SGuid::NewRandom();
	WriteRock(tree, guid);
	const SAssetCommandResult result = MoveAssets(*tree.Writer, MakeRockMove());
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	EXPECT_EQ(result.Op.PlannedGuids, std::vector<SGuid> { guid });
	ExpectDone(tree, ECommand::Move, guid, "move");

	const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 2u);
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(writes[0].Files, (std::vector<FString> { tree.VcsPath(RockMeta), tree.VcsPath(RockPath) }))
		<< "the meta that is rewritten is checked out where it was, with its source";
	EXPECT_EQ(writes[1].Name, "Move");
	EXPECT_EQ(writes[1].Files, (std::vector<FString> { tree.VcsPath(RockPath), tree.VcsPath(MovedPath), tree.VcsPath(RockMeta),
		tree.VcsPath(MovedMeta), tree.VcsPath(RockBin), tree.VcsPath(MovedBin) }));

	// Back again: the path it holds now is no former path, the one it left is.
	const std::vector<SAssetMove> back = { SAssetMove { FString(MovedPath), FString(RockPath), {} } };
	ASSERT_TRUE(MoveAssets(*tree.Writer, back).IsOk());
	const SAssetMeta meta = tree.ReadMeta(RockMeta).value_or(SAssetMeta());
	EXPECT_EQ(meta.Guid, guid);
	EXPECT_EQ(meta.FormerPaths, std::vector<FString> { FString(MovedPath) });
}

TEST(AssetCommandsTest, ACaseOnlyRenameMovesBothAndRecordsNoFormerPath)
{
	CWriterTree tree("Регістр");
	const FString meta = MakeMetaText(SGuid::NewRandom());
	tree.Write("/Project/скеля.png", "png");
	tree.Write("/Project/скеля.png.meta", meta);
	const std::vector<SAssetMove> rename = { SAssetMove { "/Project/скеля.png", "/Project/Скеля.png", {} } };
	const SAssetCommandResult result = MoveAssets(*tree.Writer, rename);
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	EXPECT_TRUE(tree.IsSpelled("/Project/Скеля.png"));
	EXPECT_TRUE(tree.IsSpelled("/Project/Скеля.png.meta"));
	EXPECT_FALSE(tree.IsSpelled("/Project/скеля.png"));
	EXPECT_FALSE(tree.IsSpelled("/Project/скеля.png.meta"));
	EXPECT_EQ(tree.Read("/Project/Скеля.png.meta"), meta) << "every lookup folds case: nothing to record";

	const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0].Name, "Move");
	EXPECT_EQ(writes[0].Files.size(), 4u) << "asset and meta in one call";
}

TEST(AssetCommandsTest, AMetaSpelledInAnotherCaseMovesAlongAndTakesTheAssetsCase)
{
	CWriterTree tree("Інший регістр");
	const SGuid guid = SGuid::NewRandom();
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/A.PNG.meta", MakeMetaText(guid));
	const std::vector<SAssetMove> move = { SAssetMove { "/Project/a.png", "/Project/b.png", {} } };
	ASSERT_TRUE(MoveAssets(*tree.Writer, move).IsOk());
	EXPECT_FALSE(tree.Exists("/Project/A.PNG.meta"));
	EXPECT_TRUE(tree.IsSpelled("/Project/b.png.meta"));
	EXPECT_EQ(tree.ReadMeta("/Project/b.png.meta").value_or(SAssetMeta()).Guid, guid);
}

TEST(AssetCommandsTest, ABrokenMetaMovesUnchangedWithAWarning)
{
	CWriterTree tree("Зламаний");
	const FStringView broken = "<<<<<<< HEAD\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n=======\nGuid: 6d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n>>>>>>> b\n";
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/a.png.meta", broken);
	const std::vector<SAssetMove> move = { SAssetMove { "/Project/a.png", "/Project/b.png", {} } };
	const SAssetCommandResult result = MoveAssets(*tree.Writer, move);
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	ASSERT_EQ(result.Problems.size(), 1u);
	EXPECT_EQ(result.Problems[0].Severity, ESeverity::Warning);
	EXPECT_EQ(tree.Read("/Project/b.png.meta"), broken);
	EXPECT_EQ(result.Op.PlannedGuids.size(), 2u) << "the salvaged GUIDs are what the move keeps";
}

// This build would write a newer engine's meta in its own, older layout: a move carries it as it is, and a dedupe of it
// is refused with a problem rather than rewriting it.
TEST(AssetCommandsTest, ANewerEnginesMetaMovesUnchangedAndIsNeverRewrittenByADedupe)
{
	CWriterTree tree("Новіший");
	const FStringView newer = "FormatVersion: 2\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\nZone: north\nCook: Auto\n";
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/a.png.meta", newer);
	const std::vector<SAssetMove> move = { SAssetMove { "/Project/a.png", "/Project/b.png", {} } };
	const SAssetCommandResult result = MoveAssets(*tree.Writer, move);
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	ASSERT_EQ(result.Problems.size(), 1u);
	EXPECT_EQ(result.Problems[0].Severity, ESeverity::Warning);
	EXPECT_EQ(tree.Read("/Project/b.png.meta"), newer) << "no FormerPaths written in this build's layout";

	SLifecycleAction dedupe;
	dedupe.Kind = ELifecycleAction::ReGuid;
	dedupe.Path = "/Project/b.png.meta";
	dedupe.Guid = SGuid::NewRandom();
	dedupe.Remap = { { SGuid::Parse("5d1e7a3b9c0f42d68e1a2b3c4d5e6f70").value_or(SGuid {}), dedupe.Guid } };
	CRegistryStore store;
	SAssetOp op;
	std::vector<SDiagnostic> problems;
	EXPECT_FALSE(PlanRepairOp(*tree.Writer, store.Live(), std::span(&dedupe, 1u), op, problems));
	EXPECT_TRUE(op.Steps.empty());
	ASSERT_EQ(problems.size(), 1u);
	EXPECT_TRUE(problems[0].Message.Contains("newer engine")) << problems[0].Message.CStr();
	EXPECT_EQ(tree.Read("/Project/b.png.meta"), newer);
}

TEST(AssetCommandsTest, AMoveThatCannotLandIsRefusedBeforeAnythingMoves)
{
	CWriterTree tree("Нікуди");
	tree.Write("/Project/a.png", "png");
	tree.Write("/Project/a.png.meta", MakeMetaText(SGuid::NewRandom()));
	tree.Write("/Project/b.png", "png");
	tree.Write("/Project/c.png", "png");
	tree.Write("/Project/d.png.meta", MakeMetaText(SGuid::NewRandom()));   // an orphan

	const auto refused = [&tree] (std::vector<SAssetMove> Moves, uint32 Code)
	{
		const SAssetCommandResult result = MoveAssets(*tree.Writer, Moves);
		EXPECT_FALSE(result.IsOk()) << Moves.back().To.CStr();
		ASSERT_FALSE(result.Problems.empty());
		EXPECT_EQ(result.Problems[0].Code, Code) << Moves.back().To.CStr();
		EXPECT_TRUE(result.Op.Steps.empty());
	};
	refused({ SAssetMove { "/Project/a.png", "/Project/b.png", {} } }, 0u);                  // occupied
	refused({ SAssetMove { "/Project/a.png", "/Project/d.png", {} } }, 0u);                  // another file's meta
	refused({ SAssetMove { "/Project/a.png", "/Project/aux.png", {} } }, Path002);           // a device name
	refused({ SAssetMove { "/Project/a.png", "/Project/x.", {} } }, Path002);                // a trailing dot
	refused({ SAssetMove { "/Project/a.png", "/Project/e.png", {} }, SAssetMove { "/Project/c.png", "/Project/E.png", {} } }, 0u);
	refused({ SAssetMove { "/Project/a.png.meta", "/Project/f.png.meta", {} } }, 0u);        // a meta follows its asset

	EXPECT_TRUE(tree.Exists("/Project/a.png"));
	EXPECT_TRUE(tree.Exists("/Project/a.png.meta"));
	EXPECT_TRUE(tree.Exists("/Project/c.png"));
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());
	EXPECT_FALSE(tree.JournalExists());
}


// ---------------- Delete ----------------

TEST(AssetCommandsTest, ADeleteHandsSourceAndMetaToOneCallAndARefusalKeepsBoth)
{
	CWriterTree tree("Видалити");
	WriteRock(tree, SGuid::NewRandom());
	tree.Vcs.Refuse("Delete", EVcsError::LockedByOther);
	const std::vector<SAssetDelete> deletes = { SAssetDelete { FString(RockPath), { FString(RockBin) } } };
	SAssetCommandResult result = DeleteAssets(*tree.Writer, deletes);
	EXPECT_FALSE(result.IsOk());
	ASSERT_EQ(result.Problems.size(), 1u);
	EXPECT_EQ(result.Problems[0].Code, Vcs002);
	EXPECT_TRUE(tree.Exists(RockPath));
	EXPECT_TRUE(tree.Exists(RockMeta));
	EXPECT_FALSE(tree.JournalExists());

	tree.Vcs.Refuse("Delete", EVcsError::None);
	tree.Vcs.ClearCalls();
	result = DeleteAssets(*tree.Writer, deletes);
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	ExpectDone(tree, ECommand::Delete, SGuid(), "delete");
	const std::vector<CRecordingVcsProvider::SCall> writes = tree.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0].Name, "Delete");
	EXPECT_EQ(writes[0].Files, (std::vector<FString> { tree.VcsPath(RockPath), tree.VcsPath(RockMeta), tree.VcsPath(RockBin) }))
		<< "the source before its meta";
}

// Naming a meta deletes it alone only when it is an orphan: the meta of a file that exists, in this spelling or another,
// is that asset's identity.
TEST(AssetCommandsTest, AMetaIsDeletedAloneOnlyWhenItsSourceIsGone)
{
	CWriterTree tree("Видалити мету");
	WriteRock(tree, SGuid::NewRandom());
	tree.Write("/Project/Інша.PNG", "png");
	tree.Write("/Project/Інша.png.meta", MakeMetaText(SGuid::NewRandom()));
	tree.Write("/Project/Сирота.png.meta", MakeMetaText(SGuid::NewRandom()));

	for (const FStringView meta : { FStringView(RockMeta), FStringView("/Project/Інша.png.meta") })
	{
		const std::vector<SAssetDelete> deletes = { SAssetDelete { FString(meta), {} } };
		const SAssetCommandResult result = DeleteAssets(*tree.Writer, deletes);
		EXPECT_FALSE(result.IsOk()) << FString(meta).CStr();
		EXPECT_TRUE(tree.Exists(meta)) << FString(meta).CStr();
	}
	EXPECT_TRUE(tree.Vcs.GetCalls().empty());

	const std::vector<SAssetDelete> orphan = { SAssetDelete { FString("/Project/Сирота.png.meta"), {} } };
	const SAssetCommandResult result = DeleteAssets(*tree.Writer, orphan);
	ASSERT_TRUE(result.IsOk()) << result.Vcs.Message.CStr();
	EXPECT_FALSE(tree.Exists("/Project/Сирота.png.meta"));
}


// ---------------- Kill mid-command ----------------

// Every crash point, in-process: after the journal's Begin and after each step, the writer stops as a crash would leave
// it, and the next start-up's roll-forward finishes the command.
TEST(AssetCommandsTest, EveryCrashPointOfEveryCommandRollsForwardToAConsistentPair)
{
	for (const ECommand command : { ECommand::Create, ECommand::Move, ECommand::Delete })
	{
		size_t stepCount = 0u;
		{
			CWriterTree probe("Кроки");
			PrepareFor(probe, command, SGuid::NewRandom());
			stepCount = RunCommand(*probe.Writer, command, SGuid::NewRandom()).Op.Steps.size();
		}
		ASSERT_GT(stepCount, 0u);
		for (uint32 stop = 0u; stop <= stepCount; ++stop)
		{
			const FString context = FString::Format("command {} stopped after {} of {} steps", static_cast<int>(command), stop, stepCount);
			CWriterTree tree("Збій");
			const SGuid guid = SGuid::NewRandom();
			PrepareFor(tree, command, guid);
			tree.Writer->SetStopAfterStepForTest(stop);
			const SAssetCommandResult stopped = RunCommand(*tree.Writer, command, guid);
			EXPECT_FALSE(stopped.IsOk()) << context.CStr();
			EXPECT_TRUE(stopped.Report.bLeftUnfinished) << context.CStr();

			// A new process: a new writer over the same log.
			const std::unique_ptr<CAssetWriter> restarted = tree.MakeWriter();
			FString error;
			ASSERT_TRUE(restarted->RollForward(&error)) << context.CStr() << " " << error.CStr();
			ExpectDone(tree, command, guid, context);
		}
	}
}

TEST(AssetCommandsTest, AProcessKilledInTheMiddleOfCreateMetaRollsForward)
{
	KillMidCommand(ECommand::Create, "create", 0u);
}

TEST(AssetCommandsTest, AProcessKilledBetweenAnAssetsMoveAndItsMetasRollsForward)
{
	KillMidCommand(ECommand::Move, "move", 1u);
}

TEST(AssetCommandsTest, AProcessKilledBetweenAnAssetsDeleteAndItsMetasRollsForward)
{
	KillMidCommand(ECommand::Delete, "delete", 1u);
}
