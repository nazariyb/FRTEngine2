// The editor's Asset problems panel as Core makes it (Assets/Meta/ProblemList.h, design §12.8, M4's H4; edits [14], [18],
// [33]), through a real registry with a lifecycle of its own: the list is exactly what RunValidation (validate's function)
// returns over every mount, each problem with its asset (a meta's source) and type, grouped by asset in the registry's
// order and filtered by severity, code (a prefix, any case), type and folder (under the fold); "Go to" is a query the
// Assets panel runs to list the asset, its folder or an orphan's folder, and "Open" names the file the problem names;
// the fix-its are the lifecycle's own actions (create, restore, relink, ...), disabled in a mount this process does not
// write, a META001 on /Engine outside engine-owner mode offers the `assets fix --engine` command, a lost meta the
// provider's restore command, an untracked half its add command (absent without a provider) and an LFS pointer a message;
// a fix applied in a live host only queues, and its outcome (fixed, refused with VCS002, no change) comes from a later
// list. Each test holds at most four files and drives the lifecycle with its own clock (Settle), never with sleeps.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "Assets/Vcs/RecordingVcsProvider.h"
#include "TestCommon.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/ProblemList.h"
#include "Assets/Meta/Validation.h"
#include "Assets/Registry/AssetQuery.h"
#include "Assets/Registry/RegistryView.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"

using namespace frt;
using namespace frt::assets;
using frt::test::CRecordingVcsProvider;
using frt::test::rules::CRulesTree;


namespace
{
constexpr FStringLiteral Salt = "Проблеми";
constexpr FStringLiteral GuidA = "5d1e7a3b9c0f42d68e1a2b3c4d5e6f70";
constexpr FStringLiteral GuidB = "3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b";

// A PNG's header as stb_image's info reads it; Size makes the bytes differ.
std::vector<uint8> MakePng (uint32 Size)
{
	std::vector<uint8> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	const auto u32 = [&] (uint32 Value) { png.insert(png.end(), { uint8(Value >> 24u), uint8(Value >> 16u), uint8(Value >> 8u), uint8(Value) }); };
	u32(13u);
	png.insert(png.end(), { 'I', 'H', 'D', 'R' });
	u32(Size);
	u32(Size);
	png.insert(png.end(), { 8u, 6u, 0u, 0u, 0u });
	u32(0u);
	for (const char* type : { "IDAT", "IEND" })
	{
		u32(0u);
		png.insert(png.end(), type, type + 4);
		u32(0u);
	}
	return png;
}

FString MetaText (FStringView Guid)
{
	return FString::Format("FormatVersion: 1\nGuid: {}\n", Guid);
}

FString P (FStringView Relative)
{
	return FString::Format("/Project/{}", Relative);
}

class CProblemTree : public CRulesTree
{
public:
	explicit CProblemTree (FStringView Name)
		: CRulesTree(Name)
	{}

	[[nodiscard]] std::filesystem::path Abs (FStringView Relative) const { return Tree.ProjectContent() / Relative.ToPath(); }
	void WritePng (FStringView Relative, uint32 Size = 4u) const
	{
		frt::test::rules::MakeDirectories(Abs(Relative).parent_path());
		ASSERT_TRUE(platform::WriteFileBytes(Abs(Relative), MakePng(Size)));
	}
	void WriteEnginePng (FStringView Relative, uint32 Size = 4u) const
	{
		const std::filesystem::path path = Tree.EngineContent() / Relative.ToPath();
		frt::test::rules::MakeDirectories(path.parent_path());
		ASSERT_TRUE(platform::WriteFileBytes(path, MakePng(Size)));
	}
	void WriteMeta (FStringView Relative, FStringView Text) const { WriteProjectContent(GetMetaPath(Relative), Text); }
	void Remove (FStringView Relative) const
	{
		std::error_code ec;
		std::filesystem::remove(Abs(Relative), ec);
	}
	[[nodiscard]] bool Exists (FStringView Relative) const
	{
		std::error_code ec;
		return std::filesystem::is_regular_file(Abs(Relative), ec);
	}
	// Automatic repairs off, so each problem stays for its fix-it.
	void StopAutoRepairs () const { WriteProjectRules("FormatVersion: 1\nSafety:\n  AutoRepairMetas: false\n"); }
	[[nodiscard]] FString Vcs (FStringView Relative) const { return FString::FromPath(Abs(Relative).make_preferred()); }
};

// The recording provider with the two commands a person is shown, as Git has them.
class CCommandProvider final : public IVcsProvider
{
public:
	bool Detect (const FString& Root) override { return Inner.Detect(Root); }
	bool IsBusy () const override { return Inner.IsBusy(); }
	SVcsResult CheckOut (std::span<const FString> Files) override { return Inner.CheckOut(Files); }
	SVcsResult Add (std::span<const FString> Files) override { return Inner.Add(Files); }
	SVcsResult Move (std::span<const SVcsMove> Moves) override { return Inner.Move(Moves); }
	SVcsResult Delete (std::span<const FString> Files) override { return Inner.Delete(Files); }
	SVcsResult GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& Out) override { return Inner.GetCommittedFacts(Files, Out); }
	SVcsResult StatusOf (std::span<const FString> Files, std::vector<SVcsStatus>& Out) override { return Inner.StatusOf(Files, Out); }
	SVcsResult GetStaged (std::vector<FString>& Out) override { return Inner.GetStaged(Out); }
	[[nodiscard]] FStringView GetName () const override { return "Probe"; }
	[[nodiscard]] FString GetRestoreCommand (const FString& File) const override { return "probe restore " + File; }
	[[nodiscard]] FString GetAddCommand (const FString& File) const override { return "probe add " + File; }

	CRecordingVcsProvider Inner;
};

jobs::SWorkerPoolDesc MakePoolDesc ()
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = 2u;
	desc.Name = "Problem List Test";
	return desc;
}

/**
 * A registry with a lifecycle over a tree, as the editor will open one: the provider on /Project, the project salt, a writer
 * that repairs (unless Configure says otherwise), the clock moved only by the test.
 */
class CRig
{
public:
	using FConfigure = std::function<void (SLifecycleDesc&, SAssetRegistryDesc&)>;

	CRig (const CProblemTree& InTree, IVcsProvider& Provider, const FConfigure& Configure = {})
		: Tree(InTree)
		, Pool(MakePoolDesc())
	{
		Lifecycle.Providers = { { ProjectMountId, &Provider } };
		Lifecycle.ProjectSalt = FString(Salt);
		Lifecycle.bRepairs = true;
		Desc.Mounts = std::make_shared<const CMountTable>(Tree.Resolve());
		Desc.Pool = &Pool;
		Desc.bWriter = true;
		Desc.bLoadSnapshots = false;
		Desc.bSnapshotAtShutDown = false;
		Desc.Lifecycle = &Lifecycle;
		if (Configure)
		{
			Configure(Lifecycle, Desc);
		}
		Registry = std::make_unique<CAssetRegistry>(Desc);
		Registry->Start();
		bOpened = Registry->WaitForInitialScan(60.0);
		Now = GetLifecycleNowMs();
	}

	~CRig ()
	{
		Registry->ShutDown();
	}

	void Settle (uint64 AdvanceMs = 0u)
	{
		Now += AdvanceMs;
		ASSERT_TRUE(Registry->GetRepairs()->Settle(Now, 30.0));
	}

	void Scan ()
	{
		const FString paths[] = { FString("/Project") };
		ASSERT_TRUE(Registry->ScanPathsSynchronous(paths, 30.0));
	}

	// What the panel lists: on this thread here, on a worker in the editor.
	[[nodiscard]] std::vector<SProblemEntry> List () const { return ListAssetProblems(*Registry->Acquire(), MakeProblemListOptions(*Registry)); }

	[[nodiscard]] SProblemFixContext Context () const { return { &Registry->GetMounts(), Registry->GetRepairs(), &Registry->Live() }; }

	const CProblemTree& Tree;
	jobs::CWorkerPool Pool;
	SLifecycleDesc Lifecycle;
	SAssetRegistryDesc Desc;
	std::unique_ptr<CAssetRegistry> Registry;
	uint64 Now = 0u;
	bool bOpened = false;
};

const SProblemEntry* Find (std::span<const SProblemEntry> Entries, uint32 Code, FStringView Asset)
{
	const auto found = std::find_if(Entries.begin(), Entries.end(), [&] (const SProblemEntry& Entry)
	{
		return Entry.Problem.Code == Code && Entry.Asset == Asset;
	});
	return found != Entries.end() ? &*found : nullptr;
}

FString Describe (std::span<const SProblemEntry> Entries)
{
	FString text;
	for (const SProblemEntry& entry : Entries)
	{
		text += FString::Format("{} {} [{}] {}\n", FormatDiagnosticCode(entry.Problem.Code), entry.Problem.Subject, entry.Asset, entry.Problem.Message);
	}
	return text;
}

std::vector<FString> AssetsOf (std::span<const SProblemGroup> Groups)
{
	std::vector<FString> assets;
	for (const SProblemGroup& group : Groups)
	{
		assets.push_back(group.Asset);
	}
	return assets;
}
}


TEST(ProblemList, TheListIsValidatesWithEachProblemsAssetAndTypeGroupedByAssetAndFiltered)
{
	CProblemTree tree("Список");
	tree.StopAutoRepairs();
	tree.WritePng("Textures/a.png");
	tree.WritePng("Textures/b.png", 5u);
	tree.WriteMeta("Textures/b.png", FString::Format("Guid: {}\nFormatVersion: 1\n", GuidA));   // not canonical: META005
	tree.WritePng("Other/c.png", 6u);
	CRecordingVcsProvider vcs;
	CRig rig(tree, vcs);
	ASSERT_TRUE(rig.bOpened);
	rig.Settle();
	rig.Settle(1600u);

	const std::vector<SProblemEntry> entries = rig.List();
	std::vector<SDiagnostic> validate;
	RunValidation(*rig.Registry->Acquire(), MakeProblemListOptions(*rig.Registry), validate);
	ASSERT_EQ(entries.size(), validate.size()) << Describe(entries).CStr();
	for (size_t i = 0u; i < entries.size(); ++i)
	{
		EXPECT_EQ(entries[i].Problem, validate[i]) << "validate's list, in its order";
	}

	const SProblemEntry* a = Find(entries, Meta001, P("Textures/a.png"));
	ASSERT_NE(a, nullptr) << Describe(entries).CStr();
	EXPECT_FALSE(a->Type.IsEmpty());
	EXPECT_FALSE(a->bFolder);
	const SProblemEntry* b = Find(entries, Meta005, P("Textures/b.png"));
	ASSERT_NE(b, nullptr) << "a meta's problem goes with its source: " << Describe(entries).CStr();
	EXPECT_EQ(b->Problem.Subject, P("Textures/b.png.meta"));
	EXPECT_EQ(b->Type, a->Type);
	const SProblemEntry* root = Find(entries, Mig001, "/Project");
	ASSERT_NE(root, nullptr) << "no Schemas/Migrations.yml: " << Describe(entries).CStr();
	EXPECT_TRUE(root->bFolder);
	EXPECT_TRUE(root->Type.IsEmpty());
	EXPECT_NE(Find(entries, Mig001, "/Engine"), nullptr) << "every mount: validate and validate --engine together";

	// Grouped by asset, in the registry's order (the portable fold, then bytes), the worst severity first in mind.
	const std::vector<SProblemGroup> all = GroupProblems(entries, SProblemFilter {});
	EXPECT_EQ(AssetsOf(all), (std::vector<FString> { "/Engine", "/Project", P("Other/c.png"), P("Textures/a.png"), P("Textures/b.png") }));
	const auto group = [&] (FStringView Asset) { return *std::find_if(all.begin(), all.end(), [&] (const SProblemGroup& G) { return G.Asset == Asset; }); };
	EXPECT_EQ(group(P("Textures/a.png")).Worst, ESeverity::Error);
	EXPECT_EQ(group(P("Textures/b.png")).Worst, ESeverity::Warning);
	EXPECT_EQ(group(P("Textures/b.png")).Type, a->Type);

	SProblemFilter filter;
	filter.bErrors = false;
	EXPECT_EQ(AssetsOf(GroupProblems(entries, filter)), (std::vector<FString> { P("Textures/b.png") })) << "warnings and infos only";
	filter = {};
	filter.Code = "meta00";
	EXPECT_EQ(AssetsOf(GroupProblems(entries, filter)), (std::vector<FString> { P("Other/c.png"), P("Textures/a.png"), P("Textures/b.png") }))
		<< "a code's start, in any case";
	filter.Code = "META005";
	EXPECT_EQ(AssetsOf(GroupProblems(entries, filter)), (std::vector<FString> { P("Textures/b.png") }));
	filter = {};
	filter.Code = "MIG";
	EXPECT_EQ(AssetsOf(GroupProblems(entries, filter)), (std::vector<FString> { "/Engine", "/Project" }));
	filter = {};
	filter.Type = a->Type.FoldCase();
	EXPECT_EQ(GroupProblems(entries, filter).size(), 3u) << "a type under the fold; the roots have none";
	filter.Type = "NoSuchType";
	EXPECT_TRUE(GroupProblems(entries, filter).empty());
	filter = {};
	filter.Folder = "/project/textures/";
	EXPECT_EQ(AssetsOf(GroupProblems(entries, filter)), (std::vector<FString> { P("Textures/a.png"), P("Textures/b.png") }))
		<< "a folder under the fold, a trailing slash ignored";
	EXPECT_EQ(GetProblemTypes(entries), std::vector<FString> { a->Type });
}

TEST(ProblemList, GoToListsTheAssetItsFolderOrAnOrphansFolderAndOpenNamesTheProblemsFile)
{
	CProblemTree tree("Перейти");
	tree.WritePng("Textures/a.png");
	tree.WritePng("Textures/b.png", 5u);
	CRecordingVcsProvider vcs;
	CRig rig(tree, vcs, [] (SLifecycleDesc&, SAssetRegistryDesc& Desc) { Desc.Lifecycle = nullptr; Desc.bWriter = false; });
	ASSERT_TRUE(rig.bOpened);
	const CRegistryView& view = rig.Registry->Live();

	SProblemEntry record;
	record.Asset = P("Textures/a.png");
	record.Type = "Texture";
	EXPECT_EQ(MakeGoToQuery(record), "$Path=\"/Project/Textures/a.png\"");
	std::vector<SRecordId> found;
	EXPECT_EQ(view.RunText(MakeGoToQuery(record), found), std::optional<uint32>(1u));

	SProblemEntry folder;
	folder.Asset = P("Textures");
	folder.bFolder = true;
	EXPECT_EQ(MakeGoToQuery(folder), "dir:\"/Project/Textures\"");
	EXPECT_EQ(view.RunText(MakeGoToQuery(folder), found), std::optional<uint32>(2u));

	SProblemEntry orphan;
	orphan.Asset = P("Textures/Gone.png");
	EXPECT_EQ(MakeGoToQuery(orphan), "dir=\"/Project/Textures\"") << "no record: its folder";
	EXPECT_EQ(view.RunText(MakeGoToQuery(orphan), found), std::optional<uint32>(2u));

	// Quotes and backslashes are escaped as the query reads them back.
	SProblemEntry odd;
	odd.Asset = "/Project/a\"b\\c.png";
	odd.Type = "Texture";
	EXPECT_EQ(MakeGoToQuery(odd), "$Path=\"/Project/a\\\"b\\\\c.png\"");
	SDiagnostic error;
	EXPECT_TRUE(SAssetQuery::Parse(MakeGoToQuery(odd), &error).has_value()) << error.Message.CStr();

	// "Open": the file the problem names, relative to its mount's owner root, else the asset.
	const CMountTable& mounts = rig.Registry->GetMounts();
	SDiagnostic meta001;
	meta001.Subject = P("Textures/a.png");
	meta001.File = "Content/Textures/a.png";
	EXPECT_EQ(GetProblemFilePath(meta001, mounts), std::optional<std::filesystem::path>((tree.Tree.ProjectContent() / "Textures" / "a.png").lexically_normal()));
	SDiagnostic mig001;
	mig001.Subject = "/Project";
	mig001.File = "Schemas/Migrations.yml";
	EXPECT_EQ(GetProblemFilePath(mig001, mounts), std::optional<std::filesystem::path>((tree.Tree.ProjectDir() / "Schemas" / "Migrations.yml").lexically_normal()));
	SDiagnostic broken;
	broken.Subject = P("Textures/a.png.meta");
	const std::optional<std::filesystem::path> asset = GetProblemFilePath(broken, mounts);
	ASSERT_TRUE(asset.has_value());
	EXPECT_EQ(asset->lexically_normal(), (tree.Tree.ProjectContent() / "Textures" / "a.png").lexically_normal());
	SDiagnostic nowhere;
	nowhere.Subject = "/Nowhere/x.png";
	EXPECT_FALSE(GetProblemFilePath(nowhere, mounts).has_value());
}

TEST(ProblemList, FixItsAreTheLifecyclesActionsDisabledWhereThisProcessWritesNothingAndEngineFilesOfferFixEngine)
{
	CProblemTree tree("Виправлення");
	tree.StopAutoRepairs();
	tree.WritePng("a.png");
	tree.WriteEnginePng("Fonts/x.png", 9u);
	const FString engineCommand = FString::Format("\"{}\" assets fix --engine", FString::FromPath((tree.Tree.EngineDir() / "FRT.bat").make_preferred()));
	{
		// A reader (a read-only editor, a guest): every action disabled.
		CRecordingVcsProvider vcs;
		CRig rig(tree, vcs, [] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc& Desc) { Lifecycle.bRepairs = false; Desc.bWriter = false; });
		ASSERT_TRUE(rig.bOpened);
		rig.Settle();
		rig.Settle(1600u);
		const std::vector<SProblemEntry> entries = rig.List();
		const SProblemEntry* project = Find(entries, Meta001, P("a.png"));
		ASSERT_NE(project, nullptr) << Describe(entries).CStr();
		const std::vector<SProblemFixIt> fixIts = GetProblemFixIts(project->Problem, rig.Context());
		ASSERT_EQ(fixIts.size(), 1u);
		EXPECT_EQ(fixIts[0].Kind, EProblemFixKind::Apply);
		EXPECT_FALSE(fixIts[0].Disabled.IsEmpty()) << "a reader writes nothing";
	}

	CRecordingVcsProvider vcs;
	CRig rig(tree, vcs);
	ASSERT_TRUE(rig.bOpened);
	rig.Settle();
	rig.Settle(1600u);
	const std::vector<SProblemEntry> entries = rig.List();
	const SProblemEntry* project = Find(entries, Meta001, P("a.png"));
	const SProblemEntry* engine = Find(entries, Meta001, "/Engine/Fonts/x.png");
	ASSERT_NE(project, nullptr) << Describe(entries).CStr();
	ASSERT_NE(engine, nullptr) << Describe(entries).CStr();

	const std::vector<SProblemFixIt> projectFixes = GetProblemFixIts(project->Problem, rig.Context());
	ASSERT_EQ(projectFixes.size(), 1u);
	EXPECT_EQ(projectFixes[0].Kind, EProblemFixKind::Apply);
	EXPECT_EQ(projectFixes[0].Label, "Create its meta");
	EXPECT_TRUE(projectFixes[0].Disabled.IsEmpty()) << projectFixes[0].Disabled.CStr();
	EXPECT_EQ(projectFixes[0].Action.Kind, ELifecycleAction::CreateMeta);

	// /Engine is not this Project-owner process's (no engine-owner mode): the action is there, disabled, beside the command
	// that fixes it from the engine's own checkout (edit [14]).
	const std::vector<SProblemFixIt> engineFixes = GetProblemFixIts(engine->Problem, rig.Context());
	ASSERT_EQ(engineFixes.size(), 2u);
	EXPECT_EQ(engineFixes[0].Kind, EProblemFixKind::Apply);
	EXPECT_FALSE(engineFixes[0].Disabled.IsEmpty());
	EXPECT_EQ(engineFixes[1].Kind, EProblemFixKind::Copy);
	EXPECT_EQ(engineFixes[1].Text, engineCommand);
	// With no lifecycle at all, only the command.
	const SProblemFixContext none { &rig.Registry->GetMounts(), nullptr, nullptr };
	const std::vector<SProblemFixIt> commandOnly = GetProblemFixIts(engine->Problem, none);
	ASSERT_EQ(commandOnly.size(), 1u);
	EXPECT_EQ(commandOnly[0].Text, engineCommand);
	EXPECT_TRUE(GetProblemFixIts(project->Problem, none).empty());

	// Applied here, in a tool host (not live): at once, and the next list no longer has it.
	const SRepairReport report = rig.Registry->GetRepairs()->ApplyFix(projectFixes[0].Action);
	EXPECT_EQ(report.Applied, 1u);
	EXPECT_TRUE(tree.Exists("a.png.meta"));
	EXPECT_EQ(Find(rig.List(), Meta001, P("a.png")), nullptr);
}

TEST(ProblemList, AFixAppliedInALiveHostOnlyQueuesAndItsOutcomeComesFromALaterList)
{
	CProblemTree tree("Наживо");
	tree.StopAutoRepairs();
	tree.WritePng("a.png");
	tree.WritePng("b.png", 5u);
	CRecordingVcsProvider vcs;
	CRig rig(tree, vcs, [] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc&) { Lifecycle.bLive = true; });
	ASSERT_TRUE(rig.bOpened);
	rig.Settle();
	rig.Settle(1600u);
	const std::vector<SProblemEntry> before = rig.List();
	const SProblemEntry* a = Find(before, Meta001, P("a.png"));
	const SProblemEntry* b = Find(before, Meta001, P("b.png"));
	ASSERT_NE(a, nullptr) << Describe(before).CStr();
	ASSERT_NE(b, nullptr) << Describe(before).CStr();

	const std::vector<SProblemFixIt> fixA = GetProblemFixIts(a->Problem, rig.Context());
	ASSERT_EQ(fixA.size(), 1u);
	const SPendingFix pendingA = MakePendingFix(a->Problem, fixA[0].Label, before, rig.Now);
	const SRepairReport queued = rig.Registry->GetRepairs()->ApplyFix(fixA[0].Action);
	EXPECT_EQ(queued.Planned.size(), 1u);
	EXPECT_EQ(queued.Applied + queued.Refused, 0u) << "a live host only queues";
	EXPECT_EQ(ResolvePendingFix(pendingA, before, rig.Now), EFixOutcome::Pending);
	EXPECT_EQ(ResolvePendingFix(pendingA, before, rig.Now + FixOutcomeTimeoutMs), EFixOutcome::NoChange) << "nothing seen for too long";
	rig.Settle();
	EXPECT_TRUE(tree.Exists("a.png.meta"));
	EXPECT_EQ(ResolvePendingFix(pendingA, rig.List(), rig.Now), EFixOutcome::Fixed);

	// Refused by the VCS: VCS002 about the asset, which was not there when it was applied.
	vcs.Refuse("CheckOut", EVcsError::LockedByOther, "held by a colleague");
	const std::vector<SProblemFixIt> fixB = GetProblemFixIts(b->Problem, rig.Context());
	ASSERT_EQ(fixB.size(), 1u);
	const SPendingFix pendingB = MakePendingFix(b->Problem, fixB[0].Label, rig.List(), rig.Now);
	EXPECT_FALSE(pendingB.bHadRefusal);
	(void)rig.Registry->GetRepairs()->ApplyFix(fixB[0].Action);
	rig.Settle();
	EXPECT_FALSE(tree.Exists("b.png.meta"));
	const std::vector<SProblemEntry> after = rig.List();
	FString message;
	EXPECT_EQ(ResolvePendingFix(pendingB, after, rig.Now, &message), EFixOutcome::Refused) << Describe(after).CStr();
	EXPECT_FALSE(message.IsEmpty());

	// The refusal's own fix-it is the same action again, to retry.
	const SProblemEntry* refusal = Find(after, Vcs002, P("b.png"));
	ASSERT_NE(refusal, nullptr) << Describe(after).CStr();
	const std::vector<SProblemFixIt> retry = GetProblemFixIts(refusal->Problem, rig.Context());
	ASSERT_EQ(retry.size(), 1u);
	EXPECT_TRUE(retry[0].Label.StartsWith("Retry: ")) << retry[0].Label.CStr();
	vcs.Refuse("CheckOut", EVcsError::None);
}

TEST(ProblemList, TheProvidersCommandsAreShownForALostMetaAndAnUntrackedHalfAndAnLfsPointerIsAMessage)
{
	CProblemTree tree("Команди");
	tree.StopAutoRepairs();
	tree.WritePng("a.png");
	tree.WriteMeta("a.png", MetaText(GuidA));
	tree.WritePng("c.png", 5u);
	tree.WriteMeta("c.png", MetaText(GuidB));
	tree.WriteProjectContent("Big.png", "version https://git-lfs.github.com/spec/v1\noid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\nsize 12345\n");
	CCommandProvider vcs;
	vcs.Inner.SetStatus(tree.Vcs("c.png"), SVcsStatus { EVcsState::Clean, {} });   // the source tracked, its meta not: VCS003
	CRig rig(tree, vcs);
	ASSERT_TRUE(rig.bOpened);
	rig.Settle();
	tree.Remove("a.png.meta");
	rig.Scan();
	rig.Settle();
	rig.Settle(1600u);   // past the removal's grace and the new pointer's creation delay
	const std::vector<SProblemEntry> entries = rig.List();

	const SProblemEntry* lost = Find(entries, Meta008, P("a.png"));
	ASSERT_NE(lost, nullptr) << Describe(entries).CStr();
	const std::vector<SProblemFixIt> restore = GetProblemFixIts(lost->Problem, rig.Context());
	ASSERT_EQ(restore.size(), 2u);
	EXPECT_EQ(restore[0].Kind, EProblemFixKind::Apply);
	EXPECT_EQ(restore[0].Action.Kind, ELifecycleAction::RestoreMeta);
	EXPECT_EQ(restore[0].Label, "Restore the verified copy");
	EXPECT_EQ(restore[1].Kind, EProblemFixKind::Copy);
	EXPECT_EQ(restore[1].Label, "Restore from Probe");
	EXPECT_EQ(restore[1].Text, "probe restore " + tree.Vcs("a.png.meta")) << "shown and copied, never run";

	const SProblemEntry* untracked = Find(entries, Vcs003, P("c.png"));
	ASSERT_NE(untracked, nullptr) << Describe(entries).CStr();
	const std::vector<SProblemFixIt> add = GetProblemFixIts(untracked->Problem, rig.Context());
	ASSERT_EQ(add.size(), 1u);
	EXPECT_EQ(add[0].Kind, EProblemFixKind::Copy);
	EXPECT_EQ(add[0].Text, "probe add " + tree.Vcs("c.png.meta")) << "the half the VCS does not track";
	const SProblemFixContext noProvider { &rig.Registry->GetMounts(), nullptr, nullptr };
	EXPECT_TRUE(GetProblemFixIts(untracked->Problem, noProvider).empty()) << "no provider (None): no command";

	const SProblemEntry* pointer = Find(entries, Vcs004, P("Big.png"));
	ASSERT_NE(pointer, nullptr) << Describe(entries).CStr();
	const std::vector<SProblemFixIt> note = GetProblemFixIts(pointer->Problem, rig.Context());
	ASSERT_EQ(note.size(), 1u);
	EXPECT_EQ(note[0].Kind, EProblemFixKind::Note);
	EXPECT_TRUE(note[0].Text.Contains("git lfs pull"));

	// The restore, applied, writes the copy back: the problem and its commands go.
	EXPECT_EQ(rig.Registry->GetRepairs()->ApplyFix(restore[0].Action).Applied, 1u);
	EXPECT_EQ(tree.Exists("a.png.meta"), true);
	EXPECT_EQ(Find(rig.List(), Meta008, P("a.png")), nullptr);
}
