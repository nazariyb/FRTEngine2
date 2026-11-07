// The owner runtime of the meta lifecycle (Assets/Meta/AssetRepairs.h, design §4.6-§4.8, §12.9, M4's F4), end to end on
// small trees through a real registry: a new file gets its derived meta once its delay is over and not before, through
// the provider (asset and meta in one call), and a second round changes nothing; a slow checkout (busy, then the meta a
// little later) never mints a meta; a folder copy re-GUIDs the copies as one op with their references remapped to each
// other; an Explorer move pairs the orphan meta by FileId; a delete then restore of a pair, and a lost meta restored byte
// for byte from METAS, keep the identity; each default-on switch off reduces exactly its repair to a problem with a fix-it
// (and the fix-it, applied, repairs it), WaitForVcs off ignores a busy provider; a refused CheckOut stays a proposal with
// VCS002 until an event on the path; a meta created beside a tracked source is VCS003; a read-only process, a mount it
// does not own and a pending provider detection write nothing; RunNow has no delays and honours its Kinds and dry run;
// an unfinished operation is rolled forward before the first scan (and by TakeOwnership); problems carry their file and
// the rule files' Validation: severities; CRLF in a meta is VCS001 and an LFS pointer VCS004. Each test holds at most
// ten files and drives the lifecycle with its own clock (Settle), never with sleeps: only one test goes through the
// file watcher with small delays. The dedupe op's crash points are rolled forward in-process, and one real child is killed.
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
#include "Assets/FileWatcherService.h"
#include "Assets/Meta/AssetCommands.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/MountPaths.h"
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
	EXPECT_TRUE(vcs003[0].Message.Contains("commit it"));
	rig.Settle(5000u);
	EXPECT_EQ(rig.Problems(Vcs003).size(), 1u) << "kept while the pair stands";
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
	rig.Repairs().SetProvider(ProjectMountId, &rig.Vcs);
	rig.Settle(100u);
	EXPECT_EQ(tree.MetaGuid("a.png"), DerivedGuid(P("a.png")));
	EXPECT_EQ(rig.Repairs().GetProvider(ProjectMountId), &rig.Vcs);
	EXPECT_EQ(providers.Find(ProjectMountId)->GetName(), "None");
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
