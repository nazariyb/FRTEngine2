// The owner runtime of the meta lifecycle (Assets/Meta/AssetRepairs.h, design §4.6-§4.8, §12.9, M4's F4), end to end on
// small trees through a real registry: a new file gets its derived meta once its delay is over and not before, through
// the provider (asset and meta in one call), and a second round changes nothing; a slow checkout (busy, then the meta a
// little later) never mints a meta; a folder copy re-GUIDs the copies as one op with their references remapped to each
// other; an Explorer move pairs the orphan meta by FileId; a delete then restore of a pair, and a lost meta restored byte
// for byte from METAS, keep the identity; each default-on switch off reduces exactly its repair to a problem with a fix-it
// (and the fix-it, applied, repairs it), WaitForVcs off ignores a busy provider; a meta beside an excluded copy is no
// duplicate; nothing is written while the watcher holds a burst, and a live fix-it is applied while the provider is busy;
// a Safety switch turned back on repairs what it stopped; a refused CheckOut stays a proposal with VCS002 until an event
// on the path or its folder; a cold start tracks only the arrivals that decide something; a meta created beside a
// tracked source is VCS003 (asked in one StatusOf per round, and again later, until the VCS tracks it); a read-only
// process, a mount it does not own and a pending provider detection write nothing, and a provider detected while a run
// is in flight is the one the repairs then use; RunNow has no delays, honours its Kinds (reporting what they leave out)
// and dry run, and keeps the other mounts' fix-its; "keep A, alias B" never overwrites a meta resolved since; an
// unfinished operation is rolled forward before the first scan (and by TakeOwnership); problems carry their file and the
// rule files' Validation: severities; CRLF in a meta is VCS001 and an LFS pointer VCS004. F4b: a second writer is refused
// naming the owner, unless Safety.GuestProcesses makes it a read-only guest that writes nothing and names the owner in its
// META001; a read-only run never asks for the lock; engine-owner mode is a project inside the engine checkout or
// EngineWritable, and in it a file added under a scratch engine's Core/Content gets its meta through a writer of the engine
// root, whose own journal is rolled forward first, while no Engine.snapshot or journal of /Engine is written. Each test holds at most fourteen
// files and drives the lifecycle with its own clock (Settle), never with sleeps: three tests go through the file watcher
// with small delays. The dedupe op's crash points are rolled forward in-process, and one real child is killed. F1: a
// meta that lands between a repair's plan and its write, or after a crash and before the roll-forward, is kept; a dedupe
// set left half written keeps its full remap when the next run plans the rest narrower. F4: a dedupe loser gives its
// Aliases and FormerPaths up to the winner, and a copied referrer of an alias references the copy.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "Assets/Vcs/RecordingVcsProvider.h"
#include "AssetWriterTestUtils.h"
#include "TestCommon.h"
#include "TestProcess.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/BuilderGuard.h"
#include "Assets/FileWatcherService.h"
#include "Assets/Meta/AssetCommands.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/MountPaths.h"
#include "Assets/OwnerLock.h"
#include "Assets/Registry/RegistryRecords.h"
#include "Assets/Registry/RegistryStore.h"
#include "Assets/Vcs/VcsProviderSet.h"
#include "Hash/StableHash.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

using namespace frt;
using namespace frt::assets;
using frt::test::CRecordingVcsProvider;
using frt::test::rules::CRulesTree;


namespace
{
constexpr FStringLiteral Salt = "Ремонт";
constexpr FStringLiteral GuidA = "5d1e7a3b9c0f42d68e1a2b3c4d5e6f70";
constexpr FStringLiteral GuidB = "3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b";

SGuid MakeGuid (FStringView Text)
{
	return SGuid::Parse(Text).value_or(SGuid {});
}

// A PNG's header as stb_image's info reads it (RegistryMetaTest's); Size makes the bytes differ.
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

FString MetaText (FStringView Guid, FStringView Rest = {})
{
	return FString::Format("FormatVersion: 1\nGuid: {}\n{}", Guid, Rest);
}

// "/Project/<Relative>".
FString P (FStringView Relative)
{
	return FString::Format("/Project/{}", Relative);
}

class CRepairTree : public CRulesTree
{
public:
	explicit CRepairTree (FStringView Name)
		: CRulesTree(Name)
	{}

	[[nodiscard]] std::filesystem::path Abs (FStringView Relative) const { return Tree.ProjectContent() / Relative.ToPath(); }
	void WritePng (FStringView Relative, uint32 Size = 4u) const
	{
		frt::test::rules::MakeDirectories(Abs(Relative).parent_path());
		ASSERT_TRUE(platform::WriteFileBytes(Abs(Relative), MakePng(Size)));
	}
	void WriteMeta (FStringView Relative, FStringView Text) const { WriteProjectContent(GetMetaPath(Relative), Text); }
	void Remove (FStringView Relative) const
	{
		std::error_code ec;
		std::filesystem::remove(Abs(Relative), ec);
	}
	void Rename (FStringView From, FStringView To) const
	{
		std::error_code ec;
		std::filesystem::create_directories(Abs(To).parent_path(), ec);
		std::filesystem::rename(Abs(From), Abs(To), ec);
		ASSERT_FALSE(ec) << ec.message();
	}
	[[nodiscard]] bool Exists (FStringView Relative) const
	{
		std::error_code ec;
		return std::filesystem::is_regular_file(Abs(Relative), ec);
	}
	[[nodiscard]] FString Read (FStringView Relative) const
	{
		const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Abs(Relative));
		return bytes.has_value() ? FString(FStringView(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size()))) : FString();
	}
	[[nodiscard]] std::optional<SAssetMeta> ReadMeta (FStringView Relative) const
	{
		if (!Exists(GetMetaPath(Relative)))
		{
			return std::nullopt;
		}
		SMetaParseResult parsed;
		ParseMeta(Read(GetMetaPath(Relative)), parsed);
		return parsed.State == EMetaParseState::Ok ? std::optional<SAssetMeta>(std::move(parsed.Meta)) : std::nullopt;
	}
	[[nodiscard]] SGuid MetaGuid (FStringView Relative) const
	{
		const std::optional<SAssetMeta> meta = ReadMeta(Relative);
		return meta.has_value() ? meta->Guid : SGuid {};
	}
	[[nodiscard]] std::shared_ptr<const CMountTable> Mounts () const { return std::make_shared<const CMountTable>(Resolve()); }
	[[nodiscard]] FString Vcs (FStringView Relative) const { return FString::FromPath(Abs(Relative).make_preferred()); }
};

jobs::SWorkerPoolDesc MakePoolDesc ()
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = 2u;
	desc.Name = "Asset Repairs Test";
	return desc;
}

/**
 * A registry with a lifecycle over a tree: the recording provider on /Project, the default delays, the project salt, a
 * writer that repairs. Configure changes the desc before the registry opens; the clock starts at the wall clock (so the
 * registry's own tombstone times compare) and moves only when a test says so.
 */
class CRig
{
public:
	using FConfigure = std::function<void (SLifecycleDesc&, SAssetRegistryDesc&)>;

	explicit CRig (const CRepairTree& InTree, const FConfigure& Configure = {})
		: Tree(InTree)
		, Pool(MakePoolDesc())
	{
		Lifecycle.Providers = { { ProjectMountId, &Vcs } };
		Lifecycle.ProjectSalt = FString(Salt);
		Lifecycle.bRepairs = true;
		Desc.Mounts = Tree.Mounts();
		Desc.Pool = &Pool;
		Desc.bWriter = true;
		Desc.bLoadSnapshots = false;
		Desc.bSnapshotAtShutDown = false;
		Desc.Lifecycle = &Lifecycle;
		if (Configure)
		{
			Configure(Lifecycle, Desc);
		}
	}

	~CRig ()
	{
		if (Registry != nullptr)
		{
			Registry->ShutDown();
		}
	}

	void Open ()
	{
		Registry = std::make_unique<CAssetRegistry>(Desc);
		Registry->Start();
		ASSERT_TRUE(Registry->WaitForInitialScan(60.0));
		Now = GetLifecycleNowMs();
	}

	// Moves the clock by AdvanceMs and lets the lifecycle do all that is due then.
	void Settle (uint64 AdvanceMs = 0u)
	{
		Now += AdvanceMs;
		ASSERT_TRUE(Repairs().Settle(Now, 30.0));
	}

	void Scan (FStringView MountPath = "/Project")
	{
		const FString paths[] = { FString(MountPath) };
		ASSERT_TRUE(Registry->ScanPathsSynchronous(paths, 30.0));
	}

	[[nodiscard]] CAssetRepairs& Repairs () { return *Registry->GetRepairs(); }
	[[nodiscard]] const CRegistryView& View () const { return Registry->Live(); }

	[[nodiscard]] SGuid GuidAt (FStringView MountPath) const
	{
		const std::optional<SStoredFile> stored = FindStoredFile(View(), MountPath);
		return stored.has_value() ? stored->Guid : SGuid {};
	}

	[[nodiscard]] EMetaState StateOf (FStringView MountPath) const
	{
		const std::optional<SStoredFile> stored = FindStoredFile(View(), MountPath);
		return stored.has_value() ? stored->MetaState : EMetaState::PendingMeta;
	}

	[[nodiscard]] std::vector<SDiagnostic> Problems (uint32 Code = 0u) const
	{
		std::vector<SDiagnostic> found;
		View().ForEachProblem([&] (FStringView, EProblemSource Source, const SDiagnostic& Diagnostic)
		{
			if (Source == EProblemSource::Lifecycle && (Code == 0u || Diagnostic.Code == Code))
			{
				found.push_back(Diagnostic);
			}
		});
		return found;
	}

	[[nodiscard]] FString DescribeProblems () const
	{
		FString text;
		for (const SDiagnostic& problem : Problems())
		{
			text += FString::Format("{} {}: {}\n", FormatDiagnosticCode(problem.Code), problem.Subject, problem.Message);
		}
		return text;
	}

	const CRepairTree& Tree;
	jobs::CWorkerPool Pool;
	CRecordingVcsProvider Vcs;
	SLifecycleDesc Lifecycle;
	SAssetRegistryDesc Desc;
	std::unique_ptr<CAssetRegistry> Registry;
	uint64 Now = 0u;
};

SGuid DerivedGuid (FStringView MountPath, uint32 PngSize = 4u)
{
	const std::vector<uint8> png = MakePng(PngSize);
	return MakeCreateMetaGuid(Salt, MountPath, hash::HashContent(png));
}

void WriteSafety (const CRepairTree& Tree, FStringView Switch)
{
	Tree.WriteProjectRules(FString::Format("FormatVersion: 1\nSafety:\n  {}: false\n", Switch));
}
}


// ---------------- New files ----------------

TEST(AssetRepairs, ANewFileGetsItsDerivedMetaOnceItsDelayIsOverThroughTheProviderAndASecondRoundChangesNothing)
{
	CRepairTree tree("Новий файл");
	tree.WritePng("Textures/Камінь.png");
	CRig rig(tree);
	rig.Open();
	const FString path = P("Textures/Камінь.png");

	rig.Settle();
	EXPECT_FALSE(tree.Exists("Textures/Камінь.png.meta")) << "the VCS may deliver it yet";
	EXPECT_EQ(rig.StateOf(path), EMetaState::PendingMeta);
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());

	rig.Settle(1600u);
	const SGuid expected = DerivedGuid(path);
	EXPECT_EQ(tree.MetaGuid("Textures/Камінь.png"), expected);
	EXPECT_EQ(rig.GuidAt(path), expected);
	EXPECT_EQ(rig.StateOf(path), EMetaState::Paired);
	const std::vector<CRecordingVcsProvider::SCall> writes = rig.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 2u);
	const std::vector<FString> pair = { tree.Vcs("Textures/Камінь.png.meta"), tree.Vcs("Textures/Камінь.png") };
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(writes[0].Files, pair);
	EXPECT_EQ(writes[1].Name, "Add");
	EXPECT_EQ(writes[1].Files, pair);
	EXPECT_EQ(rig.Registry->GetCounters().RepairsApplied, 1u);
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();

	// Level-triggered: the echo's scan sees the bytes it holds and proposes nothing.
	rig.Settle(5000u);
	EXPECT_EQ(rig.Vcs.GetWriteCalls().size(), 2u);
	EXPECT_EQ(rig.Registry->GetCounters().RepairsApplied, 1u);
}

TEST(AssetRepairs, ASlowCheckoutNeverMintsAMetaWhileTheProviderIsBusyOrTheMetaIsStillOnItsWay)
{
	CRepairTree tree("Повільно");
	CRig rig(tree);
	rig.Open();
	rig.Settle();

	// Busy: the source arrives, and well past its delay nothing is written.
	rig.Vcs.SetBusy(true);
	tree.WritePng("Textures/a.png");
	rig.Scan();
	rig.Settle();
	rig.Settle(3000u);
	EXPECT_FALSE(tree.Exists("Textures/a.png.meta"));
	// The VCS writes the meta, then goes quiet; the settle window still holds.
	tree.WriteMeta("Textures/a.png", MetaText(GuidA));
	rig.Scan();
	rig.Settle(200u);
	rig.Vcs.SetBusy(false);
	rig.Settle(200u);
	rig.Settle(3000u);
	EXPECT_EQ(tree.MetaGuid("Textures/a.png"), MakeGuid(GuidA));

	// Not busy: a meta that arrives inside the creation delay is the one that stays.
	tree.WritePng("Textures/b.png", 5u);
	rig.Scan();
	rig.Settle();
	rig.Settle(1000u);
	EXPECT_FALSE(tree.Exists("Textures/b.png.meta"));
	tree.WriteMeta("Textures/b.png", MetaText(GuidB));
	rig.Scan();
	rig.Settle(3000u);
	EXPECT_EQ(tree.MetaGuid("Textures/b.png"), MakeGuid(GuidB));
	EXPECT_EQ(rig.GuidAt(P("Textures/b.png")), MakeGuid(GuidB));
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty()) << "no CreateMeta at any step";
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

// Only the VCS of a mount the repair writes holds it (edit [56]): /Engine, read-only to a project's writer, may sit in
// another checkout that is mid-rebase (a REBASE_HEAD git left behind is busy too, §12.9), and its project's new file
// still gets its meta.
TEST(AssetRepairs, ABusyProviderOfAMountThisProcessDoesNotRepairHoldsNothing)
{
	CRepairTree tree("Чужий рушій");
	tree.WritePng("a.png");
	CRecordingVcsProvider engineVcs;
	CRig rig(tree, [&engineVcs] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc&) { Lifecycle.Providers.emplace_back(EngineMountId, &engineVcs); });
	rig.Open();
	engineVcs.SetBusy(true);
	rig.Settle();
	rig.Settle(1600u);
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png"))) << "the busy provider is /Engine's, which this process does not write";
	EXPECT_TRUE(engineVcs.GetWriteCalls().empty());
}

TEST(AssetRepairs, WaitForVcsOffIgnoresABusyProvider)
{
	CRepairTree tree("Без очікування");
	tree.WritePng("a.png");
	CRig rig(tree, [] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc&) { Lifecycle.Settings.bWaitForVcs = false; });
	rig.Open();
	rig.Vcs.SetBusy(true);
	rig.Settle();
	rig.Settle(1600u);
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png"))) << "with WaitForVcs off, repairs wait only for their own delays";
}

// ---------------- Copies, moves, deletes ----------------

namespace
{
// Pack/Rock.png's meta remaps its material to Pack/Skin.png (SubAssets.Remap), as an extracted material would be.
void WritePack (const CRepairTree& Tree)
{
	Tree.WritePng("Pack/Rock.png", 4u);
	Tree.WriteMeta("Pack/Rock.png", MetaText(GuidA, FString::Format("SubAssets:\n  Remap:\n    \"material:Skin\": {{ $asset: {} }}\n", GuidB)));
	Tree.WritePng("Pack/Skin.png", 5u);
	Tree.WriteMeta("Pack/Skin.png", MetaText(GuidB));
}

void CopyPack (const CRepairTree& Tree)
{
	std::error_code ec;
	std::filesystem::copy(Tree.Abs("Pack"), Tree.Abs("Pack2"), std::filesystem::copy_options::recursive, ec);
	ASSERT_FALSE(ec) << ec.message();
}

[[nodiscard]] FString RemapTarget (const SAssetMeta& Meta)
{
	return FString(Meta.SubAssets.GetRoot().Find("Remap").FindChild("material:Skin").FindChild("$asset").GetText());
}
}

TEST(AssetRepairs, AFolderCopyReGuidsTheCopiesInOneOpAndTheCopyReferencesOnlyTheCopy)
{
	CRepairTree tree("Копія теки");
	WritePack(tree);
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	CopyPack(tree);
	rig.Scan("/Project/Pack2");
	rig.Settle();
	EXPECT_EQ(tree.MetaGuid("Pack2/Rock.png"), MakeGuid(GuidA)) << "not before the batch has settled";
	rig.Settle(1600u);

	// The originals win (the smallest folded path: nothing is committed under the recording provider).
	EXPECT_EQ(tree.MetaGuid("Pack/Rock.png"), MakeGuid(GuidA));
	EXPECT_EQ(tree.MetaGuid("Pack/Skin.png"), MakeGuid(GuidB));
	const SGuid rock = MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Pack2/Rock.png"));
	const SGuid skin = MakeDedupeGuid(Salt, MakeGuid(GuidB), P("Pack2/Skin.png"));
	const std::optional<SAssetMeta> copied = tree.ReadMeta("Pack2/Rock.png");
	ASSERT_TRUE(copied.has_value());
	EXPECT_EQ(copied->Guid, rock);
	EXPECT_EQ(RemapTarget(*copied), skin.ToString()) << "the copy references the copy";
	EXPECT_EQ(RemapTarget(*tree.ReadMeta("Pack/Rock.png")), FString(GuidB)) << "the original keeps its reference";
	EXPECT_EQ(tree.MetaGuid("Pack2/Skin.png"), skin);
	EXPECT_EQ(rig.GuidAt(P("Pack2/Rock.png")), rock);
	EXPECT_EQ(rig.GuidAt(P("Pack/Rock.png")), MakeGuid(GuidA));

	// One op: one CheckOut of both losers' pairs, nothing moved or deleted.
	const std::vector<CRecordingVcsProvider::SCall> writes = rig.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(writes[0].Files.size(), 4u);
	EXPECT_EQ(rig.Registry->GetCounters().RepairsApplied, 2u);
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
	EXPECT_FALSE(rig.View().FindTombstone(MakeGuid(GuidA)).has_value()) << "a live GUID gets no tombstone";
}

// The redirects belong to the GUID the winner keeps: a copy that sorts first ("Pack - Copy" before "Pack") must not take
// them, or every lookup through the alias or the old path would answer the copy. A copied referrer of the alias (Skin's
// Remap names C, an alias of Rock) references the copy of Rock.
TEST(AssetRepairs, ADedupeLoserGivesItsAliasesAndFormerPathsToTheWinner)
{
	CRepairTree tree("Перенаправлення");
	constexpr FStringLiteral GuidC = "9a0b1c2d3e4f40518293a4b5c6d7e8f9";
	tree.WritePng("Pack/Rock.png", 4u);
	tree.WriteMeta("Pack/Rock.png", MetaText(GuidA, FString::Format("Aliases: [ {} ]\nFormerPaths: [ /Project/Old/Rock.png ]\n", GuidC)));
	tree.WritePng("Pack/Skin.png", 5u);
	tree.WriteMeta("Pack/Skin.png", MetaText(GuidB, FString::Format("SubAssets:\n  Remap:\n    \"material:Skin\": {{ $asset: {} }}\n", GuidC)));
	CRig rig(tree);
	rig.Open();
	const FString committed[] = { tree.Vcs("Pack/Rock.png.meta"), tree.Vcs("Pack/Skin.png.meta") };
	rig.Vcs.SetCommitted(committed);
	rig.Settle();
	std::error_code ec;
	std::filesystem::copy(tree.Abs("Pack"), tree.Abs("Pack - Copy"), std::filesystem::copy_options::recursive, ec);
	ASSERT_FALSE(ec) << ec.message();
	rig.Scan("/Project/Pack - Copy");
	rig.Settle();
	rig.Settle(1600u);
	rig.Settle(100u);

	EXPECT_EQ(tree.MetaGuid("Pack/Rock.png"), MakeGuid(GuidA)) << "the committed original wins";
	const std::optional<SAssetMeta> copy = tree.ReadMeta("Pack - Copy/Rock.png");
	ASSERT_TRUE(copy.has_value());
	const SGuid rockCopy = MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Pack - Copy/Rock.png"));
	EXPECT_EQ(copy->Guid, rockCopy);
	EXPECT_TRUE(copy->Aliases.empty());
	EXPECT_TRUE(copy->FormerPaths.empty());
	EXPECT_TRUE(copy->FormerNames.empty());
	EXPECT_EQ(tree.ReadMeta("Pack/Rock.png")->Aliases, std::vector<SGuid> { MakeGuid(GuidC) }) << "the winner keeps them";
	EXPECT_EQ(RemapTarget(*tree.ReadMeta("Pack - Copy/Skin.png")), rockCopy.ToString()) << "the copied referrer references the copy";
	EXPECT_EQ(RemapTarget(*tree.ReadMeta("Pack/Skin.png")), FString(GuidC));

	const std::optional<SRecordId> original = rig.View().FindByPath(P("Pack/Rock.png"));
	ASSERT_TRUE(original.has_value());
	EXPECT_EQ(rig.View().Find(SAssetId { MakeGuid(GuidC), 0u }), original) << "the alias answers the original";
	EXPECT_EQ(rig.View().FindByPath("/Project/Old/Rock.png"), original) << "the former path answers the original";
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, AnExplorerMovePairsTheOrphanMetaByFileIdAndMovesIt)
{
	CRepairTree tree("Переміщення");
	tree.WritePng("Textures/a.png");
	const FString metaText = MetaText(GuidA, "Labels:\n  - Rock\n");
	tree.WriteMeta("Textures/a.png", metaText);
	CRig rig(tree);
	rig.Open();
	rig.Settle();

	tree.Rename("Textures/a.png", "Moved/a.png");
	rig.Scan();
	rig.Settle();
	EXPECT_TRUE(rig.Problems(Meta002).empty()) << "the new file still in its delay holds the orphan's report";
	rig.Settle(1600u);
	EXPECT_FALSE(tree.Exists("Textures/a.png.meta"));
	EXPECT_EQ(tree.Read("Moved/a.png.meta"), metaText) << "moved, not rewritten";
	EXPECT_EQ(rig.GuidAt(P("Moved/a.png")), MakeGuid(GuidA));
	EXPECT_FALSE(rig.View().FindTombstone(MakeGuid(GuidA)).has_value());
	const std::vector<CRecordingVcsProvider::SCall> writes = rig.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0].Name, "Move");
	EXPECT_EQ(writes[0].Files, (std::vector<FString> { tree.Vcs("Textures/a.png.meta"), tree.Vcs("Moved/a.png.meta") }));
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, ADeleteThenRestoreOfAPairKeepsItsIdentityAndClearsItsTombstone)
{
	CRepairTree tree("Видалити й повернути");
	tree.WritePng("a.png");
	tree.WriteMeta("a.png", MetaText(GuidA));
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	const std::filesystem::path bin = tree.Tree.Root / "Кошик";
	std::error_code ec;
	std::filesystem::create_directories(bin, ec);
	std::filesystem::rename(tree.Abs("a.png"), bin / "a.png", ec);
	std::filesystem::rename(tree.Abs("a.png.meta"), bin / "a.png.meta", ec);
	rig.Scan();
	rig.Settle();
	EXPECT_TRUE(rig.View().FindTombstone(MakeGuid(GuidA)).has_value());
	rig.Settle(3000u);

	std::filesystem::rename(bin / "a.png", tree.Abs("a.png"), ec);
	std::filesystem::rename(bin / "a.png.meta", tree.Abs("a.png.meta"), ec);
	rig.Scan();
	rig.Settle();
	rig.Settle(3000u);
	EXPECT_EQ(rig.GuidAt(P("a.png")), MakeGuid(GuidA));
	EXPECT_EQ(rig.StateOf(P("a.png")), EMetaState::Paired);
	EXPECT_FALSE(rig.View().FindTombstone(MakeGuid(GuidA)).has_value());
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, ALostMetaIsRestoredByteForByteFromItsVerifiedCopyAfterTheGrace)
{
	CRepairTree tree("Втрачений");
	tree.WritePng("a.png");
	const FString metaText = MetaText(GuidA, "Labels:\n  - Камінь\n");
	tree.WriteMeta("a.png", metaText);
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	tree.Remove("a.png.meta");
	rig.Scan();
	rig.Settle();
	EXPECT_FALSE(tree.Exists("a.png.meta")) << "a VCS sync may put it back";
	rig.Settle(1100u);
	EXPECT_EQ(tree.Read("a.png.meta"), metaText);
	EXPECT_EQ(rig.GuidAt(P("a.png")), MakeGuid(GuidA));
	EXPECT_EQ(rig.StateOf(P("a.png")), EMetaState::Paired);
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

// ---------------- Each default-on switch off, end to end ----------------

TEST(AssetRepairs, AutoRepairMetasOffStopsANewFileAtMeta001WithAFixItThatCreatesTheMeta)
{
	CRepairTree tree("Без ремонту");
	WriteSafety(tree, "AutoRepairMetas");
	tree.WritePng("Textures/a.png");
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	EXPECT_FALSE(tree.Exists("Textures/a.png.meta"));
	const std::vector<SDiagnostic> meta001 = rig.Problems(Meta001);
	ASSERT_EQ(meta001.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(meta001[0].Severity, ESeverity::Error);
	EXPECT_EQ(meta001[0].Subject, P("Textures/a.png"));
	EXPECT_EQ(meta001[0].File, "Content/Textures/a.png");
	const std::vector<SLifecycleAction> fixIts = rig.Repairs().GetFixIts(meta001[0]);
	ASSERT_EQ(fixIts.size(), 1u);
	EXPECT_EQ(fixIts[0].Kind, ELifecycleAction::CreateMeta);

	const SRepairReport report = rig.Repairs().ApplyFix(fixIts[0]);
	EXPECT_EQ(report.Applied, 1u);
	EXPECT_EQ(tree.MetaGuid("Textures/a.png"), DerivedGuid(P("Textures/a.png")));
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, DerivedAutoGuidsOffMintsRandomGuidsAndStillRemapsACopiedFolderToItsCopies)
{
	CRepairTree tree("Випадкові");
	WriteSafety(tree, "DerivedAutoGuids");
	tree.WritePng("New.png", 7u);
	WritePack(tree);
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	CopyPack(tree);
	rig.Scan("/Project/Pack2");
	rig.Settle();
	rig.Settle(1600u);

	const SGuid created = tree.MetaGuid("New.png");
	EXPECT_TRUE(created.IsValid());
	EXPECT_NE(created, DerivedGuid(P("New.png"), 7u));
	const std::optional<SAssetMeta> rock = tree.ReadMeta("Pack2/Rock.png");
	ASSERT_TRUE(rock.has_value());
	const SGuid skin = tree.MetaGuid("Pack2/Skin.png");
	EXPECT_NE(rock->Guid, MakeGuid(GuidA));
	EXPECT_NE(rock->Guid, MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Pack2/Rock.png")));
	EXPECT_NE(skin, MakeGuid(GuidB));
	EXPECT_NE(skin, MakeDedupeGuid(Salt, MakeGuid(GuidB), P("Pack2/Skin.png")));
	EXPECT_EQ(RemapTarget(*rock), skin.ToString()) << "the stand-in replaced by the GUID minted for Skin";
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, AutoResolveDuplicatesOffReportsMeta003WithReGuidFixItsAppliedAsASet)
{
	CRepairTree tree("Без дедуплікації");
	WriteSafety(tree, "AutoResolveDuplicates");
	WritePack(tree);
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	CopyPack(tree);
	rig.Scan("/Project/Pack2");
	rig.Settle();
	rig.Settle(1600u);
	EXPECT_EQ(tree.MetaGuid("Pack2/Rock.png"), MakeGuid(GuidA));
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());
	const std::vector<SDiagnostic> meta003 = rig.Problems(Meta003);
	ASSERT_EQ(meta003.size(), 2u) << rig.DescribeProblems().CStr();
	const auto rockProblem = std::find_if(meta003.begin(), meta003.end(), [] (const SDiagnostic& Problem) { return Problem.Subject == P("Pack2/Rock.png"); });
	ASSERT_NE(rockProblem, meta003.end());
	const std::vector<SLifecycleAction> fixIts = rig.Repairs().GetFixIts(*rockProblem);
	ASSERT_EQ(fixIts.size(), 1u);
	EXPECT_EQ(fixIts[0].Kind, ELifecycleAction::ReGuid);

	const SRepairReport report = rig.Repairs().ApplyFix(fixIts[0]);
	EXPECT_EQ(report.Applied, 2u) << "both losers, so the copy's reference follows";
	const std::optional<SAssetMeta> rock = tree.ReadMeta("Pack2/Rock.png");
	ASSERT_TRUE(rock.has_value());
	EXPECT_EQ(rock->Guid, MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Pack2/Rock.png")));
	EXPECT_EQ(RemapTarget(*rock), tree.MetaGuid("Pack2/Skin.png").ToString());
	EXPECT_TRUE(rig.Problems(Meta003).empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, OrphanPairingOffKeepsAMovedFileInMissingMetaWithARelinkFixItAndNeverMintsAnIdentity)
{
	CRepairTree tree("Без пар");
	WriteSafety(tree, "OrphanPairing");
	tree.WritePng("Textures/a.png");
	tree.WriteMeta("Textures/a.png", MetaText(GuidA));
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	tree.Rename("Textures/a.png", "Moved/a.png");
	rig.Scan();
	rig.Settle();
	rig.Settle(1600u);
	rig.Settle(10000u);
	EXPECT_FALSE(tree.Exists("Moved/a.png.meta"));
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());
	EXPECT_EQ(rig.StateOf(P("Moved/a.png")), EMetaState::MissingMeta);
	const std::vector<SDiagnostic> meta008 = rig.Problems(Meta008);
	ASSERT_EQ(meta008.size(), 1u) << rig.DescribeProblems().CStr();
	const std::vector<SLifecycleAction> fixIts = rig.Repairs().GetFixIts(meta008[0]);
	ASSERT_FALSE(fixIts.empty());
	EXPECT_EQ(fixIts[0].Kind, ELifecycleAction::MoveMeta);
	EXPECT_EQ(fixIts[0].Path, P("Textures/a.png.meta"));

	EXPECT_EQ(rig.Repairs().ApplyFix(fixIts[0]).Applied, 1u);
	EXPECT_EQ(tree.MetaGuid("Moved/a.png"), MakeGuid(GuidA));
	EXPECT_EQ(rig.GuidAt(P("Moved/a.png")), MakeGuid(GuidA));
	EXPECT_TRUE(rig.Problems(Meta008).empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, AMetaBesideAnExcludedCopyIsNoDuplicateAndTheAssetKeepsItsGuid)
{
	CRepairTree tree("Виключена копія");
	tree.WritePng("B/Rock.png");
	tree.WriteMeta("B/Rock.png", MetaText(GuidA));
	// A copy of the pair where a rule excludes textures, at a path that sorts first: not an asset, so never the winner.
	tree.WritePng("A/Rock.png");
	tree.WriteMeta("A/Rock.png", MetaText(GuidA));
	tree.WriteProjectContent("A/Assets.yml", "FormatVersion: 1\nScan: { Exclude: [ \"*.png\" ] }\n");
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	ASSERT_FALSE(rig.View().FindFileExact(P("A/Rock.png")).has_value()) << "excluded: no FILES row";
	ASSERT_TRUE(rig.View().FindFileExact(P("A/Rock.png.meta")).has_value());
	EXPECT_EQ(tree.MetaGuid("B/Rock.png"), MakeGuid(GuidA));
	EXPECT_EQ(rig.GuidAt(P("B/Rock.png")), MakeGuid(GuidA));
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, NoMetaIsCreatedWhileTheWatcherHoldsABurst)
{
	CRepairTree tree("Сплеск");
	SFileWatcherSettings settings;
	settings.SettleMs = 200u;
	settings.BurstEvents = 8u;
	settings.VcsSettleMs = 60000u;   // held for the rest of the test
	CFileWatcherService watcher(settings);
	CRig rig(tree, [&] (SLifecycleDesc&, SAssetRegistryDesc& Desc) { Desc.Watcher = &watcher; });
	for (const SMount& mount : rig.Desc.Mounts->GetMounts())
	{
		watcher.AddMount(mount.ContentDir);
		watcher.AddMount(mount.RulesFile.parent_path());
	}
	rig.Open();
	rig.Settle();
	const auto pump = [&]
	{
		watcher.Pump();
		rig.Pool.PumpCompletions();
		rig.Registry->Pump();
	};

	// a.png arrives on its own; then a sync's burst starts, which the watcher holds (its meta may be in it).
	tree.WritePng("a.png");
	ASSERT_TRUE(test::WaitUntil([&] { pump(); return FindStoredFile(rig.View(), P("a.png")).has_value(); }));
	rig.Settle();
	for (uint32 i = 0u; i < 12u; ++i)
	{
		tree.WritePng(FString::Format("Sync/{}.png", i), 10u + i);
	}
	ASSERT_TRUE(test::WaitUntil([&] { watcher.Pump(); return watcher.GetStats().bHolding; }));
	rig.Settle(3000u);
	EXPECT_FALSE(tree.Exists("a.png.meta")) << "the tree the watcher holds back is not the final one";
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());
	rig.Registry->ShutDown();
	rig.Registry.reset();
}

TEST(AssetRepairs, AFixItAppliedInALiveHostWhileTheProviderIsBusyIsStillApplied)
{
	CRepairTree tree("Виправлення наживо");
	WriteSafety(tree, "AutoRepairMetas");
	tree.WritePng("a.png");
	CRig rig(tree, [] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc&) { Lifecycle.bLive = true; });
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	const std::vector<SDiagnostic> meta001 = rig.Problems(Meta001);
	ASSERT_EQ(meta001.size(), 1u) << rig.DescribeProblems().CStr();
	const std::vector<SLifecycleAction> fixIts = rig.Repairs().GetFixIts(meta001[0]);
	ASSERT_EQ(fixIts.size(), 1u);

	// A person's fix is a command: the hold of automatic repairs does not apply to it.
	rig.Vcs.SetBusy(true);
	const SRepairReport report = rig.Repairs().ApplyFix(fixIts[0]);
	EXPECT_EQ(report.Planned.size(), 1u);
	rig.Settle();
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png")));
	EXPECT_EQ(rig.Registry->GetCounters().RepairsApplied, 1u);
	rig.Vcs.SetBusy(false);
}

// A mount's Safety switch is no input of any file's row (unlike a folder's Validation:, which analyzes its files again),
// so its re-evaluation commits no row: the lifecycle must run because the rules changed.
TEST(AssetRepairs, ASafetySwitchTurnedBackOnRepairsWhatItHadStoppedThoughNoFileChanged)
{
	CRepairTree tree("Нові правила");
	WriteSafety(tree, "AutoRepairMetas");
	tree.WritePng("Textures/a.png");
	SFileWatcherSettings settings;
	settings.SettleMs = 20u;
	settings.VcsSettleMs = 30u;
	CFileWatcherService watcher(settings);
	CRig rig(tree, [&] (SLifecycleDesc&, SAssetRegistryDesc& Desc) { Desc.Watcher = &watcher; });
	for (const SMount& mount : rig.Desc.Mounts->GetMounts())
	{
		watcher.AddMount(mount.ContentDir);
		watcher.AddMount(mount.RulesFile.parent_path());
	}
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	ASSERT_EQ(rig.Problems(Meta001).size(), 1u) << rig.DescribeProblems().CStr();

	// The rules reload and their rescan (one call) are done once the switch reads on and the registry is idle.
	tree.WriteProjectRules("FormatVersion: 1\n");
	ASSERT_TRUE(test::WaitUntil([&]
	{
		watcher.Pump();
		rig.Pool.PumpCompletions();
		rig.Registry->Pump();
		return rig.Registry->GetRules().GetSafety("Project").bAutoRepairMetas && rig.Registry->IsIdle();
	}));
	rig.Settle();
	EXPECT_EQ(tree.MetaGuid("Textures/a.png"), DerivedGuid(P("Textures/a.png")));
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
	rig.Registry->ShutDown();
	rig.Registry.reset();
}

// ---------------- Refusals and the VCS ----------------

TEST(AssetRepairs, ARefusedCheckOutStaysAProposalWithVcs002UntilAnEventOnThePath)
{
	CRepairTree tree("Відмова");
	tree.WritePng("a.png");
	CRig rig(tree);
	rig.Open();
	rig.Vcs.Refuse("CheckOut", EVcsError::LockedByOther, "held by a colleague");
	rig.Settle();
	rig.Settle(1600u);
	EXPECT_FALSE(tree.Exists("a.png.meta"));
	const std::vector<SDiagnostic> vcs002 = rig.Problems(Vcs002);
	ASSERT_EQ(vcs002.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(vcs002[0].Subject, P("a.png"));
	EXPECT_EQ(rig.Registry->GetCounters().RepairsRefused, 1u);
	ASSERT_EQ(rig.Repairs().GetFixIts(vcs002[0]).size(), 1u);

	// Not retried on its own: a read-only file cannot spin.
	const size_t calls = rig.Vcs.GetCalls().size();
	rig.Settle(5000u);
	rig.Settle(5000u);
	EXPECT_EQ(rig.Vcs.GetCalls().size(), calls);

	// An event on the path proposes it again.
	rig.Vcs.Refuse("CheckOut", EVcsError::None);
	tree.WritePng("a.png", 6u);
	rig.Scan();
	rig.Settle();
	rig.Settle(1600u);
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png"), 6u));
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, ARefusalGoesWithTheFolderItWasIn)
{
	CRepairTree tree("Відмова в теці");
	tree.WritePng("Sub/a.png");
	CRig rig(tree);
	rig.Open();
	rig.Vcs.Refuse("CheckOut", EVcsError::LockedByOther, "held by a colleague");
	rig.Settle();
	rig.Settle(1600u);
	ASSERT_EQ(rig.Problems(Vcs002).size(), 1u) << rig.DescribeProblems().CStr();
	std::error_code ec;
	std::filesystem::remove_all(tree.Abs("Sub"), ec);
	ASSERT_FALSE(ec) << ec.message();
	rig.Scan();
	rig.Settle();
	EXPECT_TRUE(rig.Problems(Vcs002).empty()) << rig.DescribeProblems().CStr();
	EXPECT_TRUE(rig.Repairs().GetFixIts(SDiagnostic { .Code = Vcs002, .Subject = P("Sub/a.png") }).empty());
}

TEST(AssetRepairs, AColdStartTracksTheArrivalOfFilesWithoutAMetaOnly)
{
	CRepairTree tree("Холодний старт");
	tree.WritePng("a.png");
	tree.WriteMeta("a.png", MetaText(GuidA));
	tree.WritePng("b.png", 5u);
	tree.WriteMeta("b.png", MetaText(GuidB));
	tree.WritePng("New.png", 6u);
	tree.WritePng("Z/a.png");
	tree.WriteMeta("Z/a.png", MetaText(GuidA));
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	// New.png, and the two pairs of the duplicate group; b.png's settled pair gains nothing from an arrival time.
	EXPECT_EQ(rig.Repairs().GetStats().TrackedEvents, 5u);
	EXPECT_FALSE(tree.Exists("New.png.meta"));
	EXPECT_EQ(tree.MetaGuid("Z/a.png"), MakeGuid(GuidA)) << "a duplicate group waits for its batch's delay";
	rig.Settle(1600u);
	EXPECT_EQ(tree.MetaGuid("New.png"), DerivedGuid(P("New.png"), 6u));
	EXPECT_EQ(tree.MetaGuid("Z/a.png"), MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Z/a.png")));
	EXPECT_EQ(tree.MetaGuid("a.png"), MakeGuid(GuidA));
}

TEST(AssetRepairs, AMetaCreatedBesideATrackedSourceThatTheVcsDoesNotTrackIsVcs003)
{
	CRepairTree tree("Не в VCS");
	tree.WritePng("a.png");
	CRig rig(tree);
	rig.Vcs.SetStatus(tree.Vcs("a.png"), SVcsStatus { EVcsState::Clean, {} });
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	ASSERT_TRUE(tree.Exists("a.png.meta"));
	const std::vector<SDiagnostic> vcs003 = rig.Problems(Vcs003);
	ASSERT_EQ(vcs003.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(vcs003[0].Severity, ESeverity::Warning);
	EXPECT_EQ(vcs003[0].Subject, P("a.png"));
	EXPECT_EQ(vcs003[0].File, "Content/a.png.meta") << "File names the half to add, as validate's VCS003 does";
	EXPECT_TRUE(vcs003[0].Message.Contains("commit it"));
	rig.Settle(5000u);
	EXPECT_EQ(rig.Problems(Vcs003).size(), 1u) << "kept while the pair stands";

	// The person adds the meta (only .git/index changes, so no file event): the warning goes at the next look.
	rig.Vcs.SetStatus(tree.Vcs("a.png.meta"), SVcsStatus { EVcsState::Added, {} });
	rig.Settle(31000u);
	EXPECT_TRUE(rig.Problems(Vcs003).empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, TheMetasOfOneRoundAreAskedForTheirStatusInOneCall)
{
	CRepairTree tree("Один запит");
	tree.WritePng("a.png");
	tree.WritePng("b.png", 5u);
	tree.WritePng("c.png", 6u);
	CRig rig(tree);
	rig.Open();
	SRepairOptions createOnly;
	createOnly.Kinds = { ELifecycleAction::CreateMeta };
	EXPECT_EQ(rig.Repairs().RunNow(createOnly).Applied, 3u);
	uint32 statusCalls = 0u;
	for (const CRecordingVcsProvider::SCall& call : rig.Vcs.GetCalls())
	{
		if (call.Name == "StatusOf")
		{
			++statusCalls;
			EXPECT_EQ(call.Files.size(), 6u) << "every created pair";
		}
	}
	EXPECT_EQ(statusCalls, 1u) << "a whole-tree status per meta would be a git status per file";
}

TEST(AssetRepairs, APendingProviderDetectionHoldsEveryRepairUntilTheLastMountIsSettled)
{
	CRepairTree tree("Виявлення");
	tree.WritePng("a.png");
	CRig rig(tree, [] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc&)
	{
		Lifecycle.Providers.clear();
		Lifecycle.bProvidersPending = true;
	});
	rig.Open();
	rig.Settle();
	rig.Settle(3000u);
	EXPECT_FALSE(tree.Exists("a.png.meta")) << "a duplicate or a create before detection would go by the wrong facts";
	EXPECT_EQ(rig.Repairs().GetProvider(ProjectMountId), nullptr);

	// What a live host's detection gives: None for a folder outside any repository, through a worker.
	CVcsProviderSet providers;
	bool bDetected = false;
	const MountId ids[] = { ProjectMountId };
	providers.DetectAsync(rig.Pool, *rig.Desc.Mounts, ids, "None", [&] (MountId Mount, IVcsProvider* Provider)
	{
		EXPECT_EQ(Mount, ProjectMountId);
		ASSERT_NE(Provider, nullptr);
		EXPECT_EQ(Provider->GetName(), "None");
		bDetected = true;
	});
	ASSERT_TRUE(rig.Pool.PumpUntil([&] { return bDetected; }, 30.0));
	rig.Repairs().SetProvider(EngineMountId, nullptr);
	rig.Settle(100u);
	EXPECT_FALSE(tree.Exists("a.png.meta")) << "one mount is still pending";

	// The last mount settles while a held run is in flight (a live host's start-up race): the repairs that follow still
	// go through the provider it names.
	tree.WritePng("b.png", 5u);
	rig.Scan();
	rig.Now += 100u;
	rig.Repairs().Tick(rig.Now);
	ASSERT_TRUE(rig.Repairs().IsWorking());
	rig.Repairs().SetProvider(ProjectMountId, &rig.Vcs);
	rig.Settle();
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png")));
	EXPECT_EQ(rig.Repairs().GetProvider(ProjectMountId), &rig.Vcs);
	EXPECT_EQ(providers.Find(ProjectMountId)->GetName(), "None");
	const std::vector<CRecordingVcsProvider::SCall> writes = rig.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 2u) << "the create went through the detected provider";
	EXPECT_EQ(writes[0].Name, "CheckOut");
	EXPECT_EQ(writes[0].Files, (std::vector<FString> { tree.Vcs("a.png.meta"), tree.Vcs("a.png") }));
	EXPECT_EQ(writes[1].Name, "Add");
}

// ---------------- Who writes ----------------

TEST(AssetRepairs, AReadOnlyRunAndAMountTheWriterDoesNotOwnReportMeta001AndWriteNothing)
{
	CRepairTree tree("Лише читання");
	tree.WritePng("a.png");
	const std::filesystem::path engineFont = tree.Tree.EngineContent() / "Fonts" / "x.png";
	frt::test::rules::MakeDirectories(engineFont.parent_path());
	ASSERT_TRUE(platform::WriteFileBytes(engineFont, MakePng(9u)));
	{
		CRig rig(tree, [] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc&) { Lifecycle.bRepairs = false; });
		rig.Open();
		rig.Settle();
		rig.Settle(1600u);
		EXPECT_FALSE(tree.Exists("a.png.meta"));
		EXPECT_EQ(rig.Problems(Meta001).size(), 2u) << rig.DescribeProblems().CStr();
		EXPECT_EQ(rig.Repairs().GetWriter(), nullptr);
		EXPECT_TRUE(rig.Vcs.GetCalls().empty());
	}
	{
		CRig rig(tree);
		rig.Open();
		rig.Settle();
		rig.Settle(1600u);
		EXPECT_TRUE(tree.Exists("a.png.meta")) << "the project's own file is repaired";
		std::error_code ec;
		EXPECT_FALSE(std::filesystem::exists(GetMetaPath(FString::FromPath(engineFont)).ToPath(), ec)) << "/Engine is not this writer's";
		const std::vector<SDiagnostic> meta001 = rig.Problems(Meta001);
		ASSERT_EQ(meta001.size(), 1u) << rig.DescribeProblems().CStr();
		EXPECT_EQ(meta001[0].Subject, "/Engine/Fonts/x.png");
		ASSERT_EQ(rig.Repairs().GetFixIts(meta001[0]).size(), 1u);
		(void)rig.Repairs().RunNow(SRepairOptions {});
		EXPECT_EQ(rig.Problems(Meta001).size(), 1u) << "a run of the owned mounts leaves the others' problems";
		EXPECT_EQ(rig.Repairs().GetFixIts(meta001[0]).size(), 1u) << "and their fix-its";
	}
}

TEST(AssetRepairs, RunNowHasNoDelaysAndHonoursItsKindsAndItsDryRun)
{
	CRepairTree tree("Зараз");
	tree.WritePng("a.png");
	tree.WritePng("b.png", 5u);
	tree.WriteMeta("b.png", MetaText(GuidB));
	CRig rig(tree);
	rig.Open();
	tree.Remove("b.png.meta");
	rig.Scan();

	SRepairOptions dryRun;
	dryRun.bDryRun = true;
	const SRepairReport planned = rig.Repairs().RunNow(dryRun);
	EXPECT_EQ(planned.Planned.size(), 2u);
	EXPECT_EQ(planned.Applied, 0u);
	EXPECT_FALSE(tree.Exists("a.png.meta"));
	EXPECT_FALSE(tree.Exists("b.png.meta"));

	SRepairOptions createOnly;
	createOnly.Kinds = { ELifecycleAction::CreateMeta };
	const SRepairReport created = rig.Repairs().RunNow(createOnly);
	EXPECT_EQ(created.Applied, 1u);
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png")));
	EXPECT_FALSE(tree.Exists("b.png.meta")) << "a restore is not a CreateMeta";
	const auto lost = std::find_if(created.Problems.begin(), created.Problems.end(), [] (const SDiagnostic& Problem)
	{
		return Problem.Code == Meta008 && Problem.Subject == P("b.png");
	});
	EXPECT_NE(lost, created.Problems.end()) << "a repair the run's Kinds leave out is reported, not dropped";
	ASSERT_EQ(rig.Problems(Meta008).size(), 1u) << rig.DescribeProblems().CStr();
	const std::vector<SLifecycleAction> restore = rig.Repairs().GetFixIts(rig.Problems(Meta008)[0]);
	ASSERT_EQ(restore.size(), 1u);
	EXPECT_EQ(restore[0].Kind, ELifecycleAction::RestoreMeta);

	const SRepairReport all = rig.Repairs().RunNow(SRepairOptions {});
	EXPECT_EQ(all.Applied, 1u);
	EXPECT_EQ(tree.MetaGuid("b.png"), MakeGuid(GuidB));
	EXPECT_TRUE(all.Problems.empty());
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, AnEngineOwnerRunRepairsEngineFilesWithTheEngineSalt)
{
	CRepairTree tree("Власник рушія");
	const std::filesystem::path font = tree.Tree.EngineContent() / "Fonts" / "x.png";
	frt::test::rules::MakeDirectories(font.parent_path());
	ASSERT_TRUE(platform::WriteFileBytes(font, MakePng(9u)));
	CRig rig(tree, [&tree] (SLifecycleDesc&, SAssetRegistryDesc& Desc)
	{
		SMountRoots roots;
		roots.EngineDir = tree.Tree.EngineDir();
		roots.EngineContentDir = tree.Tree.EngineContent();
		roots.EngineConfigDir = tree.Tree.EngineConfig();
		Desc.Mounts = std::make_shared<const CMountTable>(CMountTable::Resolve(roots, nullptr));
		Desc.Owner = EAssetRegistryOwner::Engine;
	});
	rig.Lifecycle.Providers = { { EngineMountId, &rig.Vcs } };
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(GetMetaPath(FString::FromPath(font)).ToPath());
	ASSERT_TRUE(bytes.has_value());
	SMetaParseResult parsed;
	ParseMeta(FStringView(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size())), parsed);
	EXPECT_EQ(parsed.Meta.Guid, MakeCreateMetaGuid(EngineAssetSalt, "/Engine/Fonts/x.png", hash::HashContent(MakePng(9u))));
	EXPECT_EQ(rig.GuidAt("/Engine/Fonts/x.png"), parsed.Meta.Guid);
}

TEST(AssetRepairs, AMetaSpelledInAnotherCaseIsRenamedToItsSourcesCaseAfterTheGrace)
{
	CRepairTree tree("Регістр");
	tree.WritePng("a.png");
	tree.WriteProjectContent("a.PNG.meta", MetaText(GuidA));
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty()) << "a hand rename's second half is not raced";
	rig.Settle(1100u);
	bool bExact = false;
	platform::EnumerateDirectory(tree.Tree.ProjectContent(), [&] (const platform::SDirectoryEntry& Entry)
	{
		bExact = bExact || Entry.Name == FStringView("a.png.meta");
	});
	EXPECT_TRUE(bExact);
	EXPECT_EQ(tree.MetaGuid("a.png"), MakeGuid(GuidA));
	const std::vector<CRecordingVcsProvider::SCall> writes = rig.Vcs.GetWriteCalls();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0].Name, "Move");
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, ABrokenMetaWithTwoGuidsOffersKeepAAliasBAndTheFixRewritesItCanonically)
{
	CRepairTree tree("Конфлікт");
	tree.WritePng("a.png");
	tree.WriteMeta("a.png", FString::Format("FormatVersion: 1\nGuid: {}\nGuid: {}\n", GuidA, GuidB));
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	EXPECT_EQ(rig.StateOf(P("a.png")), EMetaState::Broken);
	const std::vector<SDiagnostic> meta004 = rig.Problems(Meta004);
	ASSERT_EQ(meta004.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(meta004[0].Subject, P("a.png.meta"));
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty()) << "a broken meta is never overwritten automatically";
	const std::vector<SLifecycleAction> fixIts = rig.Repairs().GetFixIts(meta004[0]);
	ASSERT_EQ(fixIts.size(), 2u);
	EXPECT_EQ(fixIts[0].Guid, MakeGuid(GuidA)) << "the record's GUID first";

	EXPECT_EQ(rig.Repairs().ApplyFix(fixIts[0]).Applied, 1u);
	const std::optional<SAssetMeta> meta = tree.ReadMeta("a.png");
	ASSERT_TRUE(meta.has_value());
	EXPECT_EQ(meta->Guid, MakeGuid(GuidA));
	EXPECT_EQ(meta->Aliases, std::vector<SGuid> { MakeGuid(GuidB) });
	EXPECT_EQ(rig.StateOf(P("a.png")), EMetaState::Paired);
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
}

TEST(AssetRepairs, KeepAliasBNeverOverwritesAMetaResolvedByHandAfterTheFixItWasOffered)
{
	CRepairTree tree("Розв'язано вручну");
	tree.WritePng("a.png");
	tree.WriteMeta("a.png", FString::Format("FormatVersion: 1\nGuid: {}\nGuid: {}\n", GuidA, GuidB));
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	const std::vector<SDiagnostic> meta004 = rig.Problems(Meta004);
	ASSERT_EQ(meta004.size(), 1u) << rig.DescribeProblems().CStr();
	const std::vector<SLifecycleAction> fixIts = rig.Repairs().GetFixIts(meta004[0]);
	ASSERT_EQ(fixIts.size(), 2u);

	// The person resolves the conflict by hand before the fix runs (no scan in between).
	const FString resolved = MetaText(GuidA, "Labels:\n  - Вручну\n");
	tree.WriteMeta("a.png", resolved);
	const SRepairReport report = rig.Repairs().ApplyFix(fixIts[0]);
	EXPECT_EQ(report.Applied, 0u);
	EXPECT_EQ(tree.Read("a.png.meta"), resolved) << "only the broken meta the fix-it was offered for is rewritten";
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty());
}

// ---------------- Start-up ----------------

namespace
{
// An op of the create-meta command stopped after its journal's Begin, as a crash there leaves it.
SGuid LeaveUnfinishedCreate (const CRepairTree& Tree, FStringView Relative)
{
	const CMountTable mounts = Tree.Resolve();
	SAssetWriterDesc desc;
	desc.Mounts = &mounts;
	desc.OwnerRoot = Tree.Tree.ProjectDir();
	desc.bHoldsLock = true;
	CAssetWriter writer(std::move(desc));
	writer.SetStopAfterStepForTest(0u);
	SCreateMetaRequest request;
	request.Source = P(Relative);
	request.Meta.Guid = SGuid::NewRandom();
	(void)CreateMeta(writer, request);
	EXPECT_TRUE(writer.GetUnfinished().has_value());
	return request.Meta.Guid;
}
}

TEST(AssetRepairs, AnUnfinishedOperationIsRolledForwardBeforeTheFirstScanCommits)
{
	CRepairTree tree("Відкат уперед");
	tree.WritePng("a.png");
	const SGuid guid = LeaveUnfinishedCreate(tree, "a.png");
	EXPECT_FALSE(tree.Exists("a.png.meta"));
	CRig rig(tree);
	rig.Open();
	EXPECT_EQ(tree.MetaGuid("a.png"), guid);
	EXPECT_EQ(rig.GuidAt(P("a.png")), guid) << "the initial scan saw the finished pair";
	EXPECT_EQ(rig.StateOf(P("a.png")), EMetaState::Paired);
	EXPECT_FALSE(rig.Repairs().GetWriter()->GetUnfinished().has_value());
}

TEST(AssetRepairs, TakeOwnershipTakesTheLockThenRollsAnUnfinishedOperationForward)
{
	CRepairTree tree("Власність");
	tree.WritePng("a.png");
	const SGuid guid = LeaveUnfinishedCreate(tree, "a.png");
	const CMountTable mounts = tree.Resolve();
	CRecordingVcsProvider vcs;
	const std::pair<MountId, IVcsProvider*> providers[] = { { ProjectMountId, &vcs } };
	SOwnership ownership = TakeOwnership(tree.Tree.ProjectDir(), mounts, providers);
	ASSERT_TRUE(ownership.Lock.has_value()) << ownership.Error.CStr();
	EXPECT_TRUE(ownership.bRolledForward);
	EXPECT_FALSE(ownership.bUnfinished);
	EXPECT_EQ(tree.MetaGuid("a.png"), guid);
	EXPECT_FALSE(vcs.GetWriteCalls().empty()) << "the roll-forward goes through the provider";

	const SOwnership second = TakeOwnership(tree.Tree.ProjectDir(), mounts, providers);
	EXPECT_FALSE(second.Lock.has_value());
	EXPECT_FALSE(second.Owner.IsEmpty()) << "a second writer is told who owns the root";
}

// The roll-forward's providers are detected (git spawned once per mount, in a host) only once the claim holds the lock of a
// root whose journal holds an unfinished operation (edit [55]): never for a refused second writer or a guest, which leave
// the operation to its owner, and never over a finished journal.
TEST(AssetRepairs, ARollForwardDetectsItsProvidersOnlyOnceTheLockIsHeldAndAnOperationIsDue)
{
	CRepairTree tree("Виявлення");
	tree.WritePng("a.png");
	const SGuid guid = LeaveUnfinishedCreate(tree, "a.png");
	const CMountTable mounts = tree.Resolve();
	CRecordingVcsProvider vcs;
	const std::pair<MountId, IVcsProvider*> providers[] = { { ProjectMountId, &vcs } };
	uint32 detections = 0u;

	SAssetOwnershipRequest request;
	request.Root = tree.Tree.ProjectDir();
	request.RulesFile = tree.ProjectRules();
	SAssetOwnership owner = ClaimAssetOwnership(request);   // the lock alone: another process, in effect
	ASSERT_EQ(owner.Role, EOwnerRole::Writer) << owner.Root.Error.CStr();

	request.Mounts = &mounts;
	request.DetectProviders = [&] () -> std::span<const std::pair<MountId, IVcsProvider*>>
	{
		++detections;
		return providers;
	};
	const SAssetOwnership refused = ClaimAssetOwnership(request);
	EXPECT_EQ(refused.Role, EOwnerRole::Refused);
	EXPECT_TRUE(refused.Root.bUnfinished);
	EXPECT_EQ(detections, 0u) << "a refused second writer detects no provider";

	tree.WriteProjectRules("FormatVersion: 1\nSafety:\n  GuestProcesses: true\n");
	const SAssetOwnership guest = ClaimAssetOwnership(request);
	EXPECT_EQ(guest.Role, EOwnerRole::Guest);
	EXPECT_EQ(detections, 0u) << "a guest detects no provider";
	EXPECT_FALSE(tree.Exists("a.png.meta"));

	owner.Root.Lock.reset();
	SAssetOwnership writer = ClaimAssetOwnership(request);
	ASSERT_EQ(writer.Role, EOwnerRole::Writer) << writer.Root.Error.CStr();
	EXPECT_EQ(detections, 1u) << "the owner of a due roll-forward detects its providers once";
	EXPECT_TRUE(writer.Root.bRolledForward) << writer.Root.Error.CStr();
	EXPECT_EQ(tree.MetaGuid("a.png"), guid);
	EXPECT_FALSE(vcs.GetWriteCalls().empty()) << "through the detected provider";

	writer.Root.Lock.reset();
	EXPECT_EQ(ClaimAssetOwnership(request).Role, EOwnerRole::Writer);
	EXPECT_EQ(detections, 1u) << "a finished journal needs no provider";
}

// ---------------- Second processes and engine-owner mode ----------------

namespace
{
// A scratch engine checkout with a project inside it, as Templates/* lie in the engine (§4.7): <root>/Core/Content and
// <root>/Core/Config for /Engine, <root>/Templates/<Project> for /Project. Nothing of the real checkout is copied.
struct SScratchEngine
{
	explicit SScratchEngine (const CRepairTree& Tree, FStringView Project = "Гра")
		: Root(Tree.Tree.Root / FStringView("Рушій 引擎").ToPath())
		, ProjectDir(Root / "Templates" / Project.ToPath())
	{
		for (const std::filesystem::path& dir : { Content(), Root / "Core" / "Config", ProjectDir / "Content", ProjectDir / "Config" })
		{
			frt::test::rules::MakeDirectories(dir);
		}
	}

	[[nodiscard]] std::filesystem::path Content () const { return Root / "Core" / "Content"; }
	[[nodiscard]] std::filesystem::path Font (FStringView Name) const { return Content() / "Fonts" / Name.ToPath(); }
	void WriteFont (FStringView Name, uint32 Size) const
	{
		frt::test::rules::MakeDirectories(Font(Name).parent_path());
		ASSERT_TRUE(platform::WriteFileBytes(Font(Name), MakePng(Size)));
	}

	[[nodiscard]] std::shared_ptr<const CMountTable> Mounts () const
	{
		SMountRoots roots;
		roots.EngineDir = Root;
		roots.EngineContentDir = Content();
		roots.EngineConfigDir = Root / "Core" / "Config";
		roots.ProjectDir = ProjectDir;
		return std::make_shared<const CMountTable>(CMountTable::Resolve(roots, nullptr));
	}

	[[nodiscard]] SAssetOwnershipRequest Request () const
	{
		SAssetOwnershipRequest request;
		request.Root = ProjectDir;
		request.RulesFile = ProjectDir / "Config" / "Assets.yml";
		if (IsEngineOwnerMode(Root, ProjectDir, false))
		{
			request.EngineRoot = Root;
		}
		return request;
	}

	std::filesystem::path Root;
	std::filesystem::path ProjectDir;
};

[[nodiscard]] FString ReadBytes (const std::filesystem::path& Path)
{
	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path);
	return bytes.has_value() ? FString(FStringView(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size()))) : FString();
}

[[nodiscard]] SGuid ReadMetaGuidAt (const std::filesystem::path& Source)
{
	SMetaParseResult parsed;
	ParseMeta(ReadBytes(GetMetaPath(FString::FromPath(Source)).ToPath()), parsed);
	return parsed.State == EMetaParseState::Ok ? parsed.Meta.Guid : SGuid {};
}
}

// §5.9: one writer per mount. A second process that wants to write is refused and told who owns the root (the M3
// deviation, an editor that stayed open writing nothing, is over); a read-only run never asks for the lock; once the
// owner lets go the next writer takes it.
TEST(AssetRepairs, ASecondWriterIsRefusedNamingTheOwnerAndAReadOnlyRunNeverAsksForTheLock)
{
	CRepairTree tree("Другий процес");
	SAssetOwnershipRequest request;
	request.Root = tree.Tree.ProjectDir();
	request.RulesFile = tree.ProjectRules();
	SAssetOwnership first = ClaimAssetOwnership(request);
	ASSERT_EQ(first.Role, EOwnerRole::Writer) << first.Root.Error.CStr();
	ASSERT_TRUE(first.Root.Lock.has_value());

	const SAssetOwnership second = ClaimAssetOwnership(request);
	EXPECT_EQ(second.Role, EOwnerRole::Refused);
	EXPECT_FALSE(second.Root.Lock.has_value());
	EXPECT_EQ(second.Root.Owner, GetThisProcessAsOwner().Describe());

	request.bReadOnly = true;
	const SAssetOwnership readOnly = ClaimAssetOwnership(request);
	EXPECT_EQ(readOnly.Role, EOwnerRole::ReadOnly);
	EXPECT_FALSE(readOnly.Root.Lock.has_value());
	EXPECT_TRUE(readOnly.Root.Owner.IsEmpty()) << "it never asked";

	first.Root.Lock.reset();
	request.bReadOnly = false;
	EXPECT_EQ(ClaimAssetOwnership(request).Role, EOwnerRole::Writer);
}

// Safety.GuestProcesses (opt-in, §4.8, §5.9): the second process runs as a read-only guest instead of exiting. It rolls no
// unfinished operation forward (it reports one), writes no meta, snapshot or journal, and its META001 names the owner,
// who creates the meta.
TEST(AssetRepairs, SafetyGuestProcessesMakesASecondProcessAReadOnlyGuestThatWritesNothingAndNamesTheOwner)
{
	CRepairTree tree("Гість");
	tree.WriteProjectRules("FormatVersion: 1\nSafety:\n  GuestProcesses: true\n");
	tree.WritePng("a.png");
	tree.WritePng("b.png", 5u);
	(void)LeaveUnfinishedCreate(tree, "b.png");
	const CMountTable mounts = tree.Resolve();

	SAssetOwnershipRequest request;
	request.Root = tree.Tree.ProjectDir();
	request.RulesFile = tree.ProjectRules();
	const SAssetOwnership owner = ClaimAssetOwnership(request);   // the lock alone: the owner rolls forward when it likes
	ASSERT_EQ(owner.Role, EOwnerRole::Writer) << owner.Root.Error.CStr();
	EXPECT_TRUE(owner.Root.bUnfinished);

	request.Mounts = &mounts;
	const SAssetOwnership guest = ClaimAssetOwnership(request);
	ASSERT_EQ(guest.Role, EOwnerRole::Guest);
	EXPECT_FALSE(guest.Root.Lock.has_value());
	EXPECT_EQ(guest.Root.Owner, GetThisProcessAsOwner().Describe());
	EXPECT_TRUE(guest.Root.bUnfinished) << "reported";
	EXPECT_FALSE(guest.Root.bRolledForward);
	EXPECT_FALSE(tree.Exists("b.png.meta")) << "a guest finishes nobody's operation";

	{
		CRig rig(tree, [&guest] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc& Desc)
		{
			Lifecycle.bRepairs = false;
			Lifecycle.Owner = guest.Root.Owner;
			Desc.bWriter = false;
			Desc.bSnapshotAtShutDown = true;
		});
		rig.Open();
		rig.Settle();
		rig.Settle(1600u);
		EXPECT_FALSE(tree.Exists("a.png.meta"));
		EXPECT_EQ(rig.Repairs().GetWriter(), nullptr);
		EXPECT_TRUE(rig.Vcs.GetCalls().empty());
		const std::vector<SDiagnostic> meta001 = rig.Problems(Meta001);
		ASSERT_EQ(meta001.size(), 2u) << rig.DescribeProblems().CStr();
		for (const SDiagnostic& problem : meta001)
		{
			EXPECT_TRUE(problem.Message.Contains(guest.Root.Owner + " owns /Project: it creates the meta")) << problem.Message.CStr();
		}
	}
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(GetRegistrySnapshotPath(tree.Tree.ProjectDir()), ec)) << "a guest writes no snapshot";
	EXPECT_FALSE(tree.Exists("a.png.meta"));
}

// §4.7: /Engine is owner-writable for a project inside the engine checkout (Templates/*) or with
// -Assets:EngineWritable=1, and read-only for an engine an external project uses.
TEST(AssetRepairs, EngineOwnerModeIsAProjectInsideTheEngineCheckoutOrEngineWritableAndNeverAnExternalProjectByItself)
{
	CRepairTree tree("Режим рушія");
	const std::filesystem::path engine = tree.Tree.Root / "Engine";
	EXPECT_TRUE(IsEngineOwnerMode(engine, engine / "Templates" / "Demo", false));
	EXPECT_FALSE(IsEngineOwnerMode(engine, tree.Tree.Root / "Game", false));
	EXPECT_FALSE(IsEngineOwnerMode(engine, tree.Tree.Root / "Engine2" / "Game", false)) << "a sibling whose name starts alike";
	EXPECT_FALSE(IsEngineOwnerMode(engine, engine.parent_path(), false)) << "an engine inside the project (a submodule)";
	EXPECT_TRUE(IsEngineOwnerMode(engine, tree.Tree.Root / "Game", true));
	EXPECT_FALSE(IsEngineOwnerMode(engine, std::filesystem::path(), true)) << "no project: an --engine run, which owns /Engine itself";

	// The host claims the engine's lock only for a registry whose lifecycle repairs: without one, bEngineMetasWritable does
	// nothing and the lock would only keep --engine runs out while the editor is open.
	SLifecycleDesc reportOnly;
	SLifecycleDesc repairs;
	repairs.bRepairs = true;
	EXPECT_FALSE(ClaimsEngineOwnership(true, nullptr)) << "no lifecycle";
	EXPECT_FALSE(ClaimsEngineOwnership(true, &reportOnly));
	EXPECT_TRUE(ClaimsEngineOwnership(true, &repairs));
	EXPECT_FALSE(ClaimsEngineOwnership(false, &repairs)) << "an external project's engine";
}

// The M4 row's proof: a file added under a scratch engine's Core/Content gets its meta in engine-owner mode, through the
// provider and a writer of the engine root, whose own OpJournal.log is rolled forward at start-up. The editor writes no
// Engine.snapshot and no journal of /Engine: only --engine runs do. A second in-engine editor of another project gets
// its own project's lock but not the engine's (best-effort: /Engine stays read-only for it, naming the holder).
TEST(AssetRepairs, AFileAddedUnderAScratchEnginesCoreContentGetsAMetaInEngineOwnerModeWithoutAnEngineSnapshot)
{
	CRepairTree tree("Власник рушія з проєкту");
	const SScratchEngine engine(tree);
	engine.WriteFont("z.png", 7u);
	const std::shared_ptr<const CMountTable> mounts = engine.Mounts();
	SGuid unfinished;
	{
		SAssetWriterDesc desc;
		desc.Mounts = mounts.get();
		desc.OwnerRoot = engine.Root;
		desc.bHoldsLock = true;
		CAssetWriter writer(std::move(desc));
		writer.SetStopAfterStepForTest(0u);
		SCreateMetaRequest create;
		create.Source = "/Engine/Fonts/z.png";
		create.Meta.Guid = SGuid::NewRandom();
		unfinished = create.Meta.Guid;
		(void)CreateMeta(writer, create);
		ASSERT_TRUE(writer.GetUnfinished().has_value());
	}

	SAssetOwnership ownership = ClaimAssetOwnership(engine.Request());
	ASSERT_EQ(ownership.Role, EOwnerRole::Writer) << ownership.Root.Error.CStr();
	ASSERT_TRUE(ownership.OwnsEngine()) << ownership.Engine.Error.CStr();
	EXPECT_TRUE(ownership.Engine.bUnfinished) << "left for the lifecycle's start";

	// Another in-engine project's editor beside it: its own project, a read-only /Engine.
	const SScratchEngine other(tree, "Інша");
	const SAssetOwnership second = ClaimAssetOwnership(other.Request());
	EXPECT_EQ(second.Role, EOwnerRole::Writer) << "only the project's own lock makes a second writer exit";
	EXPECT_FALSE(second.OwnsEngine());
	EXPECT_EQ(second.Engine.Owner, GetThisProcessAsOwner().Describe());

	CRig rig(tree, [&] (SLifecycleDesc&, SAssetRegistryDesc& Desc)
	{
		Desc.Mounts = mounts;
		Desc.bEngineMetasWritable = ownership.OwnsEngine();
		Desc.bSnapshotAtShutDown = true;
	});
	rig.Lifecycle.Providers = { { EngineMountId, &rig.Vcs }, { ProjectMountId, &rig.Vcs } };
	rig.Open();
	EXPECT_EQ(ReadMetaGuidAt(engine.Font("z.png")), unfinished) << "the engine root's journal is rolled forward too";
	CAssetWriter* engineWriter = rig.Repairs().GetWriterOf(EngineMountId);
	ASSERT_NE(engineWriter, nullptr);
	EXPECT_EQ(engineWriter->GetJournalPath(), GetOpJournalPath(engine.Root));
	EXPECT_FALSE(engineWriter->GetUnfinished().has_value());
	ASSERT_NE(rig.Repairs().GetWriterOf(ProjectMountId), nullptr);
	EXPECT_EQ(rig.Repairs().GetWriterOf(ProjectMountId)->GetJournalPath(), GetOpJournalPath(engine.ProjectDir));
	rig.Settle();

	engine.WriteFont("x.png", 9u);
	rig.Scan("/Engine");
	rig.Settle();
	EXPECT_FALSE(std::filesystem::exists(GetMetaPath(FString::FromPath(engine.Font("x.png"))).ToPath())) << "its delay first";
	rig.Settle(1600u);
	const SGuid expected = MakeCreateMetaGuid(EngineAssetSalt, "/Engine/Fonts/x.png", hash::HashContent(MakePng(9u)));
	EXPECT_EQ(ReadMetaGuidAt(engine.Font("x.png")), expected);
	EXPECT_EQ(rig.GuidAt("/Engine/Fonts/x.png"), expected);
	EXPECT_EQ(rig.StateOf("/Engine/Fonts/x.png"), EMetaState::Paired);
	const std::vector<CRecordingVcsProvider::SCall> writes = rig.Vcs.GetWriteCalls();
	ASSERT_FALSE(writes.empty());
	EXPECT_TRUE(writes.back().Files.front().Contains("x.png")) << "through the provider";
	EXPECT_TRUE(rig.Problems(Meta001).empty()) << rig.DescribeProblems().CStr();

	rig.Registry->ShutDown();
	rig.Registry.reset();
	std::error_code ec;
	EXPECT_TRUE(std::filesystem::exists(GetRegistrySnapshotPath(engine.ProjectDir), ec)) << "the project's own snapshot";
	EXPECT_FALSE(std::filesystem::exists(GetEngineSnapshotPath(engine.Root), ec));
	EXPECT_TRUE(!std::filesystem::exists(GetEngineSnapshotPath(engine.Root).parent_path(), ec)
		|| std::filesystem::is_empty(GetEngineSnapshotPath(engine.Root).parent_path(), ec)) << "no journal of /Engine either";
}

namespace
{
// A create-meta op of Source (a mount path under Root's mounts) stopped after its journal's Begin, in Root's OpJournal.log.
void LeaveUnfinishedCreateAt (const CMountTable& Mounts, const std::filesystem::path& Root, FStringView Source)
{
	SAssetWriterDesc desc;
	desc.Mounts = &Mounts;
	desc.OwnerRoot = Root;
	desc.bHoldsLock = true;
	CAssetWriter writer(std::move(desc));
	writer.SetStopAfterStepForTest(0u);
	SCreateMetaRequest create;
	create.Source = Source;
	create.Meta.Guid = SGuid::NewRandom();
	(void)CreateMeta(writer, create);
	EXPECT_TRUE(writer.GetUnfinished().has_value()) << FString::FromPath(Root).CStr();
}
}

// Edit [15]: at start-up a host reports every root whose journal holds an unfinished operation, whatever its role. A writer
// that took the lock alone (no Mounts: nothing rolled forward yet) says the lifecycle finishes it, unless its registry runs
// one that repairs; a guest, a refused writer or a reader names who finishes it; engine-owner mode's engine root counts too.
TEST(AssetRepairs, EveryRootWithAnUnfinishedOperationIsReportedAtStartUpWhateverTheRole)
{
	CRepairTree tree("Незавершене");
	const SScratchEngine engine(tree);
	engine.WriteFont("z.png", 7u);
	frt::test::rules::MakeDirectories(engine.ProjectDir / "Content");
	ASSERT_TRUE(platform::WriteFileBytes(engine.ProjectDir / "Content" / "a.png", MakePng(3u)));
	const std::shared_ptr<const CMountTable> mounts = engine.Mounts();
	LeaveUnfinishedCreateAt(*mounts, engine.Root, "/Engine/Fonts/z.png");
	LeaveUnfinishedCreateAt(*mounts, engine.ProjectDir, "/Project/a.png");
	const FString projectJournal = FString::FromPath(GetOpJournalPath(engine.ProjectDir));
	const FString engineJournal = FString::FromPath(GetOpJournalPath(engine.Root));
	const FString self = GetThisProcessAsOwner().Describe();

	SAssetOwnershipRequest request = engine.Request();
	ASSERT_FALSE(request.EngineRoot.empty());
	SAssetOwnership owner = ClaimAssetOwnership(request);
	ASSERT_EQ(owner.Role, EOwnerRole::Writer) << owner.Root.Error.CStr();
	ASSERT_TRUE(owner.OwnsEngine()) << owner.Engine.Error.CStr();
	const std::vector<FString> lines = DescribeUnfinishedOperations(request, owner, false);
	ASSERT_EQ(lines.size(), 2u);
	EXPECT_TRUE(lines[0].Contains(projectJournal)) << lines[0].CStr();
	EXPECT_TRUE(lines[0].Contains("finished when the lifecycle starts")) << lines[0].CStr();
	EXPECT_TRUE(lines[1].Contains(engineJournal)) << lines[1].CStr();
	EXPECT_TRUE(lines[1].Contains("finished when the lifecycle starts")) << lines[1].CStr();
	EXPECT_TRUE(DescribeUnfinishedOperations(request, owner, true).empty()) << "a repairing lifecycle finishes both at its Start";

	// A second writer of the same project is refused: its report names the owner, and it never asked for the engine.
	const SAssetOwnership refused = ClaimAssetOwnership(request);
	ASSERT_EQ(refused.Role, EOwnerRole::Refused);
	const std::vector<FString> refusedLines = DescribeUnfinishedOperations(request, refused, true);
	ASSERT_EQ(refusedLines.size(), 1u);
	EXPECT_TRUE(refusedLines[0].Contains(projectJournal)) << refusedLines[0].CStr();
	EXPECT_TRUE(refusedLines[0].Contains(self + " finishes it")) << refusedLines[0].CStr();

	ASSERT_TRUE(platform::WriteFileText(request.RulesFile, "FormatVersion: 1\nSafety:\n  GuestProcesses: true\n"));
	const SAssetOwnership guest = ClaimAssetOwnership(request);
	ASSERT_EQ(guest.Role, EOwnerRole::Guest);
	const std::vector<FString> guestLines = DescribeUnfinishedOperations(request, guest, false);
	ASSERT_EQ(guestLines.size(), 1u);
	EXPECT_TRUE(guestLines[0].Contains(self + " finishes it")) << guestLines[0].CStr();

	SAssetOwnershipRequest reader = request;
	reader.bReadOnly = true;
	const std::vector<FString> readerLines = DescribeUnfinishedOperations(reader, ClaimAssetOwnership(reader), false);
	ASSERT_EQ(readerLines.size(), 1u);
	EXPECT_TRUE(readerLines[0].Contains("its owner finishes it")) << readerLines[0].CStr();

	// Another in-engine project's writer: nothing waits in its own journal, the engine's waits for the engine's owner.
	const SScratchEngine other(tree, "Інша");
	const SAssetOwnershipRequest otherRequest = other.Request();
	const SAssetOwnership second = ClaimAssetOwnership(otherRequest);
	ASSERT_EQ(second.Role, EOwnerRole::Writer) << second.Root.Error.CStr();
	ASSERT_FALSE(second.OwnsEngine());
	const std::vector<FString> secondLines = DescribeUnfinishedOperations(otherRequest, second, true);
	ASSERT_EQ(secondLines.size(), 1u);
	EXPECT_TRUE(secondLines[0].Contains(engineJournal)) << secondLines[0].CStr();
	EXPECT_TRUE(secondLines[0].Contains(self + " finishes it")) << secondLines[0].CStr();
}

// Edits [6], [64]: a host hands Safety.BuilderSourceGuard of the rule file that governs its process to the builder guard at
// start-up; a missing file gives the default, on.
TEST(AssetRepairs, AHostAppliesSafetyBuilderSourceGuardOfItsRuleFile)
{
	CRepairTree tree("Охорона будівників");
	struct SRestore
	{
		~SRestore () { SetBuilderSourceGuardEnabled(true); }
	} restore;
	ASSERT_TRUE(IsBuilderSourceGuardEnabled());
	tree.WriteProjectRules("FormatVersion: 1\nSafety:\n  BuilderSourceGuard: false\n");
	EXPECT_FALSE(ApplySafetyOfRuleFile(tree.ProjectRules()).bBuilderSourceGuard);
	EXPECT_FALSE(IsBuilderSourceGuardEnabled());
	EXPECT_TRUE(ApplySafetyOfRuleFile(tree.ProjectRules().parent_path() / "Missing.yml").bBuilderSourceGuard);
	EXPECT_TRUE(IsBuilderSourceGuardEnabled());
}

// ---------------- What gets reported ----------------

TEST(AssetRepairs, ProblemsCarryTheirFileAndTheRuleFilesValidationSeverity)
{
	CRepairTree tree("Перевірка");
	tree.WriteProjectRules("FormatVersion: 1\nSafety:\n  AutoRepairMetas: false\nValidation: { META001: Warning }\n");
	tree.WritePng("Textures/a.png");
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	const std::vector<SDiagnostic> meta001 = rig.Problems(Meta001);
	ASSERT_EQ(meta001.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(meta001[0].Severity, ESeverity::Warning);
	EXPECT_EQ(meta001[0].File, "Content/Textures/a.png");
}

TEST(AssetRepairs, CrLfInAMetaIsVcs001AndAGitLfsPointerIsVcs004WithoutAMeta)
{
	CRepairTree tree("Байти");
	tree.WritePng("a.png");
	tree.WriteMeta("a.png", "FormatVersion: 1\r\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\r\n");
	tree.WriteProjectContent("Big.png", "version https://git-lfs.github.com/spec/v1\noid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\nsize 12345\n");
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	rig.Settle(1600u);
	const std::vector<SDiagnostic> vcs001 = rig.Problems(Vcs001);
	ASSERT_EQ(vcs001.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(vcs001[0].Subject, P("a.png.meta"));
	EXPECT_EQ(vcs001[0].File, "Content/a.png.meta");

	EXPECT_FALSE(tree.Exists("Big.png.meta")) << "a pointer is never hashed into a GUID";
	const std::vector<SDiagnostic> vcs004 = rig.Problems(Vcs004);
	ASSERT_EQ(vcs004.size(), 1u) << rig.DescribeProblems().CStr();
	EXPECT_EQ(vcs004[0].Subject, P("Big.png"));

	// The meta normalised: VCS001 goes.
	tree.WriteMeta("a.png", MetaText(GuidA));
	rig.Scan();
	rig.Settle(100u);
	EXPECT_TRUE(rig.Problems(Vcs001).empty()) << rig.DescribeProblems().CStr();
}

// ---------------- The dedupe op's crash points ----------------

namespace
{
// The ReGuid repairs a copied pack makes, as RunLifecycle proposes them (planned through a dry run).
std::vector<SLifecycleAction> PlanPackCopy (CRig& Rig)
{
	SRepairOptions options;
	options.bDryRun = true;
	options.Kinds = { ELifecycleAction::ReGuid };
	return Rig.Repairs().RunNow(options).Planned;
}

void ExpectPackDeduped (const CRepairTree& Tree, FStringView Context)
{
	const std::optional<SAssetMeta> rock = Tree.ReadMeta("Pack2/Rock.png");
	ASSERT_TRUE(rock.has_value()) << FString(Context).CStr();
	EXPECT_EQ(rock->Guid, MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Pack2/Rock.png"))) << FString(Context).CStr();
	EXPECT_EQ(RemapTarget(*rock), MakeDedupeGuid(Salt, MakeGuid(GuidB), P("Pack2/Skin.png")).ToString()) << FString(Context).CStr();
	EXPECT_EQ(Tree.MetaGuid("Pack2/Skin.png"), MakeDedupeGuid(Salt, MakeGuid(GuidB), P("Pack2/Skin.png"))) << FString(Context).CStr();
}

// "<engine dir> <engine content> <engine config> <project dir> <stop after> then <meta path> <old> <new> per loser": runs
// the dedupe op of those losers (each mapping every other old GUID to its new one) through a writer that stops after that
// many steps, says so, and waits there to be killed.
int RunDedupeKill (std::span<const FString> Arguments)
{
	if (Arguments.size() < 8u || (Arguments.size() - 5u) % 3u != 0u)
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
	std::vector<SLifecycleAction> actions;
	for (size_t i = 5u; i + 2u < Arguments.size(); i += 3u)
	{
		SLifecycleAction action;
		action.Kind = ELifecycleAction::ReGuid;
		action.Path = Arguments[i];
		action.Guid = SGuid::Parse(Arguments[i + 2u]).value_or(SGuid {});
		action.Remap.emplace_back(SGuid::Parse(Arguments[i + 1u]).value_or(SGuid {}), action.Guid);
		actions.push_back(std::move(action));
	}
	for (SLifecycleAction& action : actions)
	{
		for (const SLifecycleAction& other : actions)
		{
			if (&other != &action)
			{
				action.Remap.push_back(other.Remap.front());
			}
		}
	}
	CRegistryStore store;
	SAssetOp op;
	std::vector<SDiagnostic> problems;
	if (!PlanRepairOp(writer, store.Live(), actions, op, problems))
	{
		return 4;
	}
	writer.SetStopAfterStepForTest(Arguments[4].ToUInt32().value_or(0u), []
	{
		std::fputs("stopped\n", stdout);
		std::fflush(stdout);
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	});
	(void)writer.Run(op);
	std::fputs("returned\n", stdout);
	return 3;
}

const bool bDedupeKillRegistered = test::RegisterChildMode("repair-dedupe-kill", &RunDedupeKill);
}

TEST(AssetRepairs, EveryCrashPointOfTheDedupeOpRollsForwardToBothLosersAndOneRealKillDoesToo)
{
	CRepairTree tree("Збій дедуплікації");
	WritePack(tree);
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	CopyPack(tree);
	rig.Scan("/Project/Pack2");
	const std::vector<SLifecycleAction> actions = PlanPackCopy(rig);
	ASSERT_EQ(actions.size(), 2u);
	const FString rockMeta = tree.Read("Pack2/Rock.png.meta");
	const FString skinMeta = tree.Read("Pack2/Skin.png.meta");
	CAssetWriter& writer = *rig.Repairs().GetWriter();

	// In-process, each crash point: stopped after k steps with the journal's Begin on disk, then rolled forward.
	for (uint32 stop = 0u; stop <= 2u; ++stop)
	{
		tree.WriteProjectContent("Pack2/Rock.png.meta", rockMeta);
		tree.WriteProjectContent("Pack2/Skin.png.meta", skinMeta);
		SAssetOp op;
		std::vector<SDiagnostic> problems;
		ASSERT_TRUE(PlanRepairOp(writer, rig.View(), actions, op, problems));
		ASSERT_EQ(op.Steps.size(), 2u);
		EXPECT_EQ(op.PlannedGuids.size(), 2u);
		writer.SetStopAfterStepForTest(stop);
		(void)writer.Run(op);
		writer.SetStopAfterStepForTest(~0u);
		const FString context = FString::Format("stopped after {}", stop);
		ASSERT_TRUE(writer.GetUnfinished().has_value()) << context.CStr();
		EXPECT_EQ(writer.GetUnfinished()->Op.PlannedGuids, op.PlannedGuids) << "no planned identity is lost";
		FString error;
		ASSERT_TRUE(writer.RollForward(&error)) << error.CStr();
		ExpectPackDeduped(tree, context);
	}

	// One real kill, mid-op: after the first loser's write.
	tree.WriteProjectContent("Pack2/Rock.png.meta", rockMeta);
	tree.WriteProjectContent("Pack2/Skin.png.meta", skinMeta);
	std::vector<FString> arguments = { FString(test::ChildModeSwitch) + "repair-dedupe-kill", FString::FromPath(tree.Tree.EngineDir()),
		FString::FromPath(tree.Tree.EngineContent()), FString::FromPath(tree.Tree.EngineConfig()), FString::FromPath(tree.Tree.ProjectDir()), "1" };
	for (const SLifecycleAction& action : actions)
	{
		arguments.push_back(action.Path);
		arguments.push_back(action.Remap.front().first.ToString());
		arguments.push_back(action.Guid.ToString());
	}
	test::CTestProcess child;
	FString error;
	ASSERT_TRUE(child.Start(platform::GetExecutablePath(), arguments, false, {}, &error)) << error.CStr();
	ASSERT_TRUE(test::WaitUntil([&child] { return child.ReadOutput().Contains("stopped") || !child.IsRunning(); })) << child.ReadErrors().CStr();
	ASSERT_TRUE(child.ReadOutput().Contains("stopped")) << child.ReadOutput().CStr() << child.ReadErrors().CStr();
	child.Kill();
	child.Wait();
	ASSERT_TRUE(writer.GetUnfinished().has_value()) << "the killed process left its op in the journal";
	ASSERT_TRUE(writer.RollForward(&error)) << error.CStr();
	ExpectPackDeduped(tree, "killed after one step");
}

// ---------------- Plans that go stale (F1) ----------------

// A slow sync delivers the committed meta after its source, inside the window between a create's plan and its write: the
// committed GUID stays, nothing is refused, and the next run pairs it.
TEST(AssetRepairs, AMetaThatArrivesBetweenARepairsPlanAndItsWriteIsKept)
{
	CRepairTree tree("Запізнілий мета");
	tree.WritePng("a.png");
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	bool bHooked = false;
	rig.Repairs().GetWriter()->SetBeforeRunForTest([&] (const SAssetOp& Op)
	{
		bHooked = Op.Verb == FStringView("create-meta");
		tree.WriteMeta("a.png", MetaText(GuidA));
	});
	rig.Settle(1600u);
	EXPECT_TRUE(bHooked) << "the create was planned and run";
	EXPECT_EQ(tree.MetaGuid("a.png"), MakeGuid(GuidA)) << "the delivered meta is never replaced by a derived one";
	rig.Settle(100u);
	EXPECT_EQ(rig.GuidAt(P("a.png")), MakeGuid(GuidA));
	EXPECT_EQ(rig.StateOf(P("a.png")), EMetaState::Paired);
	EXPECT_EQ(rig.Registry->GetCounters().RepairsRefused, 0u);
	EXPECT_EQ(rig.Registry->GetCounters().RepairsApplied, 0u);
	EXPECT_TRUE(rig.Problems().empty()) << rig.DescribeProblems().CStr();
	for (const CRecordingVcsProvider::SCall& call : rig.Vcs.GetWriteCalls())
	{
		EXPECT_NE(call.Name, "Add");
	}
}

// The editor died between a create's Begin and its write; a pull then brought the meta it was about to create. Start's
// roll-forward keeps the pulled meta.
TEST(AssetRepairs, AStartUpRollForwardKeepsAMetaPulledAfterTheCrash)
{
	CRepairTree tree("Підтягнутий мета");
	tree.WritePng("a.png");
	(void)LeaveUnfinishedCreate(tree, "a.png");
	tree.WriteMeta("a.png", MetaText(GuidA));
	CRig rig(tree);
	rig.Open();
	EXPECT_EQ(tree.MetaGuid("a.png"), MakeGuid(GuidA));
	EXPECT_EQ(rig.GuidAt(P("a.png")), MakeGuid(GuidA));
	EXPECT_FALSE(rig.Repairs().GetWriter()->GetUnfinished().has_value()) << "the op is finished";
}

namespace
{
// Two copies that reference each other, so either loser of their dedupe holds a reference to the other.
void WriteTwins (const CRepairTree& Tree)
{
	Tree.WritePng("Twins/Rock.png", 4u);
	Tree.WriteMeta("Twins/Rock.png", MetaText(GuidA, FString::Format("SubAssets:\n  Remap:\n    \"material:Other\": {{ $asset: {} }}\n", GuidB)));
	Tree.WritePng("Twins/Skin.png", 5u);
	Tree.WriteMeta("Twins/Skin.png", MetaText(GuidB, FString::Format("SubAssets:\n  Remap:\n    \"material:Other\": {{ $asset: {} }}\n", GuidA)));
}

[[nodiscard]] FString OtherTarget (const CRepairTree& Tree, FStringView Relative)
{
	const std::optional<SAssetMeta> meta = Tree.ReadMeta(Relative);
	return meta.has_value() ? FString(meta->SubAssets.GetRoot().Find("Remap").FindChild("material:Other").FindChild("$asset").GetText()) : FString();
}
}

// A dedupe op over two losers is left in the journal after its first write (a sharing violation on the second, say). The
// next run sees only the second loser as a duplicate and plans it alone, with a narrower map; the writer finishes the first
// op instead of running that plan over it, so the second loser keeps its reference to the first one's copy.
TEST(AssetRepairs, ADedupeSetLeftHalfWrittenKeepsItsFullRemapWhenTheRestIsPlannedNarrower)
{
	CRepairTree tree("Половина набору");
	WriteTwins(tree);
	CRig rig(tree);
	rig.Open();
	rig.Settle();
	std::error_code ec;
	std::filesystem::copy(tree.Abs("Twins"), tree.Abs("Twins2"), std::filesystem::copy_options::recursive, ec);
	ASSERT_FALSE(ec) << ec.message();
	rig.Scan("/Project/Twins2");
	rig.Settle();
	rig.Repairs().GetWriter()->SetStopAfterStepForTest(1u);
	rig.Settle(1600u);
	rig.Settle(1600u);
	rig.Settle(1600u);

	const SGuid rock = MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Twins2/Rock.png"));
	const SGuid skin = MakeDedupeGuid(Salt, MakeGuid(GuidB), P("Twins2/Skin.png"));
	EXPECT_EQ(tree.MetaGuid("Twins2/Rock.png"), rock);
	EXPECT_EQ(tree.MetaGuid("Twins2/Skin.png"), skin);
	EXPECT_EQ(OtherTarget(tree, "Twins2/Rock.png"), skin.ToString()) << "each copy references the other's copy";
	EXPECT_EQ(OtherTarget(tree, "Twins2/Skin.png"), rock.ToString()) << "each copy references the other's copy";
	EXPECT_EQ(OtherTarget(tree, "Twins/Rock.png"), FString(GuidB));
	EXPECT_FALSE(rig.Repairs().GetWriter()->GetUnfinished().has_value());
}

// ---------------- Through the file watcher ----------------

TEST(AssetRepairs, ALiveRegistryCreatesAMetaForAFileTheWatcherReportsWithItsOwnClock)
{
	CRepairTree tree("Наживо");
	SFileWatcherSettings settings;
	settings.SettleMs = 20u;
	settings.VcsSettleMs = 30u;
	CFileWatcherService watcher(settings);
	CRig rig(tree, [&] (SLifecycleDesc& Lifecycle, SAssetRegistryDesc& Desc)
	{
		Lifecycle.bLive = true;
		Lifecycle.Settings.MetaCreationDelayMs = 40u;
		Lifecycle.Settings.RemoveGraceMs = 20u;
		Lifecycle.Settings.VcsSettleMs = 30u;
		Desc.Watcher = &watcher;
	});
	for (const SMount& mount : rig.Desc.Mounts->GetMounts())
	{
		watcher.AddMount(mount.ContentDir);
		watcher.AddMount(mount.RulesFile.parent_path());
	}
	rig.Open();
	tree.WritePng("Textures/a.png");
	const bool bCreated = test::WaitUntil([&]
	{
		watcher.Pump();
		rig.Pool.PumpCompletions();
		rig.Registry->Pump();
		return tree.Exists("Textures/a.png.meta") && rig.StateOf(P("Textures/a.png")) == EMetaState::Paired;
	});
	ASSERT_TRUE(bCreated) << rig.DescribeProblems().CStr();
	EXPECT_EQ(tree.MetaGuid("Textures/a.png"), DerivedGuid(P("Textures/a.png")));
	EXPECT_EQ(rig.GuidAt(P("Textures/a.png")), DerivedGuid(P("Textures/a.png")));
	rig.Registry->ShutDown();
	rig.Registry.reset();
}
