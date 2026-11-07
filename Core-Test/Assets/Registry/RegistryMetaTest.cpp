// Identity in the registry (Assets/AssetRegistry.h, design §5.2-§5.4, M4's E4): a file's meta gives its record the
// meta's GUID, and a FILES row of class Meta that no count, `files` field or dump line shows; a file without a meta
// keeps its provisional id and dumps as in M3, so a provisional id never reaches a golden; an unchanged meta of an
// unchanged file is never opened, a meta edit re-evaluates the record in memory, a lost meta keeps the identity and a
// broken one its salvaged GUID (a provisional id, when it salvages none, is no identity: fixing that meta tombstones
// nothing); the meta's values beat the rules (an Importer pin claims a file no rule claims); two
// records with one GUID answer for the smallest path and a move with its meta is a move to listeners; SubAssets
// renames keep an id and remaps add an edge. E4b: the meta's Aliases and FormerPaths resolve lookups and its bytes are
// kept in METAS (a re-evaluation reads them there, never the disk); a removed file with a meta-given GUID, or a GUID
// edited by hand, leaves a tombstone that the identity's return clears; a warm start keeps all of it. Small non-ASCII
// trees in the temp directory, no lifecycle (null desc).
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "RegistryTestFixture.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTypes.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Registry/RegistryRecords.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Reflect/DataTreeJson.h"

using namespace frt;
using namespace frt::assets;
using frt::test::rules::CRulesTree;


namespace
{
constexpr FStringLiteral GuidA = "5d1e7a3b9c0f42d68e1a2b3c4d5e6f70";
constexpr FStringLiteral GuidB = "3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b";
constexpr FStringLiteral GuidC = "1c7e0a9b33d24f1e9a6b0c1d2e3f4a5b";
constexpr FStringLiteral GuidD = "8b4d2f6a1e9c4a7b8d3e5f60718293a4";

SGuid MakeGuid (FStringView Text)
{
	const std::optional<SGuid> guid = SGuid::Parse(Text);
	EXPECT_TRUE(guid.has_value());
	return guid.value_or(SGuid {});
}

// A PNG's header as stb_image's info reads it (AssetRegistryTest's).
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

// "FormatVersion: 1", the GUID, then Rest (more keys, already in YAML).
FString MetaText (FStringView Guid, FStringView Rest = {})
{
	return FString::Format("FormatVersion: 1\nGuid: {}\n{}", Guid, Rest);
}

class CMetaTree : public CRulesTree
{
public:
	explicit CMetaTree (FStringView Name)
		: CRulesTree(Name)
	{}

	void WritePng (FStringView Relative) const
	{
		const std::filesystem::path path = Tree.ProjectContent() / Relative.ToPath();
		frt::test::rules::MakeDirectories(path.parent_path());
		ASSERT_TRUE(platform::WriteFileBytes(path, MakePng(4u)));
	}
	void WriteMeta (FStringView Relative, FStringView Text) const { WriteProjectContent(GetMetaPath(Relative), Text); }
	void Remove (FStringView Relative) const
	{
		std::error_code ec;
		std::filesystem::remove(Tree.ProjectContent() / Relative.ToPath(), ec);
	}
	void Rename (FStringView From, FStringView To) const
	{
		std::error_code ec;
		std::filesystem::create_directories((Tree.ProjectContent() / To.ToPath()).parent_path(), ec);
		std::filesystem::rename(Tree.ProjectContent() / From.ToPath(), Tree.ProjectContent() / To.ToPath(), ec);
		ASSERT_FALSE(ec) << ec.message();
	}
	[[nodiscard]] std::shared_ptr<const CMountTable> Mounts () const { return std::make_shared<const CMountTable>(Resolve()); }
};

jobs::SWorkerPoolDesc MakePoolDesc ()
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = 2u;
	desc.Name = "Registry Meta Test";
	return desc;
}

std::unique_ptr<CAssetRegistry> Open (jobs::CWorkerPool& Pool, std::shared_ptr<const CMountTable> Mounts, bool bWriter = false)
{
	SAssetRegistryDesc desc;
	desc.Mounts = std::move(Mounts);
	desc.Pool = &Pool;
	desc.bWriter = bWriter;
	desc.bLoadSnapshots = bWriter;
	auto registry = std::make_unique<CAssetRegistry>(std::move(desc));
	registry->Start();
	EXPECT_TRUE(registry->WaitForInitialScan(60.0));
	return registry;
}

FString DumpOf (const CRegistryView& View)
{
	FString text;
	View.DumpText(text);
	return text;
}

std::optional<SAssetData> FindById (const CRegistryView& View, const SAssetId& Id)
{
	SAssetData data;
	const std::optional<SRecordId> record = View.Find(Id);
	return record.has_value() && View.GetRecord(*record, data) ? std::optional<SAssetData>(data) : std::nullopt;
}

FString PathOf (const CRegistryView& View, FStringView Guid)
{
	const std::optional<SAssetData> data = FindById(View, SAssetId { MakeGuid(Guid), 0u });
	return data.has_value() ? data->MountPath : FString();
}

SGuid GuidAt (const CRegistryView& View, FStringView MountPath)
{
	SAssetData data;
	const std::optional<SRecordId> record = View.FindByPath(MountPath);
	return record.has_value() && View.GetRecord(*record, data) ? data.Id.Guid : SGuid {};
}

std::vector<SDiagnostic> ProblemsOf (const CRegistryView& View, uint32 Code)
{
	std::vector<SDiagnostic> found;
	View.ForEachProblem([&] (FStringView, EProblemSource, const SDiagnostic& Diagnostic)
	{
		if (Diagnostic.Code == Code)
		{
			found.push_back(Diagnostic);
		}
	});
	return found;
}
}


TEST(RegistryMeta, AFileWithAMetaTakesItsGuidAndAMetaRowThatNoCountOrDumpLineShows)
{
	CMetaTree tree("Ідентичність");
	tree.WritePng("Textures/Камінь.png");
	const FString metaText = MetaText(GuidA, "Labels:\n  - Rock\n");
	tree.WriteMeta("Textures/Камінь.png", metaText);
	tree.WritePng("Textures/Трава.png");
	tree.WriteMeta("Textures/Gone.png", MetaText(GuidB));
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	const CRegistryView& view = registry->Live();

	EXPECT_EQ(PathOf(view, GuidA), "/Project/Textures/Камінь.png");
	const std::optional<SStoredFile> rock = FindStoredFile(view, "/Project/Textures/Камінь.png");
	ASSERT_TRUE(rock.has_value());
	EXPECT_EQ(rock->MetaState, EMetaState::Paired);
	EXPECT_EQ(rock->MetaHash, HashMetaBytes(metaText));
	const std::optional<SStoredFile> rockMeta = FindStoredFile(view, "/Project/Textures/Камінь.png.meta");
	ASSERT_TRUE(rockMeta.has_value());
	EXPECT_EQ(rockMeta->Class, EFileClass::Meta);
	EXPECT_EQ(rockMeta->MetaState, EMetaState::Paired);
	EXPECT_FALSE(rockMeta->Record.IsValid());
	const std::optional<SStoredFile> orphan = FindStoredFile(view, "/Project/Textures/Gone.png.meta");
	ASSERT_TRUE(orphan.has_value());
	EXPECT_EQ(orphan->Class, EFileClass::Meta);
	EXPECT_EQ(orphan->MetaState, EMetaState::Orphan);
	EXPECT_TRUE(PathOf(view, GuidB).IsEmpty()) << "an orphan's bytes are not read: it names no record";

	const std::optional<SStoredFile> grass = FindStoredFile(view, "/Project/Textures/Трава.png");
	ASSERT_TRUE(grass.has_value());
	EXPECT_EQ(grass->MetaState, EMetaState::PendingMeta);
	EXPECT_EQ(grass->Guid, MakeProvisionalGuid("/Project/Textures/Трава.png"));

	EXPECT_EQ(view.GetMetaFileCount(), 2u);
	uint32 rows = 0u;
	view.ForEachFile([&] (uint32) { ++rows; });
	EXPECT_EQ(view.GetFileCount(), rows - 2u) << "metas are no files of the count";

	const FString dump = DumpOf(view);
	const size_t pngSize = MakePng(4u).size();
	EXPECT_TRUE(dump.View().Contains(FString::Format("  file /Project/Textures/Камінь.png Asset size={} meta=Paired\n    record guid={} type=frt.Texture",
		pngSize, GuidA).View())) << dump.CStr();
	EXPECT_TRUE(dump.View().Contains(FStringView("    labels authored:Rock\n"))) << dump.CStr();
	EXPECT_TRUE(dump.View().Contains(FString::Format("  file /Project/Textures/Трава.png Asset size={}\n    record type=frt.Texture", pngSize).View()))
		<< dump.CStr();
	EXPECT_FALSE(dump.View().Contains(FStringView(".meta"))) << "no meta has a line: " << dump.CStr();
	uint32 guids = 0u;
	view.DumpJson([&] (FStringView Line)
	{
		reflect::CDataTree tree;
		ASSERT_TRUE(reflect::ParseJson(Line, tree)) << FString(Line).CStr();
		const FStringView kind = tree.Find("kind").AsText().value_or(FStringView());
		const FStringView path = tree.Find("path").AsText().value_or(FStringView());
		EXPECT_FALSE(path.EndsWith(FStringView(".meta"))) << FString(Line).CStr();
		if (path == FStringView("/Project/Textures/Камінь.png"))
		{
			EXPECT_EQ(tree.Find(kind == FStringView("file") ? "meta" : "guid").AsText().value_or(FStringView()),
				kind == FStringView("file") ? FStringView("Paired") : GuidA.View()) << FString(Line).CStr();
			guids += kind == FStringView("record") ? 1u : 0u;
		}
		if (path == FStringView("/Project/Textures/Трава.png"))
		{
			EXPECT_FALSE(tree.Find("guid").IsValid()) << FString(Line).CStr();
			EXPECT_FALSE(tree.Find("meta").IsValid()) << FString(Line).CStr();
		}
	});
	EXPECT_EQ(guids, 1u);
	registry->ShutDown();
}

TEST(RegistryMeta, AWarmStartOpensNoMetaAndAMetaEditIsReEvaluatedWithoutAnalyze)
{
	CMetaTree tree("Тепло");
	tree.WritePng("Textures/Камінь.png");
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidA));
	jobs::CWorkerPool pool(MakePoolDesc());
	FString cold;
	{
		std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts(), true);
		EXPECT_EQ(registry->GetCounters().MetasRead, 1u);
		cold = DumpOf(registry->Live());
		registry->ShutDown();
	}

	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts(), true);
	const CRegistryView& view = registry->Live();
	EXPECT_EQ(DumpOf(view), cold);
	EXPECT_EQ(registry->GetCounters().MetasRead, 0u) << "an unchanged meta of an unchanged file is never opened";
	EXPECT_EQ(registry->GetCounters().Analyzed, 0u);

	const std::vector<FString> folder = { "/Project/Textures" };
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidA, "Labels:\n  - Rock\nCook: Never\n"));
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(registry->GetCounters().Analyzed, 0u) << "Cook and Labels are not Analyze-relevant";
	EXPECT_EQ(registry->GetCounters().MetasRead, 1u);
	EXPECT_GE(registry->GetCounters().ReEvaluated, 1u);
	SAssetData data;
	const std::optional<SRecordId> record = registry->Live().FindByPath("/Project/Textures/Камінь.png");
	ASSERT_TRUE(record.has_value());
	const FString edited = DumpOf(registry->Live());
	EXPECT_TRUE(edited.View().Contains(FStringView("cook=Never"))) << edited.CStr();
	EXPECT_TRUE(edited.View().Contains(FStringView("labels authored:Rock"))) << edited.CStr();

	// Another GUID in the meta: the record takes it, in memory; the old id answers nothing, and to a listener the old
	// id went and the new one came (never a Modified of an id it never saw added).
	std::vector<SAssetChangeSet> changes;
	const SDelegateHandle handle = registry->OnAssetsChanged().Add([&] (const SAssetChangeSet& Changes) { changes.push_back(Changes); });
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidC, "# the GUID was edited by hand\nLabels:\n  - Rock\nCook: Never\n"));
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	registry->OnAssetsChanged().Remove(handle);
	EXPECT_EQ(PathOf(registry->Live(), GuidC), "/Project/Textures/Камінь.png");
	EXPECT_TRUE(PathOf(registry->Live(), GuidA).IsEmpty());
	EXPECT_EQ(registry->GetCounters().Analyzed, 0u);
	std::vector<SAssetChange> added;
	std::vector<SAssetChange> removed;
	for (const SAssetChangeSet& set : changes)
	{
		EXPECT_FALSE(set.bRefreshAll);
		EXPECT_TRUE(set.Modified.empty()) << set.Modified[0].MountPath.CStr();
		EXPECT_TRUE(set.Moved.empty());
		added.insert(added.end(), set.Added.begin(), set.Added.end());
		removed.insert(removed.end(), set.Removed.begin(), set.Removed.end());
	}
	ASSERT_EQ(added.size(), 1u);
	ASSERT_EQ(removed.size(), 1u);
	EXPECT_EQ(added[0].Id, (SAssetId { MakeGuid(GuidC), 0u }));
	EXPECT_EQ(added[0].MountPath, "/Project/Textures/Камінь.png");
	EXPECT_EQ(removed[0].Id, (SAssetId { MakeGuid(GuidA), 0u }));
	EXPECT_EQ(removed[0].MountPath, "/Project/Textures/Камінь.png");

	// The meta lost: identity stays (no provisional id), and so does the stored pairing until the lifecycle decides.
	tree.Remove("Textures/Камінь.png.meta");
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidC), "/Project/Textures/Камінь.png");
	EXPECT_EQ(registry->Live().GetMetaFileCount(), 0u);
	EXPECT_EQ(FindStoredFile(registry->Live(), "/Project/Textures/Камінь.png")->MetaState, EMetaState::Paired);

	// A broken meta: its salvaged GUID, and the state Broken.
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidB, "Settings: [\n"));
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidB), "/Project/Textures/Камінь.png");
	EXPECT_EQ(FindStoredFile(registry->Live(), "/Project/Textures/Камінь.png")->MetaState, EMetaState::Broken);
	EXPECT_TRUE(DumpOf(registry->Live()).View().Contains(FStringView("meta=Broken")));
	EXPECT_TRUE(ProblemsOf(registry->Live(), Meta004).empty()) << "no lifecycle, no META problems";

	// A broken meta with no GUID to salvage: the record keeps a provisional id, which no dump shows.
	tree.WritePng("Textures/Трава.png");
	tree.WriteMeta("Textures/Трава.png", "FormatVersion: 1\nSettings: [\n");
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	const std::optional<SStoredFile> grass = FindStoredFile(registry->Live(), "/Project/Textures/Трава.png");
	ASSERT_TRUE(grass.has_value());
	EXPECT_EQ(grass->MetaState, EMetaState::Broken);
	EXPECT_EQ(grass->Guid, MakeProvisionalGuid("/Project/Textures/Трава.png"));
	const FString broken = DumpOf(registry->Live());
	EXPECT_TRUE(broken.View().Contains(FString::Format("  file /Project/Textures/Трава.png Asset size={} meta=Broken\n    record type=frt.Texture",
		MakePng(4u).size()).View())) << broken.CStr();
	EXPECT_TRUE(broken.View().Contains(FString::Format("record guid={}", GuidB).View())) << broken.CStr();
	uint32 guids = 0u;
	registry->Live().DumpJson([&] (FStringView Line)
	{
		if (Line.Contains(FStringView("Трава.png")))
		{
			EXPECT_FALSE(Line.Contains(FStringView("\"guid\""))) << FString(Line).CStr();
		}
		guids += Line.Contains(FStringView("\"guid\"")) ? 1u : 0u;
	});
	EXPECT_EQ(guids, 1u) << "only Камінь.png's record has a meta GUID";

	// That meta fixed by hand: the provisional id was never an identity, so it leaves no tombstone (§4.6's "GUID edited
	// by hand" is about a meta-given one).
	const uint32 tombstones = registry->Live().GetTombstoneCount();
	tree.WriteMeta("Textures/Трава.png", MetaText(GuidD));
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidD), "/Project/Textures/Трава.png");
	EXPECT_FALSE(registry->Live().FindTombstone(MakeProvisionalGuid("/Project/Textures/Трава.png")).has_value());
	EXPECT_EQ(registry->Live().GetTombstoneCount(), tombstones);
	registry->ShutDown();
}

TEST(RegistryMeta, AnImporterPinClaimsAFileAndAWarmStartAnalyzesNothing)
{
	CMetaTree tree("Закріплення");
	tree.WriteProjectContent("Notes/дані.bin", "data");
	tree.WriteMeta("Notes/дані.bin", MetaText(GuidA, "Importer: Raw\n"));
	tree.WriteProjectContent("Notes/інше.bin", "other");
	jobs::CWorkerPool pool(MakePoolDesc());
	FString cold;
	{
		std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts(), true);
		const CRegistryView& view = registry->Live();
		const std::optional<SStoredFile> pinned = FindStoredFile(view, "/Project/Notes/дані.bin");
		ASSERT_TRUE(pinned.has_value());
		EXPECT_EQ(pinned->Class, EFileClass::Raw);
		EXPECT_EQ(pinned->Type, asset_types::Raw);
		EXPECT_EQ(pinned->Guid, MakeGuid(GuidA));
		const std::vector<SDiagnostic> unclaimed = ProblemsOf(view, Rule002);
		ASSERT_EQ(unclaimed.size(), 1u);
		EXPECT_TRUE(unclaimed[0].Message.View().Contains(FStringView("інше.bin"))) << unclaimed[0].Message.CStr();
		EXPECT_FALSE(unclaimed[0].Message.View().Contains(FStringView("2 files"))) << unclaimed[0].Message.CStr();
		cold = DumpOf(view);
		registry->ShutDown();
	}
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts(), true);
	EXPECT_EQ(DumpOf(registry->Live()), cold);
	EXPECT_EQ(registry->GetCounters().Analyzed, 0u);
	EXPECT_EQ(registry->GetCounters().ReEvaluated, 0u);
	registry->ShutDown();
}

TEST(RegistryMeta, TwoRecordsWithOneGuidAnswerForTheSmallestPathAndAMoveWithItsMetaIsAMove)
{
	CMetaTree tree("Копія");
	tree.WritePng("Textures/B.png");
	tree.WriteMeta("Textures/B.png", MetaText(GuidA));
	tree.WritePng("Textures/a.png");
	tree.WriteMeta("Textures/a.png", MetaText(GuidA));
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	EXPECT_EQ(PathOf(registry->Live(), GuidA), "/Project/Textures/a.png") << "the smallest folded path, not the first row";
	EXPECT_EQ(GuidAt(registry->Live(), "/Project/Textures/B.png"), MakeGuid(GuidA)) << "both records are kept";
	EXPECT_TRUE(ProblemsOf(registry->Live(), Meta003).empty()) << "no lifecycle, no META problems";

	std::vector<SAssetChangeSet> changes;
	const SDelegateHandle handle = registry->OnAssetsChanged().Add([&] (const SAssetChangeSet& Changes) { changes.push_back(Changes); });
	const uint64 before = registry->GetRevision();
	tree.Remove("Textures/a.png");
	tree.Remove("Textures/a.png.meta");
	tree.Rename("Textures/B.png", "Moved/C.png");
	tree.Rename("Textures/B.png.meta", "Moved/C.png.meta");
	const std::vector<FString> root = { "/Project" };
	ASSERT_TRUE(registry->ScanPathsSynchronous(root, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidA), "/Project/Moved/C.png");
	registry->OnAssetsChanged().Remove(handle);

	std::vector<FString> moved;
	uint32 removed = 0u;
	for (const SAssetChangeSet& set : changes)
	{
		EXPECT_FALSE(set.bRefreshAll);
		for (const SAssetChange& change : set.Moved)
		{
			EXPECT_EQ(change.Id.Guid, MakeGuid(GuidA));
			moved.push_back(change.MountPath);
		}
		removed += static_cast<uint32>(set.Removed.size());
		for (const SAssetChange& change : set.Added)
		{
			ADD_FAILURE() << "added " << change.MountPath.CStr();
		}
	}
	EXPECT_EQ(moved, std::vector<FString> { "/Project/Moved/C.png" });
	EXPECT_EQ(removed, 1u) << "a.png went; B.png moved";

	// The move is one commit (ConvertBatch pairs it inside the pass), not a removal and an addition that one broadcast
	// window happens to hold: one record of GUID A (a.png or B.png, both hold it) goes in the revision C.png's comes in,
	// the other in the pass's removals before it.
	std::vector<SRecordChange> ring;
	EXPECT_TRUE(registry->ForEachChangeSince(before, [&] (const SRecordChange& Change) { ring.push_back(Change); }));
	const auto added = std::find_if(ring.begin(), ring.end(), [] (const SRecordChange& Change)
	{
		return Change.Kind == ERecordChange::Added && Change.MountPath == "/Project/Moved/C.png";
	});
	ASSERT_NE(added, ring.end());
	uint32 removedWith = 0u;
	uint32 removedBefore = 0u;
	for (const SRecordChange& change : ring)
	{
		if (change.Kind == ERecordChange::Removed && change.Id == SAssetId { MakeGuid(GuidA), 0u })
		{
			removedWith += change.Revision == added->Revision ? 1u : 0u;
			removedBefore += change.Revision < added->Revision ? 1u : 0u;
		}
	}
	EXPECT_EQ(removedWith, 1u);
	EXPECT_EQ(removedBefore, 1u);
	registry->ShutDown();
}

/**
 * OnAssetsChanged's lists from one broadcast window of the change ring, in ring order: an id that left a path it held
 * and holds one it did not is one Moved, whichever came first; a record added and removed within the window, a path
 * that went and came back (a rename there and back, a metaless file deleted and created again) stay Added and Removed.
 */
TEST(RegistryMeta, AMoveIsAnIdThatLeftAPathAndHoldsAnotherAtTheEndOfTheWindow)
{
	const SAssetId id { MakeGuid(GuidA), 0u };
	const SAssetId other { MakeGuid(GuidB), 0u };
	const auto change = [] (uint64 Revision, ERecordChange Kind, const SAssetId& Id, FStringView Path)
	{
		return SRecordChange { Revision, Kind, Id, FString(Path) };
	};
	const auto paths = [] (const std::vector<SAssetChange>& List)
	{
		std::vector<FString> out;
		for (const SAssetChange& entry : List)
		{
			out.push_back(entry.MountPath);
		}
		return out;
	};
	using FPaths = std::vector<FString>;
	constexpr ERecordChange Added = ERecordChange::Added;
	constexpr ERecordChange Removed = ERecordChange::Removed;
	constexpr ERecordChange Modified = ERecordChange::Modified;

	{
		// A rename with its meta, in one commit and then edited: one Moved and the edit.
		SAssetChangeSet set;
		const std::vector<SRecordChange> window = { change(1u, Removed, id, "/Project/B.png"), change(1u, Added, id, "/Project/C.png"),
			change(2u, Modified, id, "/Project/C.png"), change(2u, Added, other, "/Project/D.png") };
		AddRecordChanges(window, set);
		EXPECT_EQ(paths(set.Moved), FPaths { "/Project/C.png" });
		EXPECT_EQ(paths(set.Modified), FPaths { "/Project/C.png" });
		EXPECT_EQ(paths(set.Added), FPaths { "/Project/D.png" });
		EXPECT_TRUE(set.Removed.empty());
	}
	{
		// A copy with its meta (both records hold the id) deleted again: no move to a path that is gone.
		SAssetChangeSet set;
		const std::vector<SRecordChange> window = { change(1u, Added, id, "/Project/Copy.png"), change(2u, Removed, id, "/Project/Copy.png") };
		AddRecordChanges(window, set);
		EXPECT_TRUE(set.Moved.empty());
		EXPECT_EQ(paths(set.Added), FPaths { "/Project/Copy.png" });
		EXPECT_EQ(paths(set.Removed), FPaths { "/Project/Copy.png" });
	}
	{
		// A -> B -> A: the id is where it was.
		SAssetChangeSet set;
		const std::vector<SRecordChange> window = { change(1u, Removed, id, "/Project/A.png"), change(1u, Added, id, "/Project/B.png"),
			change(2u, Removed, id, "/Project/B.png"), change(2u, Added, id, "/Project/A.png") };
		AddRecordChanges(window, set);
		EXPECT_TRUE(set.Moved.empty());
		EXPECT_EQ(paths(set.Added), (FPaths { "/Project/B.png", "/Project/A.png" }));
		EXPECT_EQ(paths(set.Removed), (FPaths { "/Project/A.png", "/Project/B.png" }));
	}
	{
		// A file deleted and created again at its path (a metaless one keeps its path-derived id): no move.
		SAssetChangeSet set;
		const std::vector<SRecordChange> window = { change(1u, Removed, id, "/Project/A.png"), change(2u, Added, id, "/Project/A.png") };
		AddRecordChanges(window, set);
		EXPECT_TRUE(set.Moved.empty());
		EXPECT_EQ(paths(set.Removed), FPaths { "/Project/A.png" });
		EXPECT_EQ(paths(set.Added), FPaths { "/Project/A.png" });
	}
	{
		// A copy with its meta, then the original deleted: the asset ends at the copy's path, one Moved.
		SAssetChangeSet set;
		const std::vector<SRecordChange> window = { change(1u, Added, id, "/Project/Copy.png"), change(2u, Removed, id, "/Project/A.png") };
		AddRecordChanges(window, set);
		EXPECT_EQ(paths(set.Moved), FPaths { "/Project/Copy.png" });
		EXPECT_TRUE(set.Added.empty());
		EXPECT_TRUE(set.Removed.empty());
	}
	{
		// Two metas swapped (each record re-GUIDed in place): each id moved to the other path.
		SAssetChangeSet set;
		const std::vector<SRecordChange> window = { change(1u, Removed, id, "/Project/A.png"), change(1u, Added, other, "/Project/A.png"),
			change(1u, Removed, other, "/Project/B.png"), change(1u, Added, id, "/Project/B.png") };
		AddRecordChanges(window, set);
		ASSERT_EQ(set.Moved.size(), 2u);
		EXPECT_EQ(set.Moved[0].Id, other);
		EXPECT_EQ(set.Moved[0].MountPath, "/Project/A.png");
		EXPECT_EQ(set.Moved[1].Id, id);
		EXPECT_EQ(set.Moved[1].MountPath, "/Project/B.png");
		EXPECT_TRUE(set.Added.empty());
		EXPECT_TRUE(set.Removed.empty());
	}
}

TEST(RegistryMeta, SubAssetsRenamesKeepAnIdAndRemapsAddAnEdgeToTheExternalAsset)
{
	CMetaTree tree("Підресурси");
	tree.WritePng("Materials/Шкіра.png");
	tree.WriteMeta("Materials/Шкіра.png", MetaText(GuidA));
	tree.WriteProjectContent("Models/Box.gltf",
		R"({"asset":{"version":"2.0"},"meshes":[{"name":"Body","primitives":[]}],"materials":[{"name":"Skin"}]})");
	const FString subAssets =
		"SubAssets:\n"
		"  Renamed:\n"
		"    \"mesh:Body\": \"mesh:Torso\"\n"
		"  Remap:\n"
		"    \"material:Skin\": { $asset: " + FString(GuidA) + ", path: /Project/Materials/Шкіра.png }\n"
		"  Overrides:\n"
		"    \"image:#9\":\n"
		"      ColorSpace: Srgb\n";
	tree.WriteMeta("Models/Box.gltf", MetaText(GuidB, subAssets));
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	const CRegistryView& view = registry->Live();

	const SGuid box = MakeGuid(GuidB);
	const std::optional<SAssetData> torso = FindById(view, SAssetId { box, MakeSubId("mesh:Torso") });
	ASSERT_TRUE(torso.has_value()) << DumpOf(view).CStr();
	EXPECT_EQ(torso->Name, "mesh:Body") << "the new label keeps the old id";
	// The new label's own id resolves to it through the alias index (E4b), flagged as a redirect.
	bool bRedirected = false;
	const std::optional<SRecordId> byNewLabel = view.Find(SAssetId { box, MakeSubId("mesh:Body") }, &bRedirected);
	ASSERT_TRUE(byNewLabel.has_value());
	EXPECT_EQ(byNewLabel->Value, torso->Record.Value);
	EXPECT_TRUE(bRedirected);
	(void)view.Find(SAssetId { box, MakeSubId("mesh:Torso") }, &bRedirected);
	EXPECT_FALSE(bRedirected) << "the id it keeps is its own";

	const std::optional<SRecordId> record = view.Find(SAssetId { box, 0u });
	ASSERT_TRUE(record.has_value());
	const std::optional<SRecordId> skin = view.Find(SAssetId { MakeGuid(GuidA), 0u });
	ASSERT_TRUE(skin.has_value());
	bool bRemap = false;
	view.ForEachDependency(*record, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge)
	{
		if (Edge.FieldPath == "SubAssets.Remap.material:Skin")
		{
			bRemap = true;
			EXPECT_EQ(Edge.TargetId, (SAssetId { MakeGuid(GuidA), 0u }));
			EXPECT_EQ(Edge.FromSub, MakeSubId("material:Skin"));
			EXPECT_TRUE(HasAllFlags(Edge.Flags, EDepFlags::Hard | EDepFlags::Declared));
			EXPECT_FALSE(HasAnyFlags(Edge.Flags, EDepFlags::Schemaless | EDepFlags::Unresolved));
			EXPECT_EQ(Edge.To, *skin);
		}
	});
	EXPECT_TRUE(bRemap) << DumpOf(view).CStr();

	bool bStaleOverride = false;
	view.ForEachProblem([&] (FStringView, EProblemSource Source, const SDiagnostic& Diagnostic)
	{
		if (Diagnostic.Message.View().Contains(FStringView("image:#9")))
		{
			bStaleOverride = true;
			EXPECT_EQ(Source, EProblemSource::Analyze);
			EXPECT_EQ(Diagnostic.Severity, ESeverity::Warning);
			EXPECT_EQ(Diagnostic.File, "Content/Models/Box.gltf.meta");
			EXPECT_EQ(Diagnostic.Line, 9u);
		}
	});
	EXPECT_TRUE(bStaleOverride);

	// SubAssets is in the AnalyzeStamp: an edit of it analyzes the model again, and the rename is undone.
	const uint64 analyzed = registry->GetCounters().Analyzed;
	tree.WriteMeta("Models/Box.gltf", MetaText(GuidB));
	ASSERT_TRUE(registry->ScanPathsSynchronous(std::vector<FString> { "/Project/Models" }, 30.0));
	EXPECT_EQ(registry->GetCounters().Analyzed, analyzed + 1u);
	EXPECT_TRUE(FindById(registry->Live(), SAssetId { box, MakeSubId("mesh:Body") }).has_value());
	registry->ShutDown();
}

/**
 * The store with ids from metas: a record whose GUID changes leaves no trace of the old one in either id index (main and
 * sub-entries); two records with one GUID answer every lookup by id - Find, a sub-entry's id, an edge by id - with the
 * one whose file sorts first under the portable fold, never an assert and never an ambiguous edge, and the other once
 * it goes; authored labels are kept beside rule labels; a meta row has no record and is no companion.
 */
TEST(RegistryMeta, TheStoreAnswersForTheSmallestPathOfAGuidAndForgetsAReplacedOne)
{
	using namespace frt::assets::registry_test;
	const SGuid guid = MakeGuid(GuidA);
	const auto texture = [&] (SRegistryDelta& Delta, FStringView Path, const SGuid& Guid)
	{
		Delta.FileUpserts.push_back(MakeFile(Path));
		SRecordUpsert record = MakeRecord(Path);
		record.Guid = Guid;
		record.SubEntries.push_back(SSubEntryDecl { "image:Шар", MakeSubId("image:Шар"), asset_types::Texture, ERecordFlags::None, {} });
		record.AuthoredLabels = { "Rock" };
		record.RuleLabels = { "Environment" };
		Delta.RecordUpserts.push_back(std::move(record));
	};

	CRegistryStore store;
	SRegistryDelta first = MakeMountsDelta();
	texture(first, "/Project/T/Камінь.png", MakeGuid(GuidC));
	store.Commit(std::move(first));
	SRegistryDelta second;
	texture(second, "/Project/T/Камінь.png", guid);
	store.Commit(std::move(second));
	EXPECT_FALSE(store.Live().Find(SAssetId { MakeGuid(GuidC), 0u }).has_value());
	EXPECT_FALSE(store.Live().Find(SAssetId { MakeGuid(GuidC), MakeSubId("image:Шар") }).has_value());
	EXPECT_EQ(PathOf(store.Live(), GuidA), "/Project/T/Камінь.png");

	SRegistryDelta copy;
	texture(copy, "/Project/T/Zebra.png", guid);
	texture(copy, "/Project/T/b.png", guid);
	copy.FileUpserts.push_back(MakeFile("/Project/T/Камінь.png.meta", 60u, EFileClass::Meta));
	copy.FileUpserts.push_back(MakeFile("/Project/M/m.frtmat.yml"));
	SRecordUpsert material = MakeRecord("/Project/M/m.frtmat.yml", asset_types::Material, "Material.Legacy");
	SEdgeDecl edge;
	edge.Target = SAssetId { guid, 0u };
	edge.Flags = EDepFlags::Schemaless;
	material.Edges.push_back(edge);
	material.Companions.push_back("/Project/T/Камінь.png.meta");
	copy.RecordUpserts.push_back(std::move(material));
	store.Commit(std::move(copy));
	const CRegistryView& view = store.Live();
	EXPECT_EQ(PathOf(view, GuidA), "/Project/T/b.png") << "b before Zebra before Камінь under the fold";
	const std::optional<SAssetData> sub = FindById(view, SAssetId { guid, MakeSubId("image:Шар") });
	ASSERT_TRUE(sub.has_value());
	EXPECT_EQ(sub->MountPath, "/Project/T/b.png#image:Шар");
	const std::vector<SDependencyView> edges = GetDependencies(view, FindRecord(view, "/Project/M/m.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_FALSE(edges[0].bAmbiguous);
	EXPECT_EQ(edges[0].To, FindRecord(view, "/Project/T/b.png"));
	EXPECT_TRUE(GetDiagnostics(view, FindRecord(view, "/Project/M/m.frtmat.yml"), Path001).empty());
	std::vector<std::pair<FString, bool>> labels;
	view.ForEachLabel(FindRecord(view, "/Project/T/b.png"), [&] (FStringView Label, bool bRule) { labels.emplace_back(FString(Label), bRule); });
	EXPECT_EQ(labels, (std::vector<std::pair<FString, bool>> { { "Rock", false }, { "Environment", true } }));
	const std::optional<SStoredFile> meta = FindStoredFile(view, "/Project/T/Камінь.png.meta");
	ASSERT_TRUE(meta.has_value());
	EXPECT_EQ(meta->Class, EFileClass::Meta) << "a meta is never a companion";
	EXPECT_EQ(view.GetFileCount(), 4u);
	EXPECT_EQ(view.GetMetaFileCount(), 1u);

	SRegistryDelta gone;
	gone.FileRemoves = { "/Project/T/b.png", "/Project/T/Камінь.png.meta" };
	store.Commit(std::move(gone));
	EXPECT_EQ(PathOf(store.Live(), GuidA), "/Project/T/Zebra.png");
	EXPECT_EQ(GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"))[0].To, FindRecord(store.Live(), "/Project/T/Zebra.png"));
	EXPECT_EQ(store.Live().GetMetaFileCount(), 0u);
}


// ---------------- E4b: what a meta gives beside its GUID, and what a removal leaves ----------------

namespace
{
FString RedirectingMeta ()
{
	return MetaText(GuidA, FString::Format("Labels:\n  - Rock\nAliases:\n  - {}\nFormerPaths:\n  - /Project/Textures/Старий.png\n", GuidB));
}
}

TEST(RegistryMeta, AMetasAliasesAndFormerPathsResolveAndItsBytesAreKeptInMetas)
{
	CMetaTree tree("Перенаправлення");
	tree.WritePng("Textures/Камінь.png");
	tree.WriteMeta("Textures/Камінь.png", RedirectingMeta());
	tree.WritePng("Textures/Трава.png");
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	const CRegistryView& view = registry->Live();

	const SRecordId rock = view.FindByPath("/Project/Textures/Камінь.png").value_or(SRecordId {});
	ASSERT_TRUE(rock.IsValid());
	bool bRedirected = false;
	EXPECT_EQ(view.Find(SAssetId { MakeGuid(GuidB), 0u }, &bRedirected), std::optional<SRecordId>(rock));
	EXPECT_TRUE(bRedirected);
	SDiagnostic notice;
	EXPECT_EQ(view.FindByPath("/Project/Textures/старий.png", nullptr, &notice), std::optional<SRecordId>(rock));
	EXPECT_EQ(notice.Code, Ref006);

	const std::optional<SMetaCopyView> copy = view.FindMetaCopy(MakeGuid(GuidA));
	ASSERT_TRUE(copy.has_value());
	EXPECT_EQ(FStringView(reinterpret_cast<const char*>(copy->Bytes.data()), static_cast<uint32>(copy->Bytes.size())), RedirectingMeta().View());
	EXPECT_EQ(copy->Hash, HashMetaBytes(RedirectingMeta()));
	EXPECT_EQ(copy->SourcePath, "/Project/Textures/Камінь.png");
	EXPECT_EQ(view.GetMetaCopyCount(), 1u) << "a file without a meta has no copy";

	const FString dump = DumpOf(view);
	EXPECT_TRUE(dump.View().Contains(FString::Format("    alias {{{}}}\n    formerpath /Project/Textures/Старий.png\n", GuidB).View())) << dump.CStr();
	std::vector<SRecordId> pending;
	EXPECT_EQ(view.RunText("is:pending-meta", pending).value_or(0u), 1u);
	EXPECT_EQ(pending, std::vector<SRecordId> { view.FindByPath("/Project/Textures/Трава.png").value_or(SRecordId {}) }) << "only Трава.png has no meta";
	registry->ShutDown();
}

TEST(RegistryMeta, ARemovedFileLeavesATombstoneAndARestoredOneTakesItsIdentityBack)
{
	CMetaTree tree("Надгробок");
	tree.WritePng("Textures/Камінь.png");
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidA));
	tree.WritePng("Textures/Трава.png");
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	const std::vector<FString> folder = { "/Project/Textures" };
	ASSERT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value());
	SFileData stat;
	ASSERT_TRUE(registry->Live().GetFile(*registry->Live().FindFileExact("/Project/Textures/Камінь.png"), stat));

	// The source goes, its meta stays (an orphan): a tombstone of what was there, the copy dropped with the record.
	tree.Remove("Textures/Камінь.png");
	tree.Remove("Textures/Трава.png");
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	const CRegistryView& gone = registry->Live();
	EXPECT_TRUE(PathOf(gone, GuidA).IsEmpty());
	EXPECT_EQ(gone.GetTombstoneCount(), 1u) << "a provisional id leaves none";
	const std::optional<STombstoneView> tombstone = gone.FindTombstone(MakeGuid(GuidA));
	ASSERT_TRUE(tombstone.has_value());
	EXPECT_EQ(tombstone->LastPath, "/Project/Textures/Камінь.png");
	EXPECT_EQ(tombstone->Identity, stat.Identity);
	EXPECT_GT(tombstone->RemovedFileTime, stat.MTime) << "now, as a file time";
	EXPECT_FALSE(gone.FindMetaCopy(MakeGuid(GuidA)).has_value());
	EXPECT_EQ(FindStoredFile(gone, "/Project/Textures/Камінь.png.meta")->MetaState, EMetaState::Orphan);

	// It comes back beside its meta: the same identity, no tombstone, the copy again.
	tree.WritePng("Textures/Камінь.png");
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidA), "/Project/Textures/Камінь.png");
	EXPECT_EQ(registry->Live().GetTombstoneCount(), 0u);
	EXPECT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value());

	// A GUID edited by hand: the old one is tombstoned (§4.6), the new one registered with its copy.
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidC));
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidC), "/Project/Textures/Камінь.png");
	ASSERT_TRUE(registry->Live().FindTombstone(MakeGuid(GuidA)).has_value());
	EXPECT_EQ(registry->Live().FindTombstone(MakeGuid(GuidA))->LastPath, "/Project/Textures/Камінь.png");
	EXPECT_FALSE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value());
	EXPECT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidC)).has_value());
	registry->ShutDown();
}

TEST(RegistryMeta, ARemovedCopyOfAGuidAnotherFileHoldsLeavesNoTombstoneAndKeepsTheCopy)
{
	CMetaTree tree("Двійник");
	tree.WritePng("Textures/Камінь.png");
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidA));
	tree.WritePng("Textures/Копія.png");
	tree.WriteMeta("Textures/Копія.png", MetaText(GuidA));
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	tree.Remove("Textures/Копія.png");
	tree.Remove("Textures/Копія.png.meta");
	ASSERT_TRUE(registry->ScanPathsSynchronous(std::vector<FString> { "/Project/Textures" }, 30.0));
	EXPECT_EQ(PathOf(registry->Live(), GuidA), "/Project/Textures/Камінь.png");
	EXPECT_EQ(registry->Live().GetTombstoneCount(), 0u) << "the GUID is not gone";
	EXPECT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value());

	// An importer that goes away from the other copy (a folder's rule) leaves the copy to the record that kept it.
	tree.WritePng("Textures/Excluded/Копія.png");
	tree.WriteMeta("Textures/Excluded/Копія.png", MetaText(GuidA));
	ASSERT_TRUE(registry->ScanPathsSynchronous(std::vector<FString> { "/Project/Textures" }, 30.0));
	EXPECT_TRUE(registry->Live().FindByPath("/Project/Textures/Excluded/Копія.png").has_value());
	tree.WriteProjectContent("Textures/Excluded/Assets.yml", "FormatVersion: 1\nImporters: { Exclude: [ Texture ] }\n");
	ASSERT_TRUE(registry->ScanPathsSynchronous(std::vector<FString> { "/Project/Textures" }, 30.0));
	EXPECT_FALSE(registry->Live().FindByPath("/Project/Textures/Excluded/Копія.png").has_value());
	EXPECT_EQ(PathOf(registry->Live(), GuidA), "/Project/Textures/Камінь.png");
	EXPECT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value()) << "/Project/Textures/Камінь.png still holds the GUID";
	EXPECT_EQ(registry->Live().GetTombstoneCount(), 0u);
	registry->ShutDown();
}

// Both copies of one GUID removed in one pass (as files, or with their folder): each one's view still holds the other,
// yet the GUID is gone, so it leaves one tombstone and no copy.
TEST(RegistryMeta, BothCopiesOfAGuidRemovedInOnePassLeaveOneTombstoneAndNoCopy)
{
	CMetaTree tree("Двійники");
	for (const FStringView name : { FStringView("Textures/Камінь.png"), FStringView("Textures/Копія.png"), FStringView("Textures/Пара/Камінь.png"),
		FStringView("Textures/Пара/Копія.png") })
	{
		tree.WritePng(name);
	}
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidA));
	tree.WriteMeta("Textures/Копія.png", MetaText(GuidA));
	tree.WriteMeta("Textures/Пара/Камінь.png", MetaText(GuidB));
	tree.WriteMeta("Textures/Пара/Копія.png", MetaText(GuidB));
	jobs::CWorkerPool pool(MakePoolDesc());
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts());
	ASSERT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value());
	ASSERT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidB)).has_value());

	for (const FStringView name : { FStringView("Textures/Камінь.png"), FStringView("Textures/Копія.png") })
	{
		tree.Remove(name);
	}
	std::error_code ec;
	std::filesystem::remove_all(tree.Tree.ProjectContent() / FStringView("Textures/Пара").ToPath(), ec);
	EXPECT_FALSE(ec) << ec.message();
	ASSERT_TRUE(registry->ScanPathsSynchronous(std::vector<FString> { "/Project/Textures" }, 30.0));
	const CRegistryView& view = registry->Live();
	EXPECT_TRUE(PathOf(view, GuidA).IsEmpty());
	EXPECT_TRUE(PathOf(view, GuidB).IsEmpty());
	EXPECT_EQ(view.GetTombstoneCount(), 2u);
	const std::optional<STombstoneView> files = view.FindTombstone(MakeGuid(GuidA));
	EXPECT_TRUE(files.has_value()) << "two files removed";
	EXPECT_TRUE(files.has_value() && files->LastPath.StartsWith(FStringView("/Project/Textures/К")));
	const std::optional<STombstoneView> folder = view.FindTombstone(MakeGuid(GuidB));
	EXPECT_TRUE(folder.has_value()) << "their folder removed";
	EXPECT_TRUE(folder.has_value() && folder->LastPath.StartsWith(FStringView("/Project/Textures/Пара/")));
	EXPECT_EQ(view.GetMetaCopyCount(), 0u);
	registry->ShutDown();
}

TEST(RegistryMeta, AReEvaluationTakesAnUnchangedMetaFromItsCopyAndAWarmStartKeepsTheIdentityState)
{
	CMetaTree tree("Копія");
	tree.WritePng("Textures/Камінь.png");
	tree.WriteMeta("Textures/Камінь.png", RedirectingMeta());
	jobs::CWorkerPool pool(MakePoolDesc());
	FString cold;
	{
		std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts(), true);
		cold = DumpOf(registry->Live());
		registry->ShutDown();
	}

	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Mounts(), true);
	EXPECT_EQ(DumpOf(registry->Live()), cold);
	EXPECT_EQ(registry->GetCounters().MetasRead, 0u);
	ASSERT_TRUE(registry->Live().FindMetaCopy(MakeGuid(GuidA)).has_value()) << "METAS comes back with the snapshot";
	bool bRedirected = false;
	EXPECT_TRUE(registry->Live().Find(SAssetId { MakeGuid(GuidB), 0u }, &bRedirected).has_value() && bRedirected);

	// A rule edit re-evaluates the folder in memory: the meta's values come from its copy, never the disk.
	tree.WriteProjectContent("Textures/Assets.yml", "FormatVersion: 1\nRules:\n  - { Match: \"**/*.png\", Labels: [ Stone ] }\n");
	const std::vector<FString> folder = { "/Project/Textures" };
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_GE(registry->GetCounters().ReEvaluated, 1u);
	EXPECT_EQ(registry->GetCounters().MetasRead, 0u);
	EXPECT_EQ(registry->GetCounters().Analyzed, 0u);
	const FString edited = DumpOf(registry->Live());
	EXPECT_TRUE(edited.View().Contains(FStringView("    labels authored:Rock, rule:Stone\n"))) << edited.CStr();
	EXPECT_TRUE(edited.View().Contains(FStringView("    formerpath /Project/Textures/Старий.png\n"))) << edited.CStr();

	// A meta edit that drops an alias: re-evaluated from the file, and the alias resolves no more.
	tree.WriteMeta("Textures/Камінь.png", MetaText(GuidA, "Labels:\n  - Rock\n"));
	ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 30.0));
	EXPECT_EQ(registry->GetCounters().MetasRead, 1u);
	EXPECT_FALSE(registry->Live().Find(SAssetId { MakeGuid(GuidB), 0u }).has_value());
	EXPECT_FALSE(registry->Live().FindByPath("/Project/Textures/Старий.png").has_value());
	EXPECT_EQ(registry->Live().FindMetaCopy(MakeGuid(GuidA))->Hash, HashMetaBytes(MetaText(GuidA, "Labels:\n  - Rock\n")));
	registry->ShutDown();
}
