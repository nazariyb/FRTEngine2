// Identity in the registry store, part 2 (Assets/Registry/RegistryDelta.h, design §5.2, §5.3; M4's E4b). The alias
// index answers Find and id edges for a consolidated GUID and a renamed sub-asset's new label, after the canonical
// index and never before it; former paths answer FindByPath and path edges until a file takes the path over; an edge
// resolved through either is flagged (is:redirected) and reported as REF006 on its record, in any commit order. Meta
// copies and tombstones are kept one per GUID, a copy that is not what its hash says is refused, and registering a GUID
// drops its tombstone. All of it survives an image adopted in bulk or replayed, a snapshot (layout 2, METAS as one zstd
// frame; a layout 1 file is not used) and the journal; tombstones older than 30 days are pruned at write.
#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "../Snapshot/SnapshotTestUtils.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetTypes.h"
#include "Assets/Registry/AssetQuery.h"
#include "Assets/Registry/QueryEngine.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::snapshot_test;


namespace
{
SGuid Guid (FStringView Label)
{
	return MakeTestGuid(Label);
}

// A texture with a meta-given GUID.
SRecordUpsert MakeTexture (SRegistryDelta& Delta, FStringView Path, FStringView GuidLabel)
{
	Delta.FileUpserts.push_back(MakeFile(Path));
	Delta.FileUpserts.back().Meta = SFileMetaState { EMetaState::Paired, 1u };
	SRecordUpsert record = MakeRecord(Path);
	record.Guid = Guid(GuidLabel);
	return record;
}

SEdgeDecl MakeIdEdge (const SAssetId& Target)
{
	SEdgeDecl edge;
	edge.Target = Target;
	edge.Flags = EDepFlags::Declared;
	return edge;
}

// The query's matches as mount paths.
std::vector<FString> Query (const CRegistryView& View, FStringView Text)
{
	std::vector<SRecordId> records;
	EXPECT_TRUE(View.RunText(Text, records).has_value()) << FString(Text).CStr();
	std::vector<FString> paths;
	for (const SRecordId record : records)
	{
		SAssetData data;
		View.GetRecord(record, data);
		paths.push_back(data.MountPath);
	}
	return paths;
}

uint32 CountRef006 (const CRegistryView& View, FStringView Path)
{
	return static_cast<uint32>(GetDiagnostics(View, FindRecord(View, Path), Ref006).size());
}
}


// ---------------- The alias index ----------------

TEST(RegistryIdentityTest, AConsolidatedGuidResolvesFindAndIdEdgesUntilARecordHoldsItItself)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	SRecordUpsert rock = MakeTexture(delta, "/Project/T/Rock.png", "rock");
	rock.Aliases = { SAssetId { Guid("pebble"), 0u } };
	delta.RecordUpserts.push_back(std::move(rock));
	delta.FileUpserts.push_back(MakeFile("/Project/M/m.frtmat.yml"));
	SRecordUpsert material = MakeRecord("/Project/M/m.frtmat.yml", asset_types::Material, "Material.Legacy");
	material.Edges = { MakeIdEdge(SAssetId { Guid("pebble"), 0u }) };
	delta.RecordUpserts.push_back(std::move(material));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const SRecordId rockRecord = FindRecord(view, "/Project/T/Rock.png");
	bool bRedirected = false;
	EXPECT_EQ(view.Find(SAssetId { Guid("pebble"), 0u }, &bRedirected), std::optional<SRecordId>(rockRecord));
	EXPECT_TRUE(bRedirected);
	EXPECT_EQ(view.Find(SAssetId { Guid("rock"), 0u }, &bRedirected), std::optional<SRecordId>(rockRecord));
	EXPECT_FALSE(bRedirected);
	std::vector<SDependencyView> edges = GetDependencies(view, FindRecord(view, "/Project/M/m.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_EQ(edges[0].To, rockRecord);
	EXPECT_TRUE(edges[0].bRedirected);
	EXPECT_EQ(edges[0].TargetId, (SAssetId { Guid("pebble"), 0u })) << "a Guid edge keeps its id as declared";
	EXPECT_EQ(CountRef006(view, "/Project/M/m.frtmat.yml"), 1u);
	EXPECT_EQ(Query(view, "is:redirected"), std::vector<FString> { "/Project/M/m.frtmat.yml" });
	EXPECT_EQ(GetReferencers(view, rockRecord).size(), 1u);

	// A record that holds the GUID itself wins: the canonical index first.
	SRegistryDelta pebble;
	pebble.RecordUpserts.push_back(MakeTexture(pebble, "/Project/T/Pebble.png", "pebble"));
	store.Commit(std::move(pebble));
	edges = GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"));
	EXPECT_EQ(edges[0].To, FindRecord(store.Live(), "/Project/T/Pebble.png"));
	EXPECT_FALSE(edges[0].bRedirected);
	EXPECT_EQ(CountRef006(store.Live(), "/Project/M/m.frtmat.yml"), 0u);
	EXPECT_TRUE(Query(store.Live(), "is:redirected").empty());

	// Gone again, and the alias dropped from the meta: nothing resolves the id.
	SRegistryDelta drop;
	drop.FileRemoves = { "/Project/T/Pebble.png" };
	SRecordUpsert again = MakeTexture(drop, "/Project/T/Rock.png", "rock");
	drop.RecordUpserts.push_back(std::move(again));
	store.Commit(std::move(drop));
	edges = GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"));
	EXPECT_TRUE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	EXPECT_FALSE(store.Live().Find(SAssetId { Guid("pebble"), 0u }).has_value());
}

TEST(RegistryIdentityTest, ARenamedSubAssetsNewLabelResolvesToTheSubEntryThatKeptTheOldId)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.FileUpserts.push_back(MakeFile("/Project/Box.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Box.gltf", asset_types::Model, "Model.Gltf");
	model.Guid = Guid("box");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Torso", MakeSubId("mesh:Body"), asset_types::Mesh, ERecordFlags::None, {} });
	AppendRenamedSubAliases(model.Guid, model.SubEntries, model.Aliases);
	ASSERT_EQ(model.Aliases, (std::vector<SAssetId> { SAssetId { Guid("box"), MakeSubId("mesh:Torso") } }));
	delta.RecordUpserts.push_back(std::move(model));
	delta.FileUpserts.push_back(MakeFile("/Project/M/m.frtmat.yml"));
	SRecordUpsert material = MakeRecord("/Project/M/m.frtmat.yml", asset_types::Material, "Material.Legacy");
	material.Edges = { MakeIdEdge(SAssetId { Guid("box"), MakeSubId("mesh:Torso") }), MakeIdEdge(SAssetId { Guid("box"), MakeSubId("mesh:Body") }),
		MakePathEdge("/Project/Box.gltf#mesh:Torso") };
	delta.RecordUpserts.push_back(std::move(material));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const SRecordId torso = FindRecord(view, "/Project/Box.gltf#mesh:Torso");
	ASSERT_TRUE(torso.IsValid());
	bool bRedirected = false;
	EXPECT_EQ(view.Find(SAssetId { Guid("box"), MakeSubId("mesh:Torso") }, &bRedirected), std::optional<SRecordId>(torso));
	EXPECT_TRUE(bRedirected);
	EXPECT_EQ(view.Find(SAssetId { Guid("box"), MakeSubId("mesh:Body") }, &bRedirected), std::optional<SRecordId>(torso));
	EXPECT_FALSE(bRedirected) << "the id it kept is its own";
	const std::vector<SDependencyView> edges = GetDependencies(view, FindRecord(view, "/Project/M/m.frtmat.yml"));
	ASSERT_EQ(edges.size(), 3u);
	uint32 redirected = 0u;
	for (const SDependencyView& edge : edges)
	{
		EXPECT_EQ(edge.To, torso) << FString(edge.TargetPath).CStr();
		EXPECT_EQ(edge.TargetSub, MakeSubId("mesh:Body"));
		redirected += edge.bRedirected ? 1u : 0u;
	}
	EXPECT_EQ(redirected, 1u) << "a path names the label it has, and the kept id is the sub-entry's own";
	EXPECT_EQ(GetReferencers(view, torso).size(), 3u);
	EXPECT_EQ(CountRef006(view, "/Project/M/m.frtmat.yml"), 1u);

	// A dump pins the alias line and the redirected edge.
	const FString dump = Dump(view);
	EXPECT_TRUE(dump.View().Contains(FString::Format("    alias {{{}}}#{:016x}\n", Guid("box").ToString(), MakeSubId("mesh:Torso")).View())) << dump.CStr();
	EXPECT_TRUE(dump.View().Contains(FStringView(" flags=Declared Resolved redirected\n"))) << dump.CStr();
}

// ---------------- Former paths ----------------

TEST(RegistryIdentityTest, AFormerPathResolvesPathEdgesAndFindByPathUntilAFileTakesThePathOver)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Project/T/old.png", "/Project/T/Old.png#mip:0" });
	SRecordUpsert moved = MakeTexture(delta, "/Project/T/Moved.png", "moved");
	moved.FormerPaths = { "/Project/T/Old.png", "/Project/T/OLD.png", "/Project/T/Moved.png" };
	delta.RecordUpserts.push_back(std::move(moved));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const SRecordId record = FindRecord(view, "/Project/T/Moved.png");
	std::vector<FString> formerPaths;
	view.ForEachFormerPath(record, [&] (FStringView Path) { formerPaths.emplace_back(Path); });
	EXPECT_EQ(formerPaths, std::vector<FString> { "/Project/T/Old.png" }) << "one per path under the fold, never its own";
	SDiagnostic notice;
	EXPECT_EQ(view.FindByPath("/Project/t/old.png", nullptr, &notice), std::optional<SRecordId>(record));
	EXPECT_EQ(notice.Code, Ref006);
	EXPECT_EQ(notice.Severity, ESeverity::Info);
	EXPECT_EQ(notice.Subject, "/Project/T/Moved.png");
	EXPECT_EQ(view.FindByPath("/Project/T/Moved.png", nullptr, &notice), std::optional<SRecordId>(record));
	EXPECT_EQ(notice.Code, 0u);
	EXPECT_EQ(view.FindByPath("/Project/T/moved.png", nullptr, &notice), std::optional<SRecordId>(record));
	EXPECT_EQ(notice.Code, Ref006) << "another spelling of a current path";

	const std::vector<SDependencyView> edges = GetDependencies(view, FindRecord(view, "/Project/M/m.frtmat.yml"));
	ASSERT_EQ(edges.size(), 2u);
	for (const SDependencyView& edge : edges)
	{
		EXPECT_EQ(edge.To, record) << FString(edge.TargetPath).CStr();
		EXPECT_TRUE(edge.bRedirected);
	}
	const std::vector<SDiagnostic> notices = GetDiagnostics(view, FindRecord(view, "/Project/M/m.frtmat.yml"), Ref006);
	ASSERT_EQ(notices.size(), 2u);
	EXPECT_TRUE(notices[0].Message.View().Contains(FStringView("FormerPaths"))) << notices[0].Message.CStr();
	EXPECT_EQ(Query(view, "is:redirected"), std::vector<FString> { "/Project/M/m.frtmat.yml" });

	// A file now at the old path takes the references over; when it goes, the former path answers again.
	SRegistryDelta takeOver;
	AddTexture(takeOver, "/Project/T/Old.png");
	store.Commit(std::move(takeOver));
	EXPECT_EQ(GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"))[0].To, FindRecord(store.Live(), "/Project/T/Old.png"));
	EXPECT_EQ(CountRef006(store.Live(), "/Project/M/m.frtmat.yml"), 1u) << "only the spelling differs now";
	EXPECT_TRUE(Query(store.Live(), "is:redirected").empty());
	SRegistryDelta remove;
	remove.FileRemoves = { "/Project/T/Old.png" };
	store.Commit(std::move(remove));
	EXPECT_EQ(GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"))[0].To, FindRecord(store.Live(), "/Project/T/Moved.png"));

	// The meta forgets the path: the references are unresolved.
	SRegistryDelta forget;
	forget.RecordUpserts.push_back(MakeTexture(forget, "/Project/T/Moved.png", "moved"));
	store.Commit(std::move(forget));
	EXPECT_TRUE(HasAnyFlags(GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"))[0].Flags, EDepFlags::Unresolved));
	EXPECT_FALSE(store.Live().FindByPath("/Project/T/Old.png").has_value());
}

TEST(RegistryIdentityTest, RedirectsResolveAlikeInAnyCommitOrder)
{
	std::vector<SRegistryDelta> content;
	{
		SRegistryDelta& moved = content.emplace_back();
		SRecordUpsert record = MakeTexture(moved, "/Project/T/Moved.png", "moved");
		record.Aliases = { SAssetId { Guid("absorbed"), 0u } };
		record.FormerPaths = { "/Project/T/Old.png" };
		moved.RecordUpserts.push_back(std::move(record));
	}
	{
		SRegistryDelta& material = content.emplace_back();
		AddMaterial(material, "/Project/M/m.frtmat.yml", { "/Project/T/OLD.png" });
		material.RecordUpserts.back().Edges.push_back(MakeIdEdge(SAssetId { Guid("absorbed"), 0u }));
	}
	content.emplace_back().FolderUpserts.push_back(SFolderUpsert { "/Project/Empty" });
	{
		SRegistryDelta& other = content.emplace_back();
		AddTexture(other, "/Project/T/Other.png");
	}
	const auto dumpOf = [&] (std::span<const uint32> Order)
	{
		CRegistryStore store;
		store.Commit(MakeMountsDelta());
		for (const uint32 index : Order)
		{
			store.Commit(SRegistryDelta(content[index]));
		}
		return Dump(store.Live());
	};
	std::vector<uint32> order(content.size());
	for (uint32 i = 0u; i < order.size(); ++i)
	{
		order[i] = i;
	}
	const FString reference = dumpOf(order);
	EXPECT_TRUE(reference.View().Contains(FStringView("Resolved redirected"))) << reference.CStr();
	EXPECT_TRUE(reference.View().Contains(FStringView("    formerpath /Project/T/Old.png\n"))) << reference.CStr();
	std::mt19937 random(4321u);
	for (uint32 attempt = 0u; attempt < 6u; ++attempt)
	{
		std::shuffle(order.begin(), order.end(), random);
		EXPECT_EQ(dumpOf(order), reference);
	}
}

// ---------------- Meta copies and tombstones ----------------

TEST(RegistryIdentityTest, MetaCopiesAndTombstonesAreKeptOnePerGuidAndARegisteredGuidDropsItsTombstone)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.MetaCopies.push_back(MakeMetaCopy(Guid("rock"), "/Project/T/Rock.png"));
	SMetaCopy forged = MakeMetaCopy(Guid("forged"), "/Project/T/Forged.png");
	forged.Hash ^= 1u;
	delta.MetaCopies.push_back(forged);
	delta.Tombstones.push_back(STombstone { Guid("rock"), "/Project/T/Rock.png", hash::HashContent(FStringView("a").AsBytes()), {}, 100 });
	delta.Tombstones.push_back(STombstone { Guid("gone"), "/Project/T/Gone.png", {}, {}, 200 });
	store.Commit(std::move(delta));
	const CRegistryView& view = store.Live();
	EXPECT_EQ(view.GetMetaCopyCount(), 1u);
	ASSERT_TRUE(view.FindMetaCopy(Guid("rock")).has_value());
	EXPECT_EQ(view.FindMetaCopy(Guid("rock"))->SourcePath, "/Project/T/Rock.png");
	EXPECT_FALSE(view.FindMetaCopy(Guid("forged")).has_value()) << "a copy is verified against its hash";
	EXPECT_EQ(view.GetTombstoneCount(), 2u);

	// A later copy or tombstone of a GUID replaces it.
	SRegistryDelta later;
	SMetaCopy moved = MakeMetaCopy(Guid("rock"), "/Project/Moved/Rock.png");
	later.MetaCopies.push_back(moved);
	later.Tombstones.push_back(STombstone { Guid("gone"), "/Project/T/Gone again.png", {}, {}, 300 });
	store.Commit(std::move(later));
	EXPECT_EQ(store.Live().GetMetaCopyCount(), 1u);
	EXPECT_EQ(store.Live().FindMetaCopy(Guid("rock"))->SourcePath, "/Project/Moved/Rock.png");
	EXPECT_EQ(store.Live().GetTombstoneCount(), 2u);
	EXPECT_EQ(store.Live().FindTombstone(Guid("gone"))->LastPath, "/Project/T/Gone again.png");
	EXPECT_EQ(store.Live().FindTombstone(Guid("gone"))->RemovedFileTime, 300);

	// A record of the GUID: its tombstone goes; a removal and an add of one GUID in one delta (a move) leaves none.
	SRegistryDelta back;
	back.RecordUpserts.push_back(MakeTexture(back, "/Project/T/Rock.png", "rock"));
	store.Commit(std::move(back));
	EXPECT_FALSE(store.Live().FindTombstone(Guid("rock")).has_value());
	SRegistryDelta move;
	move.FileRemoves = { "/Project/T/Rock.png" };
	move.Tombstones.push_back(STombstone { Guid("rock"), "/Project/T/Rock.png", {}, {}, 400 });
	move.RecordUpserts.push_back(MakeTexture(move, "/Project/U/Rock.png", "rock"));
	store.Commit(std::move(move));
	EXPECT_FALSE(store.Live().FindTombstone(Guid("rock")).has_value());

	// The copy of a GUID a live record holds is not removed; once no record holds it, it is.
	SRegistryDelta removes;
	removes.MetaCopyRemoves = { Guid("rock"), Guid("never") };
	removes.TombstoneRemoves = { Guid("gone") };
	store.Commit(std::move(removes));
	EXPECT_EQ(store.Live().GetMetaCopyCount(), 1u) << "/Project/U/Rock.png holds the GUID";
	EXPECT_EQ(store.Live().GetTombstoneCount(), 0u);
	EXPECT_TRUE(Dump(store.Live()).View().Contains(FStringView("/Project/U/Rock.png"))) << "tombstones and copies never reach a dump";
	EXPECT_FALSE(Dump(store.Live()).View().Contains(FStringView("Gone"))) << Dump(store.Live()).CStr();
	SRegistryDelta gone;
	gone.FileRemoves = { "/Project/U/Rock.png" };
	gone.MetaCopyRemoves = { Guid("rock") };
	store.Commit(std::move(gone));
	EXPECT_EQ(store.Live().GetMetaCopyCount(), 0u);
}

// An asset moved with its meta from /Project to a plugin while the editor was closed: the two mounts' passes commit in
// either order (each from the view it began with), and a journal replays another file's part of the removal (its
// GUID-only removes) last. Every order gives one dump, no tombstone and the plugin's copy, as an adoption of it does.
TEST(RegistryIdentityTest, AMoveAcrossMountsKeepsOneIdentityStateInAnyCommitOrder)
{
	const auto base = []
	{
		SRegistryDelta delta = MakeMountsDelta();
		delta.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, true));
		delta.RecordUpserts.push_back(MakeTexture(delta, "/Project/T/Rock.png", "rock"));
		delta.MetaCopies.push_back(MakeMetaCopy(Guid("rock"), "/Project/T/Rock.png"));
		return delta;
	};
	std::vector<SRegistryDelta> content;
	{
		SRegistryDelta& added = content.emplace_back();
		added.RecordUpserts.push_back(MakeTexture(added, "/Plugin.Trees/Rock.png", "rock"));
		added.MetaCopies.push_back(MakeMetaCopy(Guid("rock"), "/Plugin.Trees/Rock.png"));
	}
	{
		SRegistryDelta& removed = content.emplace_back();
		removed.FileRemoves = { "/Project/T/Rock.png" };
		removed.Tombstones.push_back(STombstone { Guid("rock"), "/Project/T/Rock.png", {}, {}, 500 });
		removed.MetaCopyRemoves = { Guid("rock") };
	}
	{
		SRegistryDelta& part = content.emplace_back();   // the removal's part in the plugin's journal
		part.MetaCopyRemoves = { Guid("rock") };
	}
	std::vector<FString> dumps;
	for (const std::vector<uint32>& order : std::vector<std::vector<uint32>> { { 0u, 1u, 2u }, { 1u, 0u, 2u }, { 1u, 2u, 0u }, { 2u, 1u, 0u }, { 0u, 2u, 1u } })
	{
		CRegistryStore store;
		store.Commit(base());
		for (const uint32 index : order)
		{
			store.Commit(SRegistryDelta(content[index]));
		}
		const CRegistryView& view = store.Live();
		EXPECT_EQ(view.GetTombstoneCount(), 0u) << order[0] << order[1] << order[2];
		ASSERT_TRUE(view.FindMetaCopy(Guid("rock")).has_value()) << order[0] << order[1] << order[2];
		EXPECT_EQ(view.FindMetaCopy(Guid("rock"))->SourcePath, "/Plugin.Trees/Rock.png");
		dumps.push_back(Dump(view));
		EXPECT_EQ(dumps.back(), dumps.front());

		CRegistryStore adopted;
		adopted.AdoptImage(view.ExportImage({}), 9u);
		ExpectSameContent(view, adopted.Live());
	}
}

// ---------------- Images, snapshots, journals ----------------

TEST(RegistryIdentityTest, ABulkAdoptionAndAReplayOfAnImageGiveTheSameIdentity)
{
	CRegistryStore original;
	original.Commit(MakeContent());
	ASSERT_EQ(original.Live().GetMetaCopyCount(), 2u);
	ASSERT_EQ(original.Live().GetTombstoneCount(), 2u);

	CRegistryStore bulk;
	bulk.AdoptImage(original.Live().ExportImage({}), 9u);
	EXPECT_FALSE(bulk.GetLastCommitStats().bReplayedImage);
	CRegistryStore replayed;
	SRegistryDelta mounts = MakeMountsDelta();
	mounts.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, true));
	replayed.Commit(std::move(mounts));
	replayed.AdoptImage(original.Live().ExportImage({}), 9u);
	EXPECT_TRUE(replayed.GetLastCommitStats().bReplayedImage);
	ExpectSameContent(original.Live(), bulk.Live());
	ExpectSameContent(original.Live(), replayed.Live());
	EXPECT_EQ(Query(bulk.Live(), "is:redirected"), Query(original.Live(), "is:redirected"));
	EXPECT_EQ(Query(original.Live(), "is:redirected"), std::vector<FString> { "/Project/M/old.frtmat.yml" });

	// A tombstone of a GUID the image's records hold is dropped by both (the record's upsert clears it).
	SRegistryImage image = original.Live().ExportImage({});
	image.Tombstones.push_back(SImageTombstone { Guid("moved"), image.Tombstones[0].LastPath, {}, {}, 1 });
	CRegistryStore bulkWithLive;
	bulkWithLive.AdoptImage(SRegistryImage(image), 9u);
	EXPECT_FALSE(bulkWithLive.Live().FindTombstone(Guid("moved")).has_value());
	CRegistryStore replayedWithLive;
	SRegistryDelta withMounts = MakeMountsDelta();
	withMounts.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, true));
	replayedWithLive.Commit(std::move(withMounts));
	replayedWithLive.AdoptImage(std::move(image), 9u);
	EXPECT_TRUE(replayedWithLive.GetLastCommitStats().bReplayedImage);
	EXPECT_FALSE(replayedWithLive.Live().FindTombstone(Guid("moved")).has_value());
	EXPECT_EQ(replayedWithLive.Live().GetTombstoneCount(), bulkWithLive.Live().GetTombstoneCount());
}

TEST_F(SnapshotTest, ASnapshotOfLayoutOneIsNotUsedAndTombstonesOlderThanThirtyDaysArePruned)
{
	CRegistryStore original;
	SRegistryDelta content = MakeContent();
	const int64 now = GetFileTimeNow();
	content.Tombstones.push_back(STombstone { Guid("ancient"), "/Project/T/Ancient.png", {}, {}, now - TombstoneLifetimeFileTicks - 10000000ll });
	original.Commit(std::move(content));
	ASSERT_EQ(original.Live().GetTombstoneCount(), 3u);

	const std::filesystem::path path = Dir / "AssetRegistry.snapshot";
	const SGuid projectId = SGuid::NewRandom();
	SSnapshotWriteStats stats;
	ASSERT_TRUE(WriteSnapshot(original.Live(), {}, path, projectId, 0u, nullptr, &stats));
	EXPECT_EQ(stats.PrunedTombstones, 1u);
	const SSnapshotSource sources[] = { SSnapshotSource { path, projectId } };
	{
		CRegistryStore loaded;
		const SSnapshotLoadResult result = LoadSnapshots(loaded, sources);
		ASSERT_TRUE(result.AnyLoaded());
		EXPECT_TRUE(result.Files[0].DroppedSections.empty()) << result.Files[0].Message.CStr();
		EXPECT_EQ(loaded.Live().GetTombstoneCount(), 2u);
		EXPECT_FALSE(loaded.Live().FindTombstone(Guid("ancient")).has_value());
		EXPECT_TRUE(loaded.Live().FindTombstone(Guid("deleted")).has_value());
		EXPECT_EQ(loaded.Live().GetMetaCopyCount(), 2u);
		EXPECT_EQ(Dump(loaded.Live()), Dump(original.Live()));
	}

	// The same file with every section marked layout 1 (as M3 wrote it): rejected whole, a cold start.
	std::vector<uint8> bytes = ReadBytes(path);
	const uint64 count = binary::ReadLittleEndianAt(bytes, 12u, 4u);
	for (uint64 i = 0u; i < count; ++i)
	{
		binary::WriteLittleEndianAt(bytes, SnapshotHeaderBytes + static_cast<size_t>(i) * SnapshotSectionEntryBytes + 4u, 1u, 4u);
	}
	WriteBytes(path, bytes);
	CRegistryStore loaded;
	const SSnapshotLoadResult result = LoadSnapshots(loaded, sources);
	ASSERT_EQ(result.Files.size(), 1u);
	EXPECT_EQ(result.Files[0].State, ESnapshotFileState::Rejected);
	EXPECT_FALSE(result.AnyLoaded());
	EXPECT_EQ(loaded.Live().GetRecordCount(), 0u);
}

TEST_F(SnapshotTest, AMetaCopyThatDoesNotMatchItsHashIsDroppedAloneAtLoad)
{
	CRegistryStore original;
	original.Commit(MakeContent());
	SRegistryImage image = original.Live().ExportImage({});
	ASSERT_EQ(image.Metas.size(), 2u);
	image.Metas[0].Bytes.back() ^= 1u;   // the copy on disk is not what its hash says
	const std::filesystem::path path = Dir / "AssetRegistry.snapshot";
	const SGuid projectId = SGuid::NewRandom();
	WriteBytes(path, EncodeSnapshot(image, projectId, original.GetRevision(), 0u));
	CRegistryStore loaded;
	const SSnapshotSource sources[] = { SSnapshotSource { path, projectId } };
	const SSnapshotLoadResult result = LoadSnapshots(loaded, sources);
	ASSERT_TRUE(result.AnyLoaded());
	EXPECT_TRUE(result.Files[0].DroppedSections.empty()) << result.Files[0].Message.CStr();
	EXPECT_EQ(loaded.Live().GetMetaCopyCount(), 1u);
	EXPECT_FALSE(loaded.Live().FindMetaCopy(image.Metas[0].Guid).has_value());
	EXPECT_TRUE(loaded.Live().FindMetaCopy(image.Metas[1].Guid).has_value());
}
