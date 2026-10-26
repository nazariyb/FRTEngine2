// The registry snapshot (Assets/Registry/Snapshot.h, design §5.3). A snapshot written from a view and loaded into
// a fresh store gives the same registry - dump, file stats and hashes, ids, mounts, edges - and the engine's and
// the project's files loaded together give the store both came from; mounts are remapped by name to this
// process's ids. Validity is per section: a section that fails its checksum is dropped with what depends on it,
// and what was lost is flagged for the start-up to rebuild (StaleTags with AnalyzeStamp 0: analyze again; no
// rule files: re-evaluate) - never a wrong answer, never a failed start. One bad type table drops only its type;
// one bad file leaves the other intact; a type this build does not know stays Schemaless; another project's
// file is not taken.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "Assets/AssetTypes.h"
#include "Assets/MountPaths.h"
#include "SnapshotTestUtils.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::snapshot_test;


namespace
{
const MountId EngineMounts[] = { EngineMountId };
const MountId ProjectMounts[] = { ProjectMountId, FirstPluginMountId };

// An invalid ProjectId takes the file whatever project it names (bAnyProject: the test inspects a given file).
SSnapshotLoadResult Load (CRegistryStore& Store, const std::filesystem::path& Path, const SGuid& ProjectId = SGuid {},
	std::span<const SMountUpsert> Mounts = {})
{
	const SSnapshotSource sources[] = { SSnapshotSource { Path, ProjectId, !ProjectId.IsValid() } };
	return LoadSnapshots(Store, sources, Mounts);
}

class SnapshotRoundTripTest : public SnapshotTest
{
protected:
	void SetUp () override
	{
		SnapshotTest::SetUp();
		Original.Commit(MakeContent());
		Path = Dir / "AssetRegistry.snapshot";
		FString error;
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId, &error)) << error.CStr();
	}

	CRegistryStore Original;
	std::filesystem::path Path;
	SGuid ProjectId = SGuid::NewRandom();
};
}


// ---------------- Round trips ----------------

TEST_F(SnapshotRoundTripTest, ARoundTripKeepsEveryTableAndDumpsIdentically)
{
	CRegistryStore loaded;
	const SSnapshotLoadResult result = Load(loaded, Path, ProjectId);
	ASSERT_EQ(result.Files.size(), 1u);
	const SSnapshotFileReport& report = result.Files[0];
	EXPECT_EQ(report.State, ESnapshotFileState::Loaded) << report.Message.CStr();
	EXPECT_TRUE(report.DroppedSections.empty()) << report.Message.CStr();
	EXPECT_TRUE(report.DroppedTypeTables.empty());
	EXPECT_EQ(report.StaleRecords, 0u);
	EXPECT_FALSE(report.bRuleFilesDropped);
	EXPECT_FALSE(report.bJournalFound);
	EXPECT_EQ(report.Mounts, (std::vector<FString> { "Engine", "Project", "Plugin.Trees" }));
	EXPECT_EQ(report.JournalBase, Original.GetRevision());
	EXPECT_TRUE(result.bAdoptedInBulk);
	EXPECT_TRUE(result.AnyLoaded());
	EXPECT_EQ(result.Records, Original.Live().GetRecordCount());
	EXPECT_EQ(loaded.GetRevision(), Original.GetRevision());
	ExpectSameContent(Original.Live(), loaded.Live());

	// The registry goes on alike: an edit to both stores answers alike.
	const auto edit = [] (CRegistryStore& Store)
	{
		SRegistryDelta delta;
		AddTexture(delta, "/Project/T/gone.png");
		delta.FileRemoves.push_back("/Plugin.Trees/Bark.png");
		Store.Commit(std::move(delta));
	};
	edit(Original);
	edit(loaded);
	ExpectSameContent(Original.Live(), loaded.Live());
}

TEST_F(SnapshotRoundTripTest, AStoreLoadedFromASnapshotWritesTheSameBytes)
{
	CRegistryStore loaded;
	ASSERT_TRUE(Load(loaded, Path, ProjectId).AnyLoaded());
	const std::filesystem::path again = Dir / "Again.snapshot";
	ASSERT_TRUE(WriteSnapshot(loaded.Live(), {}, again, ProjectId));
	EXPECT_EQ(ReadBytes(again), ReadBytes(Path));

	// One image gives one byte sequence.
	const SRegistryImage image = Original.Live().ExportImage({});
	EXPECT_EQ(EncodeSnapshot(image, ProjectId, 7u), EncodeSnapshot(image, ProjectId, 7u));
}

TEST_F(SnapshotTest, EngineAndProjectFilesLoadedTogetherEqualOneFreshCommit)
{
	CRegistryStore original;
	original.Commit(MakeContent(false));
	const std::filesystem::path engine = Dir / "Engine.snapshot";
	const std::filesystem::path project = Dir / "AssetRegistry.snapshot";
	const SGuid engineId = SGuid::NewRandom();
	const SGuid projectId = SGuid::NewRandom();
	ASSERT_TRUE(WriteSnapshot(original.Live(), EngineMounts, engine, engineId));
	ASSERT_TRUE(WriteSnapshot(original.Live(), ProjectMounts, project, projectId));

	// In either order: the project's edges into /Engine resolve once both are in.
	for (const bool bEngineFirst : { true, false })
	{
		CRegistryStore merged;
		const SSnapshotSource engineSource { engine, engineId };
		const SSnapshotSource projectSource { project, projectId };
		const SSnapshotSource sources[] = { bEngineFirst ? engineSource : projectSource, bEngineFirst ? projectSource : engineSource };
		const SSnapshotLoadResult result = LoadSnapshots(merged, sources);
		ASSERT_EQ(result.Files.size(), 2u);
		for (const SSnapshotFileReport& file : result.Files)
		{
			EXPECT_EQ(file.State, ESnapshotFileState::Loaded) << file.Message.CStr();
			EXPECT_TRUE(file.DroppedSections.empty()) << file.Message.CStr();
		}
		EXPECT_EQ(result.Files[bEngineFirst ? 0u : 1u].Mounts, std::vector<FString> { "Engine" });
		EXPECT_EQ(result.Files[bEngineFirst ? 1u : 0u].Mounts, (std::vector<FString> { "Project", "Plugin.Trees" }));
		EXPECT_TRUE(result.bAdoptedInBulk);   // one merged image, one bulk adoption
		ExpectSameContent(original.Live(), merged.Live());
	}

	// The project alone: its edges into /Engine wait, unresolved, and the engine's rule file is not in it.
	CRegistryStore alone;
	ASSERT_TRUE(Load(alone, project, projectId).AnyLoaded());
	const std::vector<SDependencyView> edges = GetDependencies(alone.Live(), FindRecord(alone.Live(), "/Project/M/m.frtmat.yml"));
	ASSERT_FALSE(edges.empty());
	EXPECT_TRUE(std::any_of(edges.begin(), edges.end(), [] (const SDependencyView& Edge)
	{
		return Edge.TargetPath == FStringView("/Engine/textures/white.png") && HasAnyFlags(Edge.Flags, EDepFlags::Unresolved);
	}));
	EXPECT_FALSE(alone.Live().FindMount("Engine").has_value());
	EXPECT_FALSE(alone.Live().FindRuleFile("Engine", "").has_value());

	// An edge by id into /Engine is stored by the target's mount path and resolves once both files are in.
	CRegistryStore withId;
	withId.Commit(MakeContent(true));
	ASSERT_TRUE(WriteSnapshot(withId.Live(), EngineMounts, engine, engineId));
	ASSERT_TRUE(WriteSnapshot(withId.Live(), ProjectMounts, project, projectId));
	CRegistryStore merged;
	const SSnapshotSource sources[] = { SSnapshotSource { project, projectId }, SSnapshotSource { engine, engineId } };
	ASSERT_TRUE(LoadSnapshots(merged, sources).AnyLoaded());
	const SRecordId white = FindRecord(merged.Live(), "/Engine/Textures/White.png");
	const std::vector<SDependencyView> heroEdges = GetDependencies(merged.Live(), FindRecord(merged.Live(), "/Project/Hero.gltf"));
	EXPECT_TRUE(std::any_of(heroEdges.begin(), heroEdges.end(), [&] (const SDependencyView& Edge)
	{
		return Edge.To == white && Edge.TargetPath == FStringView("/Engine/Textures/White.png") && !HasAnyFlags(Edge.Flags, EDepFlags::Unresolved);
	}));
	EXPECT_EQ(GetReferencers(merged.Live(), white).size(), GetReferencers(withId.Live(), FindRecord(withId.Live(), "/Engine/Textures/White.png")).size());
}

TEST_F(SnapshotTest, MountsAreRemappedByNameToThisProcesssIds)
{
	CRegistryStore original;
	original.Commit(MakeContent());
	const std::filesystem::path project = Dir / "AssetRegistry.snapshot";
	ASSERT_TRUE(WriteSnapshot(original.Live(), ProjectMounts, project, SGuid {}));

	// Another plugin sorts before Trees now: Trees gets id 3.
	{
		const SMountUpsert mounts[] = { MakeMount("Engine", EngineMountId), MakeMount("Project", ProjectMountId),
			MakeMount("Plugin.Apples", FirstPluginMountId), MakeMount("Plugin.Trees", FirstPluginMountId + 1u, true) };
		CRegistryStore loaded;
		const SSnapshotLoadResult result = Load(loaded, project, SGuid {}, mounts);
		ASSERT_EQ(result.Files[0].State, ESnapshotFileState::Loaded) << result.Files[0].Message.CStr();
		EXPECT_TRUE(result.Files[0].SkippedMounts.empty());
		ASSERT_TRUE(loaded.Live().FindMount("Plugin.Trees").has_value());
		EXPECT_EQ(loaded.Live().FindMount("Plugin.Trees")->Id, FirstPluginMountId + 1u);
		EXPECT_TRUE(FindRecord(loaded.Live(), "/Plugin.Trees/Bark.png").IsValid());
		SFileData file;
		ASSERT_TRUE(loaded.Live().GetFile(*loaded.Live().FindFileExact("/Plugin.Trees/Bark.png"), file));
		EXPECT_EQ(file.Mount, FirstPluginMountId + 1u);
		// F3's mount upserts afterwards agree with what the image made.
		SRegistryDelta again;
		for (const SMountUpsert& mount : mounts)
		{
			again.Mounts.push_back(mount);
		}
		loaded.Commit(std::move(again));
		EXPECT_EQ(GetDiagnostics(loaded.Live(), FindRecord(loaded.Live(), "/Plugin.Trees/Bark.png"), 0u).size(), 0u);
		uint32 contractProblems = 0u;
		loaded.Live().ForEachProblem([&] (FStringView, EProblemSource Source, const SDiagnostic& Diagnostic)
		{
			contractProblems += Source == EProblemSource::Registry && Diagnostic.Code == 0u ? 1u : 0u;
		});
		EXPECT_EQ(contractProblems, 0u);
	}

	// The plugin is gone, or its folder became case-insensitive: it starts cold; /Project still loads.
	for (const bool bRemoved : { true, false })
	{
		std::vector<SMountUpsert> mounts = { MakeMount("Engine", EngineMountId), MakeMount("Project", ProjectMountId) };
		if (!bRemoved)
		{
			mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, false));
		}
		CRegistryStore loaded;
		const SSnapshotLoadResult result = Load(loaded, project, SGuid {}, mounts);
		ASSERT_EQ(result.Files[0].State, ESnapshotFileState::Loaded);
		ASSERT_EQ(result.Files[0].SkippedMounts.size(), 1u);
		EXPECT_TRUE(result.Files[0].SkippedMounts[0].StartsWith("Plugin.Trees ("));
		EXPECT_EQ(result.Files[0].Mounts, std::vector<FString> { "Project" });
		EXPECT_FALSE(loaded.Live().FindMount("Plugin.Trees").has_value());
		EXPECT_FALSE(loaded.Live().FindFileExact("/Plugin.Trees/Bark.png").has_value());
		EXPECT_TRUE(FindRecord(loaded.Live(), "/Project/T/Rock.png").IsValid());
		EXPECT_TRUE(result.bAdoptedInBulk);
	}
}


// ---------------- Per-section validity ----------------

TEST_F(SnapshotRoundTripTest, AFileWithoutItsStructureIsNotUsed)
{
	for (const ESnapshotSection section : { ESnapshotSection::Mounts, ESnapshotSection::Names, ESnapshotSection::Folders, ESnapshotSection::Files })
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, section);
		CRegistryStore loaded;
		const SSnapshotLoadResult result = Load(loaded, Path, ProjectId);
		EXPECT_EQ(result.Files[0].State, ESnapshotFileState::Rejected) << enum_::ToString(section).GetData();
		EXPECT_TRUE(HasDropped(result.Files[0], section));
		EXPECT_FALSE(result.AnyLoaded());
		EXPECT_EQ(loaded.Live().GetFileCount(), 0u);
		EXPECT_TRUE(loaded.Live().GetMounts().empty());
	}
}

TEST_F(SnapshotRoundTripTest, LostRecordsKeepTheirFilesStatsAndHashes)
{
	DamageSection(Path, ESnapshotSection::Assets);
	CRegistryStore loaded;
	const SSnapshotLoadResult result = Load(loaded, Path, ProjectId);
	const SSnapshotFileReport& report = result.Files[0];
	ASSERT_EQ(report.State, ESnapshotFileState::Loaded);
	EXPECT_TRUE(HasDropped(report, ESnapshotSection::Assets));
	EXPECT_TRUE(HasDropped(report, ESnapshotSection::SubEntry));
	EXPECT_TRUE(HasDropped(report, ESnapshotSection::Deps));
	EXPECT_EQ(loaded.Live().GetRecordCount(), 0u);
	EXPECT_EQ(loaded.Live().GetFileCount(), Original.Live().GetFileCount());
	EXPECT_FALSE(report.bRuleFilesDropped);
	EXPECT_TRUE(loaded.Live().FindRuleFile("Project", "/Project/T").has_value());
	SFileData before;
	SFileData after;
	ASSERT_TRUE(Original.Live().GetFile(*Original.Live().FindFileExact("/Project/T/Rock.png"), before));
	ASSERT_TRUE(loaded.Live().GetFile(*loaded.Live().FindFileExact("/Project/T/Rock.png"), after));
	EXPECT_EQ(after.ContentHash, before.ContentHash);
	EXPECT_EQ(after.MTime, before.MTime);
	EXPECT_EQ(after.Identity, before.Identity);
	EXPECT_FALSE(after.Record.IsValid());
}

TEST_F(SnapshotRoundTripTest, LostAnalyzeOutputsMakeOnlyTheirRecordsStale)
{
	// SUBENTRY: the model loses its sub-entries (and the edges that may name them) and is analyzed again.
	{
		DamageSection(Path, ESnapshotSection::SubEntry);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		ASSERT_EQ(report.State, ESnapshotFileState::Loaded);
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::SubEntry));
		EXPECT_EQ(report.StaleRecords, 1u);
		EXPECT_EQ(loaded.Live().GetSubEntryCount(), 0u);
		const SAssetData model = GetData(loaded.Live(), "/Project/Hero.gltf");
		EXPECT_TRUE(HasAnyFlags(model.Flags, ERecordFlags::StaleTags));
		EXPECT_EQ(GetAnalyzeStamp(loaded.Live(), "/Project/Hero.gltf"), 0u);
		EXPECT_TRUE(GetDependencies(loaded.Live(), model.Record).empty());
		EXPECT_FALSE(HasAnyFlags(GetData(loaded.Live(), "/Project/T/Rock.png").Flags, ERecordFlags::StaleTags));
		EXPECT_NE(GetAnalyzeStamp(loaded.Live(), "/Project/T/Rock.png"), 0u);
		EXPECT_FALSE(report.bRuleFilesDropped);
	}
	// DEPS: every record with edges or companions is stale; the companion is a plain file until re-analyzed.
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, ESnapshotSection::Deps);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Deps));
		EXPECT_EQ(report.StaleRecords, 4u);   // two materials, the model, the input map
		for (const FStringView path : { "/Project/M/m.frtmat.yml", "/Plugin.Trees/Oak.frtmat.yml", "/Project/Hero.gltf", "/Project/Input/Map.frtinputmap" })
		{
			EXPECT_TRUE(HasAnyFlags(GetData(loaded.Live(), path).Flags, ERecordFlags::StaleTags)) << FString(path).CStr();
		}
		EXPECT_FALSE(HasAnyFlags(GetData(loaded.Live(), "/Project/Input/Jump.frtinputaction").Flags, ERecordFlags::StaleTags));
		SFileData bin;
		ASSERT_TRUE(loaded.Live().GetFile(*loaded.Live().FindFileExact("/Project/Hero.bin"), bin));
		EXPECT_EQ(bin.Class, EFileClass::Unclaimed);
		EXPECT_EQ(loaded.Live().GetSubEntryCount(), Original.Live().GetSubEntryCount());
	}
	// TYPES: every type index is meaningless; every record is frt.Unknown and analyzed again.
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, ESnapshotSection::Types);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Types));
		EXPECT_EQ(report.StaleRecords, Original.Live().GetRecordCount());
		const SAssetData rock = GetData(loaded.Live(), "/Project/T/Rock.png");
		EXPECT_EQ(rock.Type, asset_types::Unknown);
		EXPECT_TRUE(HasAnyFlags(rock.Flags, ERecordFlags::StaleTags));
		EXPECT_EQ(GetAnalyzeStamp(loaded.Live(), "/Project/T/Rock.png"), 0u);
	}
}

TEST_F(SnapshotRoundTripTest, LostRuleOutputsDropTheRuleHashes)
{
	// LABELS: labels are gone and no RulesHash survives, so every record is re-evaluated; nothing is stale.
	{
		DamageSection(Path, ESnapshotSection::Labels);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Labels));
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::RuleFiles));
		EXPECT_TRUE(report.bRuleFilesDropped);
		EXPECT_EQ(report.StaleRecords, 0u);
		EXPECT_FALSE(loaded.Live().FindRuleFile("Project", "").has_value());
		uint32 labels = 0u;
		loaded.Live().ForEachLabel(FindRecord(loaded.Live(), "/Project/Hero.gltf"), [&] (FStringView, bool) { ++labels; });
		EXPECT_EQ(labels, 0u);
	}
	// RULEFILES alone: the hashes go, the labels stay.
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, ESnapshotSection::RuleFiles);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		EXPECT_EQ(report.DroppedSections, std::vector<ESnapshotSection> { ESnapshotSection::RuleFiles });
		EXPECT_FALSE(loaded.Live().FindRuleFile("Project", "/Project/T").has_value());
		uint32 labels = 0u;
		loaded.Live().ForEachLabel(FindRecord(loaded.Live(), "/Project/Hero.gltf"), [&] (FStringView, bool) { ++labels; });
		EXPECT_EQ(labels, 2u);
		EXPECT_EQ(report.StaleRecords, 0u);
	}
	// PROBLEMS: Analyze and Rules problems are lost: every record is analyzed and re-evaluated again.
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, ESnapshotSection::Problems);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Problems));
		EXPECT_TRUE(report.bRuleFilesDropped);
		EXPECT_EQ(report.StaleRecords, Original.Live().GetRecordCount());
		uint32 analyzeProblems = 0u;
		loaded.Live().ForEachProblem([&] (FStringView, EProblemSource Source, const SDiagnostic&) { analyzeProblems += Source == EProblemSource::Analyze ? 1u : 0u; });
		EXPECT_EQ(analyzeProblems, 0u);
	}
	// STRINGS: edges, companions and problems go with it.
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, ESnapshotSection::Strings);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		ASSERT_EQ(report.State, ESnapshotFileState::Loaded);
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Strings));
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Deps));
		EXPECT_TRUE(HasDropped(report, ESnapshotSection::Problems));
		EXPECT_EQ(report.StaleRecords, Original.Live().GetRecordCount());
		EXPECT_TRUE(GetDependencies(loaded.Live(), FindRecord(loaded.Live(), "/Project/M/m.frtmat.yml")).empty());
		STagValue width;
		EXPECT_TRUE(loaded.Live().GetTag(FindRecord(loaded.Live(), "/Project/T/Rock.png"), "Width", width));
	}
	// The sections M3 writes empty are not read: damage there costs nothing.
	{
		ASSERT_TRUE(WriteSnapshot(Original.Live(), {}, Path, ProjectId));
		DamageSection(Path, ESnapshotSection::Metas);
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
		EXPECT_TRUE(report.DroppedSections.empty());
		ExpectSameContent(Original.Live(), loaded.Live());
	}
}

namespace
{
// Where a type table's name ends inside TYPETABLES (its schema hash follows), for byte surgery.
size_t FindTypeTable (std::span<const uint8> Bytes, FStringView TypeName)
{
	const std::optional<std::pair<size_t, size_t>> section = FindSection(Bytes, ESnapshotSection::TypeTables);
	if (!section)
	{
		return 0u;
	}
	const std::span<const uint8> body = Bytes.subspan(section->first, section->second);
	const auto found = std::search(body.begin(), body.end(), TypeName.AsBytes().begin(), TypeName.AsBytes().end());
	return found == body.end() ? 0u : section->first + static_cast<size_t>(found - body.begin()) + TypeName.Len();
}
}

TEST_F(SnapshotRoundTripTest, OneDamagedTypeTableDropsOnlyItsType)
{
	std::vector<uint8> bytes = ReadBytes(Path);
	const size_t texture = FindTypeTable(bytes, "frt.Texture");
	ASSERT_GT(texture, 0u);
	// Past the schema hash (8), the payload size (one varint byte here) and the checksum (8): the payload.
	bytes[texture + 8u + 1u + 8u + 2u] ^= 0x33u;
	WriteBytes(Path, bytes);

	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
	ASSERT_EQ(report.State, ESnapshotFileState::Loaded);
	EXPECT_EQ(report.DroppedTypeTables, std::vector<FString> { "frt.Texture" });
	EXPECT_FALSE(HasDropped(report, ESnapshotSection::TypeTables));
	// Four texture files and the model (one of its sub-entries is a texture) are analyzed again.
	EXPECT_EQ(report.StaleRecords, 5u);
	STagValue value;
	EXPECT_FALSE(loaded.Live().GetTag(FindRecord(loaded.Live(), "/Project/T/Rock.png"), "Width", value));
	EXPECT_TRUE(HasAnyFlags(GetData(loaded.Live(), "/Project/T/Rock.png").Flags, ERecordFlags::StaleTags));
	EXPECT_TRUE(HasAnyFlags(GetData(loaded.Live(), "/Project/Hero.gltf").Flags, ERecordFlags::StaleTags));
	EXPECT_TRUE(loaded.Live().GetTag(FindRecord(loaded.Live(), "/Project/Hero.gltf"), "MeshCount", value));
	EXPECT_TRUE(loaded.Live().GetTag(FindRecord(loaded.Live(), "/Project/Hero.gltf#mesh:Body"), "VertexCount", value));
	EXPECT_FALSE(HasAnyFlags(GetData(loaded.Live(), "/Project/Input/Map.frtinputmap").Flags, ERecordFlags::StaleTags));
	uint32 actions = 0u;
	loaded.Live().ForEachTagValue(FindRecord(loaded.Live(), "/Project/Input/Map.frtinputmap"), "Actions", [&] (const STagValue&) { ++actions; });
	EXPECT_EQ(actions, 2u);
}

TEST_F(SnapshotRoundTripTest, ATypeTableOfAnotherTagSchemaIsGatheredAgain)
{
	// This build declares other keys for frt.Mesh than the writer did: only meshes' owners are analyzed again.
	std::vector<uint8> bytes = ReadBytes(Path);
	const size_t mesh = FindTypeTable(bytes, "frt.Mesh");
	ASSERT_GT(mesh, 0u);
	bytes[mesh] ^= 0x01u;   // the stored TagSchemaHash
	WriteBytes(Path, bytes);

	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded, Path, ProjectId).Files[0];
	EXPECT_EQ(report.DroppedTypeTables, std::vector<FString> { "frt.Mesh" });
	EXPECT_EQ(report.StaleRecords, 1u);
	EXPECT_TRUE(HasAnyFlags(GetData(loaded.Live(), "/Project/Hero.gltf").Flags, ERecordFlags::StaleTags));
	STagValue value;
	EXPECT_FALSE(loaded.Live().GetTag(FindRecord(loaded.Live(), "/Project/Hero.gltf#mesh:Body"), "VertexCount", value));
	EXPECT_TRUE(loaded.Live().GetTag(FindRecord(loaded.Live(), "/Project/T/Rock.png"), "Width", value));
}

TEST_F(SnapshotTest, ADamagedFileLeavesTheOtherFileIntact)
{
	CRegistryStore original;
	original.Commit(MakeContent());
	const std::filesystem::path engine = Dir / "Engine.snapshot";
	const std::filesystem::path project = Dir / "AssetRegistry.snapshot";
	ASSERT_TRUE(WriteSnapshot(original.Live(), EngineMounts, engine, SGuid {}));
	ASSERT_TRUE(WriteSnapshot(original.Live(), ProjectMounts, project, SGuid {}));
	const SSnapshotSource sources[] = { SSnapshotSource { engine, SGuid {}, true }, SSnapshotSource { project, SGuid {}, true } };

	DamageSection(engine, ESnapshotSection::Files);
	{
		CRegistryStore merged;
		const SSnapshotLoadResult result = LoadSnapshots(merged, sources);
		EXPECT_EQ(result.Files[0].State, ESnapshotFileState::Rejected);
		EXPECT_EQ(result.Files[1].State, ESnapshotFileState::Loaded);
		EXPECT_TRUE(result.Files[1].DroppedSections.empty());
		EXPECT_FALSE(merged.Live().FindMount("Engine").has_value());
		EXPECT_FALSE(FindRecord(merged.Live(), "/Engine/Textures/White.png").IsValid());
		EXPECT_TRUE(FindRecord(merged.Live(), "/Project/T/Rock.png").IsValid());
		EXPECT_EQ(result.Files[1].StaleRecords, 0u);
	}

	ASSERT_TRUE(WriteSnapshot(original.Live(), EngineMounts, engine, SGuid {}));
	DamageSection(project, ESnapshotSection::SubEntry);
	{
		CRegistryStore merged;
		const SSnapshotLoadResult result = LoadSnapshots(merged, sources);
		EXPECT_TRUE(result.Files[0].DroppedSections.empty());
		EXPECT_EQ(result.Files[0].StaleRecords, 0u);
		EXPECT_TRUE(HasDropped(result.Files[1], ESnapshotSection::SubEntry));
		EXPECT_FALSE(HasAnyFlags(GetData(merged.Live(), "/Engine/Textures/White.png").Flags, ERecordFlags::StaleTags));
		EXPECT_TRUE(HasAnyFlags(GetData(merged.Live(), "/Project/Hero.gltf").Flags, ERecordFlags::StaleTags));
	}
}

TEST_F(SnapshotTest, ARecordOfATypeThisBuildDoesNotKnowWaitsSchemaless)
{
	CRegistryStore original;
	SRegistryDelta delta = MakeContent();
	for (SRecordUpsert& record : delta.RecordUpserts)
	{
		if (record.FileMountPath == "/Project/Input/Jump.frtinputaction")
		{
			record.AnalyzeStamp = 0x77u;
		}
	}
	original.Commit(std::move(delta));
	SRegistryImage image = original.Live().ExportImage({});
	const auto type = std::find(image.TypeNames.begin(), image.TypeNames.end(), FString("frt.InputAction"));
	ASSERT_NE(type, image.TypeNames.end());
	*type = "Game.Weapon";
	const std::filesystem::path path = Dir / "AssetRegistry.snapshot";
	WriteBytes(path, EncodeSnapshot(image, SGuid {}, 5u));

	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded, path).Files[0];
	ASSERT_EQ(report.State, ESnapshotFileState::Loaded);
	EXPECT_TRUE(report.DroppedSections.empty());
	EXPECT_EQ(report.StaleRecords, 0u);   // not lost: waiting for its type
	const SAssetData action = GetData(loaded.Live(), "/Project/Input/Jump.frtinputaction");
	EXPECT_EQ(action.Type, asset_types::Unknown);
	EXPECT_TRUE(HasAllFlags(action.Flags, ERecordFlags::Schemaless | ERecordFlags::StaleTags));
	EXPECT_EQ(GetAnalyzeStamp(loaded.Live(), "/Project/Input/Jump.frtinputaction"), 0x77u);
	STagValue value;
	EXPECT_FALSE(loaded.Live().GetTag(action.Record, "ValueType", value));
	EXPECT_EQ(loaded.GetRevision(), 5u);
}


// ---------------- Whole files ----------------

TEST_F(SnapshotTest, AFileOfAnotherProjectFormatOrKindIsNotTaken)
{
	CRegistryStore original;
	original.Commit(MakeContent());
	const std::filesystem::path path = Dir / "AssetRegistry.snapshot";
	const SGuid writer = SGuid::NewRandom();
	ASSERT_TRUE(WriteSnapshot(original.Live(), {}, path, writer));

	const auto loadState = [&] (const SGuid& Expected, bool bAnyProject = false)
	{
		CRegistryStore loaded;
		const SSnapshotSource sources[] = { SSnapshotSource { path, Expected, bAnyProject } };
		const SSnapshotLoadResult result = LoadSnapshots(loaded, sources);
		EXPECT_EQ(loaded.Live().GetRecordCount(), result.AnyLoaded() ? original.Live().GetRecordCount() : 0u);
		return result.Files[0].State;
	};
	EXPECT_EQ(loadState(writer), ESnapshotFileState::Loaded);
	EXPECT_EQ(loadState(SGuid::NewRandom()), ESnapshotFileState::Rejected);
	// A process without a ProjectId file (ReadProjectId gave nullopt) takes no snapshot of its root; a tool that
	// inspects the file asks for any project.
	EXPECT_EQ(loadState(SGuid {}), ESnapshotFileState::Rejected);
	EXPECT_EQ(loadState(SGuid {}, true), ESnapshotFileState::Loaded);
	EXPECT_EQ(loadState(SGuid::NewRandom(), true), ESnapshotFileState::Loaded);

	std::vector<uint8> bytes = ReadBytes(path);
	std::vector<uint8> version = bytes;
	version[8] = 99u;
	WriteBytes(path, version);
	EXPECT_EQ(loadState(writer), ESnapshotFileState::Rejected);
	WriteBytes(path, std::span<const uint8>(bytes).first(30u));
	EXPECT_EQ(loadState(writer), ESnapshotFileState::Rejected);
	const char text[] = "FormatVersion: 1\n";
	WriteBytes(path, std::span<const uint8>(reinterpret_cast<const uint8*>(text), sizeof(text) - 1u));
	EXPECT_EQ(loadState(writer), ESnapshotFileState::Rejected);
	std::filesystem::remove(path);
	EXPECT_EQ(loadState(writer), ESnapshotFileState::Missing);

	// A section table that names offsets past the end leaves those sections absent.
	std::vector<uint8> table = bytes;
	binary::WriteLittleEndianAt(table, SnapshotHeaderBytes + 8u, 0xFFFFFFFFFFull, 8u);   // MOUNTS' offset
	WriteBytes(path, table);
	EXPECT_EQ(loadState(writer), ESnapshotFileState::Rejected);
}

TEST_F(SnapshotTest, TheProjectIdIsCreatedOnceAndReadBack)
{
	const std::filesystem::path root = Dir / "Проєкт";
	EXPECT_FALSE(ReadProjectId(root).has_value());
	FString error;
	const std::optional<SGuid> created = ReadOrCreateProjectId(root, &error);
	ASSERT_TRUE(created.has_value()) << error.CStr();
	EXPECT_TRUE(created->IsValid());
	EXPECT_TRUE(std::filesystem::exists(GetProjectIdPath(root)));
	EXPECT_EQ(ReadProjectId(root), created);
	EXPECT_EQ(ReadOrCreateProjectId(root), created);
	EXPECT_EQ(GetJournalPath(Dir / "AssetRegistry.snapshot"), Dir / "AssetRegistry.journal");

	// A damaged id is replaced (its snapshots are then another project's: a cold start, never a wrong load).
	ASSERT_TRUE(platform::WriteFileText(GetProjectIdPath(root), "not a guid"));
	EXPECT_FALSE(ReadProjectId(root).has_value());
	const std::optional<SGuid> replaced = ReadOrCreateProjectId(root);
	ASSERT_TRUE(replaced.has_value());
	EXPECT_NE(replaced, created);
}
