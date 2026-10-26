// The importers' common contract (E3, design §7.2, §5.2): every available ImporterTable row has an Analyze
// implementation and no unavailable one does; AnalyzeFile sets the import status (Analyzed, AnalyzeFailed with the
// record kept, ImporterUnavailable, NotAnalyzed without an importer); AppendRecordUpsert writes the whole record -
// provisional id, importer, AnalyzeStamp, the rules' cook rule, chunk and labels - and replaces the file's Analyze
// problems per subject; and what the importers produce commits into the registry store with every edge and companion
// resolved.
#include <gtest/gtest.h>

#include <vector>

#include "ImporterTestUtils.h"

#include "Assets/Registry/RegistryStore.h"
#include "Assets/Registry/RegistryView.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
SFileUpsert MakeFileRow (FStringView MountPath, EFileClass Class)
{
	SFileUpsert file;
	file.MountPath = FString(MountPath);
	file.Size = 1u;
	file.MTime = 1;
	file.Class = Class;
	return file;
}
}


TEST(AssetImporterTest, EveryAvailableImporterHasAnImplementation)
{
	const std::span<const SImporterDesc> table = GetImporters();
	const std::span<const IAssetImporter* const> importers = GetAssetImporters();
	uint32 available = 0u;
	for (const SImporterDesc& desc : table)
	{
		SCOPED_TRACE(desc.Name.GetData());
		const IAssetImporter* importer = FindAssetImporter(desc);
		EXPECT_EQ(importer != nullptr, desc.bAvailable);
		if (importer != nullptr)
		{
			EXPECT_EQ(&importer->GetInfo(), &desc);
			ASSERT_LT(available, importers.size());
			EXPECT_EQ(importers[available], importer);   // in the table's order
			++available;
		}
	}
	EXPECT_EQ(available, importers.size());
	EXPECT_EQ(FindAssetImporter("model.GLTF"), FindAssetImporter(*FindImporter("Model.Gltf")));
	EXPECT_EQ(FindAssetImporter("Document"), nullptr);
	EXPECT_EQ(FindAssetImporter("NoSuchImporter"), nullptr);
}

TEST(AssetImporterTest, AnalyzeFileSetsTheImportStatus)
{
	const CImporterTree tree("Status");
	tree.Write("Data/table.frt", "$type: frt.DataTable\n");
	tree.Write("Fonts/font.ttf", "glyphs");
	const CMountTable mounts = tree.Resolve();
	const SMount& project = *mounts.Find("Project");

	SAnalyzeResult result;
	result.Tags.push_back({ "Stale", STagValue::FromInt64(1) });
	EXPECT_FALSE(AnalyzeFile(*SAnalyzeInput::FromMount(project, "/Project/Data/table.frt"), MakeSettings(nullptr), result));
	EXPECT_EQ(result.Status, EImportStatus::NotAnalyzed);
	EXPECT_TRUE(result.Tags.empty());   // reset first

	// A rule may name an importer this build does not have: the record says so.
	EXPECT_FALSE(AnalyzeFile(*SAnalyzeInput::FromMount(project, "/Project/Data/table.frt"), MakeSettings(FindImporter("Document")), result));
	EXPECT_EQ(result.Status, EImportStatus::ImporterUnavailable);
	EXPECT_EQ(result.Type, asset_types::Unknown);
	ASSERT_EQ(result.Diagnostics.size(), 1u);
	EXPECT_EQ(result.Diagnostics[0].Severity, ESeverity::Info);

	// Raw records the file and nothing else; nothing is read.
	EXPECT_TRUE(AnalyzeFile(*SAnalyzeInput::FromMount(project, "/Project/Fonts/font.ttf"), MakeSettings(FindImporter("Raw")), result));
	EXPECT_EQ(result.Status, EImportStatus::Analyzed);
	EXPECT_EQ(result.Type, asset_types::Raw);
	EXPECT_TRUE(result.Tags.empty());
	EXPECT_TRUE(result.Diagnostics.empty());
	EXPECT_FALSE(result.ContentHash.has_value());

	// A rule that gives a file another importer: Analyze fails, the record keeps that importer's type.
	const SAnalyzeResult forced = AnalyzeAt(mounts, "/Project/Fonts/font.ttf", FindImporter("Texture"));
	EXPECT_EQ(forced.Status, EImportStatus::AnalyzeFailed);
	EXPECT_EQ(forced.Type, asset_types::Texture);

	EXPECT_FALSE(SAnalyzeInput::FromMount(project, "/Engine/x.png").has_value());
	EXPECT_FALSE(SAnalyzeInput::FromMount(project, "/Project").has_value());
	EXPECT_FALSE(SAnalyzeInput::FromMount(project, "/Project/../x.png").has_value());
}

TEST(AssetImporterTest, AppendRecordUpsertWritesTheWholeRecordAndItsProblems)
{
	const CImporterTree tree("Upsert");
	tree.Write("Models/m.gltf", R"({"asset":{"version":"2.0"},"meshes":[{"name":"A#2","primitives":[]},{"name":"A","primitives":[]},{"name":"A","primitives":[]}],
		"images":[{"uri":"../../../../x.png"}]})");
	const SAnalyzeResult result = tree.Analyze("Models/m.gltf");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed);
	ASSERT_EQ(result.Diagnostics.size(), 2u) << DescribeDiagnostics(result).CStr();

	const SImporterDesc* gltf = FindImporter("Model.Gltf");
	SEffectiveFileSettings settings = MakeSettings(gltf);
	settings.CookRule = ECookRule::Always;
	settings.Chunk = 3u;
	settings.RuleLabels = { "Props", "Hero" };

	SRegistryDelta delta;
	AppendRecordUpsert(result, settings, "/Project/Models/m.gltf", delta);
	ASSERT_EQ(delta.RecordUpserts.size(), 1u);
	const SRecordUpsert& record = delta.RecordUpserts[0];
	EXPECT_EQ(record.FileMountPath, "/Project/Models/m.gltf");
	EXPECT_EQ(record.Guid, MakeProvisionalGuid("/Project/Models/m.gltf"));
	EXPECT_EQ(record.Type, asset_types::Model);
	EXPECT_EQ(record.Importer, "Model.Gltf");
	EXPECT_EQ(record.CookRule, ECookRule::Always);
	EXPECT_EQ(record.Chunk, 3u);
	EXPECT_EQ(record.RuleLabels, (std::vector<FString> { "Props", "Hero" }));
	EXPECT_EQ(record.ImportStatus, EImportStatus::Analyzed);
	EXPECT_EQ(record.AnalyzeStamp, MakeAnalyzeStamp(gltf->AnalyzeVersion, 0u));
	EXPECT_EQ(record.Tags.size(), result.Tags.size());
	EXPECT_EQ(record.SubEntries.size(), 3u);   // two meshes and the image
	EXPECT_TRUE(record.PrimaryName.IsEmpty());

	// The file's Analyze problems are cleared, then set per subject: the file's warning, the sub-entry's error.
	ASSERT_EQ(delta.ProblemClears.size(), 1u);
	EXPECT_EQ(delta.ProblemClears[0].MountPath, "/Project/Models/m.gltf");
	EXPECT_EQ(delta.ProblemClears[0].Source, EProblemSource::Analyze);
	ASSERT_EQ(delta.Problems.size(), 2u);
	for (const SProblemsSet& set : delta.Problems)
	{
		EXPECT_EQ(set.Source, EProblemSource::Analyze);
		ASSERT_EQ(set.Diagnostics.size(), 1u);
		EXPECT_EQ(set.Diagnostics[0].Subject, set.Subject);
	}
	EXPECT_EQ(delta.Problems[0].Subject, "/Project/Models/m.gltf#mesh:A#2");
	EXPECT_EQ(delta.Problems[1].Subject, "/Project/Models/m.gltf");

	// The rvalue form moves; an unavailable importer's record has no stamp.
	SAnalyzeResult moved = result;
	SRegistryDelta second;
	AppendRecordUpsert(std::move(moved), settings, "/Project/Models/m.gltf", second);
	EXPECT_EQ(second.RecordUpserts[0].SubEntries.size(), 3u);
	EXPECT_EQ(second.Problems.size(), 2u);

	SAnalyzeResult unavailable;
	unavailable.Status = EImportStatus::ImporterUnavailable;
	SRegistryDelta third;
	AppendRecordUpsert(unavailable, MakeSettings(FindImporter("Document")), "/Project/Data/t.frt", third);
	EXPECT_EQ(third.RecordUpserts[0].AnalyzeStamp, 0u);
	EXPECT_EQ(third.RecordUpserts[0].Importer, "Document");
	EXPECT_TRUE(third.Problems.empty());
	EXPECT_EQ(third.ProblemClears.size(), 1u);   // an earlier Analyze's problems do not survive
}

TEST(AssetImporterTest, AnalyzedDemoFilesCommitWithResolvedEdges)
{
	const CMountTable mounts = ResolveDemoMounts();
	constexpr FStringLiteral Model = "/Project/Models/Duck/Duck.gltf";
	constexpr FStringLiteral Buffer = "/Project/Models/Duck/Duck0.bin";
	constexpr FStringLiteral Texture = "/Project/Models/Duck/DuckCM.png";
	constexpr FStringLiteral Material = "/Project/Models/Duck/Duck_mat0.frtmat.yml";
	constexpr FStringLiteral InputMap = "/Project/Input/IAM_Editor.frtinputmap";
	constexpr FStringLiteral Action = "/Project/Input/IA_MoveUp.frtinputaction";

	SRegistryDelta delta;
	delta.Mounts.push_back(SMountUpsert { "Engine", false, EngineMountId });
	delta.Mounts.push_back(SMountUpsert { "Project", false, ProjectMountId });
	delta.FileUpserts.push_back(MakeFileRow(Buffer, EFileClass::Unclaimed));
	for (const FStringLiteral path : { Model, Texture, Material, InputMap, Action })
	{
		delta.FileUpserts.push_back(MakeFileRow(path, EFileClass::Asset));
		SAnalyzeResult result = AnalyzeAt(mounts, path);
		ASSERT_EQ(result.Status, EImportStatus::Analyzed) << path.GetData() << DescribeDiagnostics(result).CStr();
		AppendRecordUpsert(std::move(result), MakeSettings(FindImporterForFile(GetMountPathLeaf(path))), path, delta);
	}
	CRegistryStore store;
	store.Commit(std::move(delta));
	const CRegistryView& view = store.Live();

	// The buffer is the model's companion; the sub-entries are reachable by "path#label".
	SFileData buffer;
	ASSERT_TRUE(view.GetFile(*view.FindFileExact(Buffer), buffer));
	EXPECT_EQ(buffer.Class, EFileClass::Companion);
	EXPECT_TRUE(view.FindByPath("/Project/Models/Duck/Duck.gltf#mesh:LOD3spShape").has_value());
	EXPECT_TRUE(view.FindByPath("/Project/Models/Duck/Duck.gltf#image:#0").has_value());

	const auto countUnresolved = [&] (FStringView Path)
	{
		uint32 total = 0u;
		uint32 unresolved = 0u;
		view.ForEachDependency(*view.FindByPath(Path), EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge)
		{
			++total;
			unresolved += (Edge.Flags & EDepFlags::Unresolved) != EDepFlags {} ? 1u : 0u;
		});
		return std::pair(total, unresolved);
	};
	// The image edge resolves to the texture's record. The buffer edge names the companion, a FILES row without a
	// record of its own, and the store resolves edges to records only: it stays Unresolved (an open question for
	// the integration, not the importer's).
	bool bTextureResolved = false;
	view.ForEachDependency(*view.FindByPath(Model), EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge)
	{
		if (Edge.TargetPath == Texture)
		{
			bTextureResolved = (Edge.Flags & EDepFlags::Unresolved) == EDepFlags {} && Edge.FromSub == MakeSubId("image:#0");
		}
	});
	EXPECT_TRUE(bTextureResolved);
	EXPECT_EQ(countUnresolved(Model).first, 2u);
	EXPECT_EQ(countUnresolved(Material), std::pair(1u, 0u));
	// Only IA_MoveUp's file is registered here: three of the map's four edges wait for theirs.
	EXPECT_EQ(countUnresolved(InputMap), std::pair(4u, 3u));

	STagValue width;
	ASSERT_TRUE(view.GetTag(*view.FindByPath(Texture), "Width", width));
	EXPECT_EQ(width, STagValue::FromInt64(512));
	SAssetData map;
	ASSERT_TRUE(view.GetRecord(*view.FindByPath(InputMap), map));
	EXPECT_TRUE((map.Flags & ERecordFlags::Primary) != ERecordFlags::None);

	uint32 notices = 0u;
	view.ForEachDiagnosticAt(Material, [&] (EProblemSource Source, const SDiagnostic& Diagnostic)
	{
		notices += Source == EProblemSource::Analyze && Diagnostic.Code == Ref007 ? 1u : 0u;
	});
	EXPECT_EQ(notices, 1u);
}
