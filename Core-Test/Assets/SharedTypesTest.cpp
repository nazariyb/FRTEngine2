// The registry's shared declarations (M3 wave 0): the mount-path helpers every piece splits and joins paths
// with, the static asset-type table (TypeIds and tag keys the importers emit), the importer table's
// extension claims (longest match, the portable fold, unavailable importers), and the frozen derivations
// that end up in snapshots and records - MakeAnalyzeStamp and the provisional id - each pinned to a known
// answer and recomputed from the formula its header documents.
#include <gtest/gtest.h>

#include <set>
#include <span>
#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetId.h"
#include "Assets/AssetTypes.h"
#include "Assets/Discovery/ScanTypes.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/RuleTypes.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Hash/StableHash.h"

using namespace frt;
using namespace frt::assets;


namespace
{
struct SSplitResult
{
	bool bOk = false;
	FString MountName;
	FString Relative;
};

SSplitResult Split (FStringView Path)
{
	SSplitResult result;
	FStringView mountName = "untouched";
	FStringView relative = "untouched";
	result.bOk = SplitMountPath(Path, mountName, relative);
	result.MountName = FString(mountName);
	result.Relative = FString(relative);
	return result;
}

FStringView ImporterNameForFile (FStringView FileName)
{
	const SImporterDesc* importer = FindImporterForFile(FileName);
	return importer ? FStringView(importer->Name) : FStringView("<none>");
}
}


// ---------------- Mount paths ----------------

TEST(SharedTypesTest, SplitMountPathSeparatesTheMountNameFromTheRelativePart)
{
	SSplitResult split = Split("/Project/A/b.png");
	EXPECT_TRUE(split.bOk);
	EXPECT_EQ(split.MountName, "Project");
	EXPECT_EQ(split.Relative, "A/b.png");

	split = Split("/Plugin.Foliage/Trees/Oak.gltf");
	EXPECT_TRUE(split.bOk);
	EXPECT_EQ(split.MountName, "Plugin.Foliage");
	EXPECT_EQ(split.Relative, "Trees/Oak.gltf");

	// A mount root has an empty relative part.
	split = Split("/Engine");
	EXPECT_TRUE(split.bOk);
	EXPECT_EQ(split.MountName, "Engine");
	EXPECT_EQ(split.Relative, "");

	split = Split("/Project/Текстури/Камінь 漢字.png");
	EXPECT_TRUE(split.bOk);
	EXPECT_EQ(split.MountName, "Project");
	EXPECT_EQ(split.Relative, "Текстури/Камінь 漢字.png");
}

TEST(SharedTypesTest, SplitMountPathRejectsPathsThatNameNoMount)
{
	for (const FStringView path : { "", "/", "//A", "Project/A", "A" })
	{
		const SSplitResult split = Split(path);
		EXPECT_FALSE(split.bOk) << path;
		EXPECT_EQ(split.MountName, "untouched") << path;
		EXPECT_EQ(split.Relative, "untouched") << path;
	}
}

TEST(SharedTypesTest, JoinMountPathIsTheInverseOfSplit)
{
	EXPECT_EQ(JoinMountPath("Project", "A/b.png"), "/Project/A/b.png");
	EXPECT_EQ(JoinMountPath("Engine", ""), "/Engine");
	EXPECT_EQ(JoinMountPath("Project", "Текстури/Камінь.png"), "/Project/Текстури/Камінь.png");

	for (const FStringView path : { "/Project/A/b.png", "/Engine", "/Plugin.X/a", "/Project/Ü/ß.png" })
	{
		const SSplitResult split = Split(path);
		ASSERT_TRUE(split.bOk) << path;
		EXPECT_EQ(JoinMountPath(split.MountName, split.Relative), path);
	}

	EXPECT_EQ(MakeChildMountPath("/Project", "A"), "/Project/A");
	EXPECT_EQ(MakeChildMountPath("/Project/A", "b.png"), "/Project/A/b.png");
	EXPECT_EQ(MakeChildMountPath("/Project/A", "B/c.png"), "/Project/A/B/c.png");
}

TEST(SharedTypesTest, WellFormedMountPathsHaveNoEmptyOrDotSegments)
{
	for (const FStringView path : { "/Project", "/Project/A", "/Project/A/b.png", "/Plugin.X/a.b/c..d/.e", "/Project/漢字/ї.png",
		"/Project/...", "/Project/a.", "/Project/ space " })
	{
		EXPECT_TRUE(IsWellFormedMountPath(path)) << path;
	}
	for (const FStringView path : { "", "/", "//", "Project", "Project/A", "/Project/", "/Project//A", "//Project", "/Project/A/",
		"/Project/./A", "/Project/../A", "/Project/A/.", "/Project/A/..", "/.", "/..", "/./A" })
	{
		EXPECT_FALSE(IsWellFormedMountPath(path)) << path;
	}
}

TEST(SharedTypesTest, ParentAndLeafWalkTheSegments)
{
	EXPECT_EQ(GetMountPathParent("/Project/A/b.png"), "/Project/A");
	EXPECT_EQ(GetMountPathParent("/Project/A"), "/Project");
	EXPECT_EQ(GetMountPathParent("/Project"), "");
	EXPECT_EQ(GetMountPathParent("/"), "");
	EXPECT_EQ(GetMountPathParent(""), "");
	EXPECT_EQ(GetMountPathParent("/Project/Текстури/Камінь.png"), "/Project/Текстури");

	EXPECT_EQ(GetMountPathLeaf("/Project/A/b.png"), "b.png");
	EXPECT_EQ(GetMountPathLeaf("/Project/A"), "A");
	EXPECT_EQ(GetMountPathLeaf("/Project"), "Project");
	EXPECT_EQ(GetMountPathLeaf("/Project/Текстури/Камінь 漢字.png"), "Камінь 漢字.png");
	// Parent + "/" + Leaf rebuilds every path below a mount root.
	for (const FStringView path : { "/Project/A/b.png", "/Project/A", "/Engine/Fonts/Roboto.ttf" })
	{
		EXPECT_EQ(MakeChildMountPath(GetMountPathParent(path), GetMountPathLeaf(path)), path);
	}
}

TEST(SharedTypesTest, MountPathHelpersTakeNoLabel)
{
	// A label may hold '/' (glTF names do) and '#', so the helpers would split inside it: unsupported input.
	const FStringView labelled = "/Project/Models/Box.gltf#mesh:Armature/Body";
	EXPECT_EQ(GetMountPathLeaf(labelled), "Body");
	EXPECT_EQ(GetMountPathParent(labelled), "/Project/Models/Box.gltf#mesh:Armature");

	// Callers split the label off at the end of the file's mount path, which they know from the record ('#'
	// is legal in file names, so searching for it would cut "a#b.png" short).
	const FStringView file = "/Project/Models/Box.gltf";
	ASSERT_TRUE(labelled.StartsWith(file));
	EXPECT_EQ(labelled.RightChop(file.Len() + 1u), "mesh:Armature/Body");
	EXPECT_EQ(GetMountPathLeaf(labelled.Left(file.Len())), "Box.gltf");
	EXPECT_EQ(GetMountPathParent(labelled.Left(file.Len())), "/Project/Models");
	EXPECT_EQ(GetMountPathLeaf("/Project/a#b.png"), "a#b.png");
}

TEST(SharedTypesTest, ResolveMountPathCollapsesRelativeReferences)
{
	const auto resolve = [] (FStringView Folder, FStringView Reference) -> FString
	{
		const std::optional<FString> path = ResolveMountPath(Folder, Reference);
		return path ? *path : FString("<none>");
	};

	EXPECT_EQ(resolve("/Project/Models", "Box.bin"), "/Project/Models/Box.bin");
	EXPECT_EQ(resolve("/Project/Models", "./Box.bin"), "/Project/Models/Box.bin");
	EXPECT_EQ(resolve("/Project/Models", "Textures/Rock.png"), "/Project/Models/Textures/Rock.png");
	EXPECT_EQ(resolve("/Project/Models", "../Textures/Rock.png"), "/Project/Textures/Rock.png");
	EXPECT_EQ(resolve("/Project/Models/Box", "../../Rock.png"), "/Project/Rock.png");
	EXPECT_EQ(resolve("/Project/Models", "A/../B/./c.png"), "/Project/Models/B/c.png");
	EXPECT_EQ(resolve("/Project/Models", "A//B/c.png"), "/Project/Models/A/B/c.png");
	EXPECT_EQ(resolve("/Project/Models", "Textures\\Rock.png"), "/Project/Models/Textures/Rock.png");
	EXPECT_EQ(resolve("/Project/Models", "..\\Rock.png"), "/Project/Rock.png");
	EXPECT_EQ(resolve("/Project/Models", "Текстури/Камінь 漢字.png"), "/Project/Models/Текстури/Камінь 漢字.png");
	EXPECT_EQ(resolve("/Project", "Rock.png"), "/Project/Rock.png");
	EXPECT_EQ(resolve("/Project/Models", "Textures/"), "/Project/Models/Textures");
	EXPECT_EQ(resolve("/Project/Models", "."), "/Project/Models");
	EXPECT_EQ(resolve("/Project/Models", ".."), "/Project");
	EXPECT_EQ(resolve("/Project/Models", "...png"), "/Project/Models/...png");

	// A leading separator makes the reference absolute: its first segment is the mount name.
	EXPECT_EQ(resolve("/Project/Models", "/Engine/Textures/White.png"), "/Engine/Textures/White.png");
	EXPECT_EQ(resolve("/Project/Models", "\\Engine\\Textures\\White.png"), "/Engine/Textures/White.png");
	EXPECT_EQ(resolve("/Project/Models", "/Project/A/../B.png"), "/Project/B.png");
	EXPECT_EQ(resolve("/Project/Models", "/Project"), "/Project");
	EXPECT_EQ(resolve("/Project/Models", "/Project/"), "/Project");

	// Never above the mount root, never a malformed result.
	for (const auto& [folder, reference] : std::initializer_list<std::pair<FStringView, FStringView>> {
		{ "/Project", ".." }, { "/Project", "../Engine/x.png" }, { "/Project/Models", "../../x.png" },
		{ "/Project/Models", "A/../../../x.png" }, { "/Project/Models", "/Project/../Engine/x.png" },
		{ "/Project/Models", "" }, { "/Project/Models", "/" }, { "/Project/Models", "//Engine/x" },
		{ "/Project/Models", "/../x" }, { "/Project/Models", "/./x" }, { "Project/Models", "x.png" },
		{ "/Project/", "x.png" }, { "", "x.png" } })
	{
		EXPECT_EQ(resolve(folder, reference), "<none>") << folder << " + " << reference;
	}

	for (const FStringView reference : { "a", "a/b", "../a", "./a/./b/..", "/Engine/a/b", "\\x\\y" })
	{
		const std::optional<FString> path = ResolveMountPath("/Project/Models", reference);
		ASSERT_TRUE(path.has_value()) << reference;
		EXPECT_TRUE(IsWellFormedMountPath(*path)) << *path;
	}
}


// ---------------- Provisional ids ----------------

TEST(SharedTypesTest, ProvisionalGuidIsTheDerivedPathGuid)
{
	const FStringView path = "/Project/Textures/Rock.png";
	EXPECT_EQ(MakeProvisionalGuid(path), MakeDerivedGuid(SGuid(), "path", path));
	EXPECT_EQ(MakeProvisionalGuid(path).ToString(), "85828d1c8b9786f5894c92299e9e25c5");
	EXPECT_EQ(MakeProvisionalGuid(path).GetVersion(), 8u);

	// The spelling on disk is the input: a PATH001 pair gets two ids.
	EXPECT_NE(MakeProvisionalGuid("/Project/Textures/rock.png"), MakeProvisionalGuid(path));
	EXPECT_NE(MakeProvisionalGuid("/Engine/Textures/Rock.png"), MakeProvisionalGuid(path));
}


// ---------------- Asset types ----------------

TEST(SharedTypesTest, AssetTypeIdsAreFrozen)
{
	const std::span<const SAssetTypeInfo> types = CAssetTypeRegistry::GetTypes();
	ASSERT_EQ(types.size(), 8u);
	const FStringView names[] = { "frt.Unknown", "frt.Raw", "frt.Texture", "frt.Model", "frt.Mesh", "frt.Material",
		"frt.InputActionMap", "frt.InputAction" };
	const FStringView shortNames[] = { "Unknown", "Raw", "Texture", "Model", "Mesh", "Material", "InputActionMap",
		"InputAction" };
	for (uint32 i = 0u; i < types.size(); ++i)
	{
		EXPECT_EQ(types[i].Name, names[i]);
		EXPECT_EQ(types[i].ShortName, shortNames[i]);
		EXPECT_EQ(&CAssetTypeRegistry::Get(static_cast<reflect::TypeId>(i)), &types[i]);
	}
	EXPECT_EQ(asset_types::Texture, 2u);
	EXPECT_EQ(asset_types::InputAction, 7u);

	// An id outside the table is frt.Unknown.
	EXPECT_EQ(CAssetTypeRegistry::Get(reflect::InvalidTypeId).Name, "frt.Unknown");
	EXPECT_EQ(CAssetTypeRegistry::Get(8u).Name, "frt.Unknown");
}

TEST(SharedTypesTest, AssetTypesHaveTheirM3TagKeys)
{
	const auto keyNames = [] (reflect::TypeId Type)
	{
		std::vector<FString> names;
		for (const SStaticTagKey& key : CAssetTypeRegistry::Get(Type).Keys)
		{
			names.emplace_back(key.Name);
		}
		return names;
	};
	EXPECT_EQ(keyNames(asset_types::Texture), (std::vector<FString> { "Width", "Height", "Channels", "HasAlpha" }));
	EXPECT_EQ(keyNames(asset_types::Model),
		(std::vector<FString> { "MeshCount", "MaterialCount", "ImageCount", "VertexCount", "TriangleCount" }));
	EXPECT_EQ(keyNames(asset_types::Mesh), (std::vector<FString> { "VertexCount", "TriangleCount" }));
	EXPECT_EQ(keyNames(asset_types::InputActionMap), (std::vector<FString> { "Actions" }));
	EXPECT_EQ(keyNames(asset_types::InputAction), (std::vector<FString> { "ValueType" }));
	EXPECT_TRUE(keyNames(asset_types::Unknown).empty());
	EXPECT_TRUE(keyNames(asset_types::Raw).empty());
	EXPECT_TRUE(keyNames(asset_types::Material).empty());

	const SAssetTypeInfo& texture = CAssetTypeRegistry::Get(asset_types::Texture);
	EXPECT_EQ(texture.Keys[0].Type, ETagType::Int64);
	EXPECT_EQ(texture.Keys[3].Type, ETagType::Bool);
	const SStaticTagKey& actions = CAssetTypeRegistry::Get(asset_types::InputActionMap).Keys[0];
	EXPECT_EQ(actions.Type, ETagType::Name);
	EXPECT_TRUE(HasAnyFlags(actions.Flags, ETagFlags::MultiValue));
	EXPECT_EQ(CAssetTypeRegistry::Get(asset_types::InputAction).Keys[0].Type, ETagType::Name);

	EXPECT_EQ(CAssetTypeRegistry::FindKey(asset_types::Texture, "height"), 1u);
	EXPECT_EQ(CAssetTypeRegistry::FindKey(asset_types::Model, "TriangleCount"), 4u);
	EXPECT_FALSE(CAssetTypeRegistry::FindKey(asset_types::Texture, "MeanColor").has_value());
	EXPECT_FALSE(CAssetTypeRegistry::FindKey(asset_types::Material, "Width").has_value());
}

TEST(SharedTypesTest, AssetTypeLookupTakesDottedThenShortNamesUnderTheFold)
{
	EXPECT_EQ(CAssetTypeRegistry::Find("frt.Texture"), asset_types::Texture);
	EXPECT_EQ(CAssetTypeRegistry::Find("FRT.TEXTURE"), asset_types::Texture);
	EXPECT_EQ(CAssetTypeRegistry::Find("Texture"), asset_types::Texture);
	EXPECT_EQ(CAssetTypeRegistry::Find("texture"), asset_types::Texture);
	EXPECT_EQ(CAssetTypeRegistry::Find("inputactionmap"), asset_types::InputActionMap);
	EXPECT_EQ(CAssetTypeRegistry::Find("frt.InputAction"), asset_types::InputAction);
	EXPECT_EQ(CAssetTypeRegistry::Find("frt.unknown"), asset_types::Unknown);

	EXPECT_FALSE(CAssetTypeRegistry::Find("").has_value());
	EXPECT_FALSE(CAssetTypeRegistry::Find("frt.").has_value());
	EXPECT_FALSE(CAssetTypeRegistry::Find("Textures").has_value());
	EXPECT_FALSE(CAssetTypeRegistry::Find("game.Texture").has_value());
	EXPECT_FALSE(CAssetTypeRegistry::Find("frt.Texture.Psd").has_value());
}


// ---------------- Importers ----------------

TEST(SharedTypesTest, ImportersClaimTheirExtensionsUnderTheFold)
{
	EXPECT_EQ(ImporterNameForFile("Rock.png"), "Texture");
	EXPECT_EQ(ImporterNameForFile("Rock.PNG"), "Texture");
	EXPECT_EQ(ImporterNameForFile("Камінь.JpEg"), "Texture");
	EXPECT_EQ(ImporterNameForFile("a.jpg"), "Texture");
	EXPECT_EQ(ImporterNameForFile("a.tga"), "Texture");
	EXPECT_EQ(ImporterNameForFile("a.bmp"), "Texture");
	EXPECT_EQ(ImporterNameForFile("Sponza.gltf"), "Model.Gltf");
	EXPECT_EQ(ImporterNameForFile("Sponza.GLB"), "Model.Gltf");
	EXPECT_EQ(ImporterNameForFile("Brick.frtmat.yml"), "Material.Legacy");
	EXPECT_EQ(ImporterNameForFile("Brick.FrtMat.YML"), "Material.Legacy");
	EXPECT_EQ(ImporterNameForFile("Player.frtinputmap"), "Input.Legacy");
	EXPECT_EQ(ImporterNameForFile("Jump.frtinputaction"), "Input.Legacy");
	// A path works as well as a name: only its end is read.
	EXPECT_EQ(ImporterNameForFile("/Project/Textures/Rock.png"), "Texture");

	// Nothing claims these.
	for (const FStringView name : { "readme.md", "a.yml", "Assets.yml", "a.png.txt", "png", "a.frtmat", "a.frtmat.yaml",
		"a.gltf.bak", "" })
	{
		EXPECT_EQ(ImporterNameForFile(name), "<none>") << name;
	}
}

TEST(SharedTypesTest, ImporterClaimsMatchTheWholeLongestExtension)
{
	// The longest claimed extension the name ends with decides, and only the end counts.
	EXPECT_EQ(ImporterNameForFile("a.png.frtmat.yml"), "Material.Legacy");
	EXPECT_EQ(ImporterNameForFile("a.frtmat.yml.png"), "Texture");
	EXPECT_EQ(ImporterNameForFile("a.frtinputmap.gltf"), "Model.Gltf");

	// Something must precede the extension: a dot-name claims nothing (and is a built-in exclude anyway).
	EXPECT_EQ(ImporterNameForFile(".png"), "<none>");
	EXPECT_EQ(ImporterNameForFile(".frtmat.yml"), "<none>");   // the whole name is the extension
	EXPECT_EQ(ImporterNameForFile("x.frtmat.yml"), "Material.Legacy");
	EXPECT_EQ(ImporterNameForFile("/Project/.png"), "<none>");

	// The main type follows the extension where the importer has more than one.
	const SImporterDesc* input = FindImporter("Input.Legacy");
	ASSERT_NE(input, nullptr);
	EXPECT_EQ(input->GetMainType("Player.frtinputmap"), asset_types::InputActionMap);
	EXPECT_EQ(input->GetMainType("Jump.FRTINPUTACTION"), asset_types::InputAction);
	const SImporterDesc* texture = FindImporter("Texture");
	ASSERT_NE(texture, nullptr);
	EXPECT_EQ(texture->GetMainType("Rock.png"), asset_types::Texture);
	EXPECT_EQ(FindImporter("Model.Gltf")->GetMainType("Box.glb"), asset_types::Model);
	EXPECT_EQ(FindImporter("Material.Legacy")->GetMainType("Brick.frtmat.yml"), asset_types::Material);
	EXPECT_EQ(FindImporter("Raw")->GetMainType("Roboto.ttf"), asset_types::Raw);
}

TEST(SharedTypesTest, UnavailableImportersAreKnownButClaimNothing)
{
	for (const FStringView name : { "Document", "Texture.Psd", "Texture.Krita", "Model.Blender", "DataTable.Csv" })
	{
		const SImporterDesc* importer = FindImporter(name);
		ASSERT_NE(importer, nullptr) << name;
		EXPECT_FALSE(importer->bAvailable) << name;
	}
	EXPECT_EQ(ImporterNameForFile("Level.frt"), "<none>");
	EXPECT_EQ(ImporterNameForFile("Rock.psd"), "<none>");
	EXPECT_EQ(ImporterNameForFile("Rock.kra"), "<none>");
	EXPECT_EQ(ImporterNameForFile("Rock.ora"), "<none>");
	EXPECT_EQ(ImporterNameForFile("Ship.blend"), "<none>");
	EXPECT_EQ(ImporterNameForFile("Ammo.csv"), "<none>");

	// The later importers share the settings of the ones they extend (§3.5.2).
	EXPECT_EQ(FindImporter("Texture.Psd")->Settings, FindImporter("Texture")->Settings);
	EXPECT_EQ(FindImporter("Texture.Krita")->Settings, FindImporter("Texture")->Settings);
	EXPECT_EQ(FindImporter("Model.Blender")->Settings, FindImporter("Model.Gltf")->Settings);
	EXPECT_TRUE(FindImporter("DataTable.Csv")->Extensions.empty());
	EXPECT_TRUE(FindImporter("Raw")->Extensions.empty());
	EXPECT_TRUE(FindImporter("Raw")->bAvailable);
}

TEST(SharedTypesTest, ImporterNamesAreFoldedAndTheTableIsConsistent)
{
	EXPECT_EQ(FindImporter("texture"), FindImporter("Texture"));
	EXPECT_EQ(FindImporter("MODEL.GLTF"), FindImporter("Model.Gltf"));
	EXPECT_EQ(FindImporter("texture.psd"), FindImporter("Texture.Psd"));
	EXPECT_EQ(FindImporter("Gltf"), nullptr);
	EXPECT_EQ(FindImporter(""), nullptr);
	EXPECT_EQ(FindImporter("Material"), nullptr);   // the M7 builder, not an importer

	const std::span<const SImporterDesc> importers = GetImporters();
	EXPECT_EQ(importers.size(), 10u);
	std::set<SGuid> versions;
	std::set<FString, SStringLessIgnoreCase> names;
	bool bSeenUnavailable = false;
	for (const SImporterDesc& importer : importers)
	{
		EXPECT_TRUE(importer.AnalyzeVersion.IsValid()) << importer.Name;
		EXPECT_EQ(importer.AnalyzeVersion.GetVersion(), 4u) << importer.Name;
		EXPECT_TRUE(versions.insert(importer.AnalyzeVersion).second) << importer.Name;
		EXPECT_TRUE(names.insert(FString(importer.Name)).second) << importer.Name;
		for (const FStringLiteral extension : importer.Extensions)
		{
			EXPECT_TRUE(extension.StartsWith('.') && extension.Len() > 1u) << extension;
			EXPECT_EQ(extension, FString(extension).FoldCase()) << extension;
		}
		// Available importers come first.
		EXPECT_FALSE(bSeenUnavailable && importer.bAvailable) << importer.Name;
		bSeenUnavailable = bSeenUnavailable || !importer.bAvailable;
	}
}

TEST(SharedTypesTest, SettingsTablesHaveTheirM3FieldsAndDefaults)
{
	const SSettingsTable* texture = FindImporter("Texture")->Settings;
	ASSERT_NE(texture, nullptr);
	EXPECT_EQ(texture->Name, "Texture");
	ASSERT_EQ(texture->Fields.size(), 4u);
	EXPECT_EQ(texture->Fields[0].Name, "ColorSpace");
	EXPECT_EQ(texture->Fields[0].Default, "Srgb");
	EXPECT_EQ(texture->Fields[0].EnumValues.size(), 3u);
	EXPECT_EQ(texture->Fields[1].Name, "Mips");
	EXPECT_EQ(texture->Fields[2].Name, "Compression");
	EXPECT_EQ(texture->Fields[3].Name, "MaxSize");
	EXPECT_EQ(texture->Fields[3].Kind, ETagType::Int64);
	EXPECT_EQ(texture->Fields[3].Default, "0");
	EXPECT_EQ(FindSettingsField(*texture, "colorspace"), 0);
	EXPECT_EQ(FindSettingsField(*texture, "MaxSize"), 3);
	EXPECT_EQ(FindSettingsField(*texture, "Hierarchy"), -1);

	const SSettingsTable* gltf = FindImporter("Model.Gltf")->Settings;
	ASSERT_NE(gltf, nullptr);
	EXPECT_EQ(gltf->Name, "Model.Gltf");
	ASSERT_EQ(gltf->Fields.size(), 3u);
	EXPECT_EQ(gltf->Fields[0].Name, "Hierarchy");
	EXPECT_EQ(gltf->Fields[0].Default, "Flatten");
	EXPECT_EQ(gltf->Fields[1].Name, "Tangents");
	EXPECT_EQ(gltf->Fields[1].Default, "Generate");
	EXPECT_EQ(gltf->Fields[2].Name, "ScaleFactor");
	EXPECT_EQ(gltf->Fields[2].Kind, ETagType::Double);
	EXPECT_EQ(gltf->Fields[2].Default, "1");

	// No field changes Analyze in M3, and an Enum field's default is one of its values.
	for (const SSettingsTable* table : { texture, gltf })
	{
		for (const SSettingsField& field : table->Fields)
		{
			EXPECT_FALSE(field.bAnalyzeRelevant) << field.Name;
			if (field.Kind == ETagType::Enum)
			{
				bool bDefaultListed = false;
				for (const FStringLiteral value : field.EnumValues)
				{
					bDefaultListed = bDefaultListed || value == field.Default;
				}
				EXPECT_TRUE(bDefaultListed) << field.Name;
			}
			else
			{
				EXPECT_TRUE(field.EnumValues.empty()) << field.Name;
			}
		}
	}
	EXPECT_EQ(FindImporter("Material.Legacy")->Settings, nullptr);
}


// ---------------- Analyze stamps ----------------

TEST(SharedTypesTest, AnalyzeStampIsXxh3OfTheVersionAndTheLittleEndianHash)
{
	const std::optional<SGuid> version = SGuid::Parse("8615e3393e8f45ea9aa1ba548b0997ef");
	ASSERT_TRUE(version.has_value());
	const uint64 analyzeHash = 0x0123456789ABCDEFull;

	uint8 bytes[24] = {};
	for (uint32 i = 0u; i < 16u; ++i)
	{
		bytes[i] = version->Bytes[i];
	}
	const uint8 hashBytes[8] = { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[16u + i] = hashBytes[i];
	}
	EXPECT_EQ(MakeAnalyzeStamp(*version, analyzeHash), hash::Xxh3_64(bytes));

	// Frozen: a change re-analyzes every file of every project.
	EXPECT_EQ(MakeAnalyzeStamp(*version, analyzeHash), 0x68A9C73F34B6EC88ull);
	EXPECT_EQ(MakeAnalyzeStamp(*version, 0u), 0x3B1FE50FDC88252Dull);
	EXPECT_EQ(MakeAnalyzeStamp(SGuid(), 0u), 0x743DF94EE4C78A2Aull);

	EXPECT_NE(MakeAnalyzeStamp(*version, 1u), MakeAnalyzeStamp(*version, 0u));
	EXPECT_EQ(FindImporter("Texture")->AnalyzeVersion, *version);
}


// ---------------- The rest of the vocabulary ----------------

TEST(SharedTypesTest, DiagnosticCodesSpellTheSpecsCodes)
{
	EXPECT_EQ(FormatDiagnosticCode(Path001), "PATH001");
	EXPECT_EQ(FormatDiagnosticCode(Path002), "PATH002");
	EXPECT_EQ(FormatDiagnosticCode(Path003), "PATH003");
	EXPECT_EQ(FormatDiagnosticCode(Rule001), "RULE001");
	EXPECT_EQ(FormatDiagnosticCode(Rule002), "RULE002");
	EXPECT_EQ(FormatDiagnosticCode(Ref006), "REF006");
	EXPECT_EQ(FormatDiagnosticCode(Ref007), "REF007");
}

TEST(SharedTypesTest, FlagEnumsCombineAndTest)
{
	const EDepFlags flags = EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared;
	EXPECT_TRUE(HasAnyFlags(flags, EDepFlags::Path));
	EXPECT_TRUE(HasAllFlags(flags, EDepFlags::Build | EDepFlags::Declared));
	EXPECT_FALSE(HasAllFlags(flags, EDepFlags::Build | EDepFlags::Hard));
	EXPECT_FALSE(HasAnyFlags(flags, EDepFlags::Hard | EDepFlags::Unresolved));
	EXPECT_FALSE(HasAnyFlags(flags, EDepFlags {}));
	EXPECT_EQ(static_cast<uint16>(flags & ~EDepFlags::Path), static_cast<uint16>(EDepFlags::Build | EDepFlags::Declared));

	ERecordFlags record {};
	record |= ERecordFlags::Primary;
	record |= ERecordFlags::LegacyReferences;
	EXPECT_EQ(static_cast<uint16>(record), 0x0101u);
	record &= ~ERecordFlags::Primary;
	EXPECT_EQ(record, ERecordFlags::LegacyReferences);

	// The spec's values (§5.2), which snapshots will store.
	EXPECT_EQ(static_cast<uint16>(ETagFlags::CookToRuntime), 2u);
	EXPECT_EQ(static_cast<uint16>(ERecordFlags::TypeExcluded), 1u << 10u);
	EXPECT_EQ(static_cast<uint16>(EDepFlags::Code), 1u << 9u);
}

TEST(SharedTypesTest, RecordIdsAndTagValuesHaveUsefulDefaults)
{
	const SRecordId none;
	EXPECT_FALSE(none.IsValid());
	const SRecordId sub { 0x80000005u, 3u };
	EXPECT_TRUE(sub.IsValid());
	EXPECT_TRUE(sub.IsSubEntry());
	EXPECT_FALSE((SRecordId { 5u, 3u }).IsSubEntry());
	EXPECT_NE(sub, (SRecordId { 0x80000005u, 4u }));

	EXPECT_EQ(STagValue::FromInt64(-3).AsInt64(), -3);
	EXPECT_EQ(STagValue::FromDouble(0.25).AsDouble(), 0.25);
	EXPECT_TRUE(STagValue::FromBool(true).AsBool());
	EXPECT_EQ(STagValue::FromName("Button").Type, ETagType::Name);
	EXPECT_EQ(STagValue::FromName("Button").Text, "Button");
	EXPECT_EQ(STagValue::FromString("Камінь").Type, ETagType::String);
	EXPECT_EQ(STagValue::FromInt64(7), STagValue::FromInt64(7));
	EXPECT_NE(STagValue::FromInt64(7), STagValue::FromDouble(7.0));

	// An untyped query literal: Compile types it per key; a quoted one stays text.
	const STagValue literal = STagValue::FromLiteral("4096", false);
	EXPECT_EQ(literal.Type, ETagType::None);
	EXPECT_TRUE(literal.IsLiteral());
	EXPECT_FALSE(literal.IsQuotedLiteral());
	EXPECT_TRUE(STagValue::FromLiteral("1", true).IsQuotedLiteral());
	EXPECT_TRUE(STagValue::FromLiteral("", true).IsLiteral());
	EXPECT_FALSE(STagValue().IsLiteral());
	EXPECT_FALSE(STagValue::FromName("Button").IsLiteral());
	EXPECT_NE(STagValue::FromLiteral("1", true), STagValue::FromLiteral("1", false));

	SRegistryDelta delta;
	EXPECT_TRUE(delta.IsEmpty());
	delta.ProblemClears.push_back(SProblemsClear { "/Project/A", EProblemSource::Scan });
	EXPECT_FALSE(delta.IsEmpty());
	delta.ProblemClears.clear();
	delta.FileUpserts.push_back(SFileUpsert { "/Project/Rock.png" });
	EXPECT_FALSE(delta.IsEmpty());
	EXPECT_EQ(SMountUpsert().Id, InvalidMountId);
	EXPECT_EQ(delta.FileUpserts[0].MetaState, EMetaState::PendingMeta);
	EXPECT_EQ(delta.FileUpserts[0].Class, EFileClass::Unclaimed);
	EXPECT_FALSE(delta.FileUpserts[0].ContentHash.has_value());

	const SEffectiveFileSettings effective;
	EXPECT_EQ(effective.Importer, nullptr);
	EXPECT_EQ(effective.CookRule, ECookRule::Default);
	const SScanBatch batch;
	EXPECT_TRUE(batch.Folders.empty());
	EXPECT_EQ(SScanFolder().Parent, ~0u);
	EXPECT_EQ(SScanFolder().Scope, nullptr);
}


namespace
{
// Every name round-trips through TryParse and is the C++ identifier; values are unique. Plain enums list
// every value from 0 in order; flag enums one single bit per entry.
template <typename E>
void ExpectNamesRoundTrip (bool bFlags, size_t ExpectedCount)
{
	const auto& entries = enum_::TEnumTraits<E>::Entries;
	EXPECT_EQ(entries.size(), ExpectedCount);
	std::set<FString> names;
	std::set<uint64> values;
	for (size_t i = 0u; i < entries.size(); ++i)
	{
		const auto& [value, name] = entries[i];
		const uint64 bits = static_cast<uint64>(value);
		EXPECT_FALSE(name.IsEmpty());
		EXPECT_TRUE(names.insert(FString(name)).second) << name;
		EXPECT_TRUE(values.insert(bits).second) << name;
		EXPECT_EQ(enum_::ToString(value), name);
		E parsed {};
		EXPECT_TRUE(enum_::TryParse(name, &parsed)) << name;
		EXPECT_EQ(parsed, value) << name;
		if (bFlags)
		{
			EXPECT_TRUE(bits != 0u && (bits & (bits - 1u)) == 0u) << name;
		}
		else
		{
			EXPECT_EQ(bits, i) << name;
		}
	}
}
}

TEST(SharedTypesTest, RegistryEnumsHaveTextNames)
{
	ExpectNamesRoundTrip<EFileClass>(false, 5u);
	ExpectNamesRoundTrip<EMetaState>(false, 5u);
	ExpectNamesRoundTrip<ETagType>(false, 9u);
	ExpectNamesRoundTrip<ECookRule>(false, 5u);
	ExpectNamesRoundTrip<EImportStatus>(false, 4u);
	ExpectNamesRoundTrip<ECompare>(false, 12u);
	ExpectNamesRoundTrip<EExists>(false, 3u);
	ExpectNamesRoundTrip<ERecordScope>(false, 4u);
	ExpectNamesRoundTrip<EProblemSource>(false, 5u);
	ExpectNamesRoundTrip<EScanFileState>(false, 4u);
	ExpectNamesRoundTrip<ERecordFlags>(true, 11u);
	ExpectNamesRoundTrip<ETagFlags>(true, 5u);
	ExpectNamesRoundTrip<EDepFlags>(true, 10u);

	// The spellings every piece prints and parses.
	EXPECT_EQ(enum_::ToString(ECookRule::Default), "Default");
	EXPECT_EQ(enum_::ToString(EImportStatus::AnalyzeFailed), "AnalyzeFailed");
	EXPECT_EQ(enum_::ToString(EFileClass::Unclaimed), "Unclaimed");
	EXPECT_EQ(enum_::ToString(ERecordFlags::LegacyReferences), "LegacyReferences");
	EXPECT_EQ(enum_::ToString(EDepFlags::Unresolved), "Unresolved");
	EXPECT_EQ(enum_::ToString(ECompare::Match), "Match");
	ECookRule rule = ECookRule::Always;
	EXPECT_FALSE(enum_::TryParse("Auto", &rule));   // the rules engine's alias, not a name
	EXPECT_EQ(rule, ECookRule::Always);
	EXPECT_TRUE(enum_::TryParse("editoronly", &rule));
	EXPECT_EQ(rule, ECookRule::EditorOnly);
	// A combination has no single name: flags print bit by bit.
	EXPECT_TRUE(enum_::ToString(EDepFlags::Build | EDepFlags::Path).IsEmpty());
}
