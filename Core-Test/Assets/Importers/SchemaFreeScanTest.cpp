// The schema-free scan of §5.5 step 4 (E3) over fixture trees: every `$asset` map becomes a Schemaless edge to
// SAssetId{GUID, SubId} - SubId from a `sub` label or native id, or a `row` key or its `id` pin - with the map's field
// path and the mount-path hint; a document with `$type: frt.DataTable` gets one sub-entry per row (pinned `$id`s
// honoured, bad keys, case-only duplicates and id collisions rejected), and references inside a row come from it.
#include <gtest/gtest.h>

#include "ImporterTestUtils.h"

#include "Reflect/DataTree.h"
#include "Reflect/DataTreeYaml.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
constexpr FStringLiteral HeroGuid = "3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b";
constexpr FStringLiteral AmmoGuid = "1b2c3d4e5f604182a3b4c5d6e7f80912";

SAnalyzeInput MakeInput ()
{
	SAnalyzeInput input;
	input.FileMountPath = "/Project/Data/Doc.frt";
	input.SourceFile = "Content/Data/Doc.frt";
	return input;
}

SAnalyzeResult Scan (FStringView Yaml)
{
	reflect::CDataTree tree;
	SDiagnostic error;
	EXPECT_TRUE(reflect::ParseYaml(Yaml, tree, &error)) << error.Message.CStr();
	SAnalyzeResult result;
	GatherSchemaFree(tree, MakeInput(), result);
	return result;
}

const SEdgeDecl* FindEdgeAt (const SAnalyzeResult& Result, FStringView FieldPath)
{
	for (const SEdgeDecl& edge : Result.Edges)
	{
		if (edge.FieldPath == FieldPath)
		{
			return &edge;
		}
	}
	return nullptr;
}

SGuid Guid (FStringView Text)
{
	return SGuid::Parse(Text).value_or(SGuid {});
}
}


TEST(SchemaFreeScanTest, AssetMapsBecomeSchemalessEdges)
{
	const SAnalyzeResult result = Scan(FString::Format(R"(Icon:   {{ $asset: 2d8f1e4a6b3c4d5e8f9a0b1c2d3e4f50, path: /Project/UI/Icons/rifle.png }}
Mesh:   {{ $asset: {0}, sub: "mesh:Body", path: /Project/Characters/Hero/Hero.gltf }}
Socket: {{ $asset: {0}, sub: $3f9c0a7e5d21b864 }}
Ammo:   {{ $asset: {1}, row: Rifle556, path: /Project/Data/Ammo.frt }}
Shell:  {{ $asset: {1}, row: Shotgun12ga, id: $4c1f9e27a05d3b68 }}
Decal:  null
Nested:
  List:
    - {{ $asset: {0} }}
    - {{ $asset: null }}
)", HeroGuid, AmmoGuid));
	EXPECT_TRUE(result.Diagnostics.empty()) << DescribeDiagnostics(result).CStr();
	ASSERT_EQ(result.Edges.size(), 6u);
	EXPECT_TRUE(result.SubEntries.empty());

	const SEdgeDecl* icon = FindEdgeAt(result, "Icon");
	ASSERT_NE(icon, nullptr);
	EXPECT_EQ(icon->Target.Guid, Guid("2d8f1e4a6b3c4d5e8f9a0b1c2d3e4f50"));
	EXPECT_EQ(icon->Target.SubId, 0u);
	EXPECT_EQ(icon->TargetPath, "/Project/UI/Icons/rifle.png");
	EXPECT_EQ(icon->Flags, EDepFlags::Schemaless);
	EXPECT_EQ(icon->FromSub, 0u);

	const SEdgeDecl* mesh = FindEdgeAt(result, "Mesh");
	ASSERT_NE(mesh, nullptr);
	EXPECT_EQ(mesh->Target.Guid, Guid(HeroGuid));
	EXPECT_EQ(mesh->Target.SubId, MakeSubId("mesh:Body"));

	const SEdgeDecl* socket = FindEdgeAt(result, "Socket");
	ASSERT_NE(socket, nullptr);
	EXPECT_EQ(socket->Target.SubId, 0x3f9c0a7e5d21b864ull);
	EXPECT_TRUE(socket->TargetPath.IsEmpty());

	const SEdgeDecl* ammo = FindEdgeAt(result, "Ammo");
	ASSERT_NE(ammo, nullptr);
	EXPECT_EQ(ammo->Target.SubId, MakeSubId("row:Rifle556"));
	EXPECT_EQ(ammo->Flags, EDepFlags::Schemaless | EDepFlags::Row);

	const SEdgeDecl* shell = FindEdgeAt(result, "Shell");
	ASSERT_NE(shell, nullptr);
	EXPECT_EQ(shell->Target.SubId, 0x4c1f9e27a05d3b68ull);
	EXPECT_EQ(shell->Flags, EDepFlags::Schemaless | EDepFlags::Row);

	EXPECT_NE(FindEdgeAt(result, "Nested.List.0"), nullptr);
}

TEST(SchemaFreeScanTest, MalformedReferencesAreWarningsWithPositions)
{
	const SAnalyzeResult result = Scan(FString::Format(R"(A: {{ $asset: not-a-guid }}
B: {{ $asset: 00000000000000000000000000000000 }}
C: {{ $asset: {0}, sub: Body }}
D: {{ $asset: {0}, sub: $12 }}
E: {{ $asset: {0}, row: X, id: 12 }}
F: {{ $asset: {0}, sub: "mesh:A", row: X }}
G: {{ $asset: {0}, path: Project/relative }}
H: {{ $asset: [ {0} ] }}
)", HeroGuid));
	// G keeps its edge without the hint; every other line is no reference.
	ASSERT_EQ(result.Edges.size(), 1u);
	EXPECT_EQ(result.Edges[0].FieldPath, "G");
	EXPECT_TRUE(result.Edges[0].TargetPath.IsEmpty());
	ASSERT_EQ(result.Diagnostics.size(), 8u) << DescribeDiagnostics(result).CStr();
	for (const SDiagnostic& diagnostic : result.Diagnostics)
	{
		EXPECT_EQ(diagnostic.Severity, ESeverity::Warning);
		EXPECT_EQ(diagnostic.Code, 0u);
		EXPECT_EQ(diagnostic.Subject, "/Project/Data/Doc.frt");
		EXPECT_EQ(diagnostic.File, "Content/Data/Doc.frt");
		EXPECT_NE(diagnostic.Line, 0u);
	}
	EXPECT_EQ(result.Diagnostics[0].Line, 1u);
	EXPECT_EQ(result.Diagnostics[0].FieldPath, "A");
	EXPECT_EQ(result.Diagnostics[7].FieldPath, "H");
}

TEST(SchemaFreeScanTest, TableRowsBecomeSubEntries)
{
	const SAnalyzeResult result = Scan(FString::Format(R"($type: frt.DataTable
$rowType: Game.AmmoRow
$rowVersion: 2
Defaults:
  Penetration: 0.25
  Icon: {{ $asset: {0} }}
Rows:
  Pistol9mm:
    Damage: 18
  Rifle556:
    Damage: 24
    Tracer: {{ $asset: 3e4f5a6b7c8d4e0f9a2b3c4d5e6f7081, path: /Project/FX/Tracer.frt }}
  Shotgun12ga:
    $id: $4c1f9e27a05d3b68
    Damage: 9
  Empty: null
)", HeroGuid));
	EXPECT_TRUE(result.Diagnostics.empty()) << DescribeDiagnostics(result).CStr();
	ASSERT_EQ(result.SubEntries.size(), 4u);
	EXPECT_EQ(result.SubEntries[0].Label, "Pistol9mm");
	EXPECT_EQ(result.SubEntries[0].SubId, MakeSubId("row:Pistol9mm"));
	EXPECT_EQ(result.SubEntries[0].Type, asset_types::Unknown);
	EXPECT_EQ(result.SubEntries[0].Flags, ERecordFlags::Schemaless);
	EXPECT_EQ(result.SubEntries[2].Label, "Shotgun12ga");
	EXPECT_EQ(result.SubEntries[2].SubId, 0x4c1f9e27a05d3b68ull);
	EXPECT_EQ(result.SubEntries[2].Flags, ERecordFlags::Schemaless | ERecordFlags::PinnedId);
	EXPECT_EQ(result.SubEntries[3].Label, "Empty");

	ASSERT_EQ(result.Edges.size(), 2u);
	const SEdgeDecl* defaults = FindEdgeAt(result, "Defaults.Icon");
	ASSERT_NE(defaults, nullptr);
	EXPECT_EQ(defaults->FromSub, 0u);
	const SEdgeDecl* tracer = FindEdgeAt(result, "Rows.Rifle556.Tracer");
	ASSERT_NE(tracer, nullptr);
	EXPECT_EQ(tracer->FromSub, MakeSubId("row:Rifle556"));
	EXPECT_EQ(tracer->TargetPath, "/Project/FX/Tracer.frt");
}

TEST(SchemaFreeScanTest, BadRowsAreErrorsAndGetNoSubEntry)
{
	const SAnalyzeResult result = Scan(FString::Format(R"($type: frt.DataTable
Rows:
  Good:
    X: 1
  good:
    Ref: {{ $asset: {0} }}
  9Lives:
    X: 1
  Pinned:
    $id: $12
  Buckshot:
    X: 1
  Renamed:
    $id: ${1:016x}
)", HeroGuid, MakeSubId("row:Buckshot")));
	// Only "Good" and "Buckshot" register: a case-only duplicate, a bad key, a malformed pin, and a pin that
	// collides with another row's id are errors.
	ASSERT_EQ(result.SubEntries.size(), 2u);
	EXPECT_EQ(result.SubEntries[0].Label, "Good");
	EXPECT_EQ(result.SubEntries[1].Label, "Buckshot");
	ASSERT_EQ(CountDiagnostics(result, ESeverity::Error), 4u) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Diagnostics.size(), 4u);
	EXPECT_EQ(result.Diagnostics[0].Line, 5u);
	EXPECT_EQ(result.Diagnostics[0].FieldPath, "Rows.good");
	// The reference inside the rejected row comes from the record.
	ASSERT_EQ(result.Edges.size(), 1u);
	EXPECT_EQ(result.Edges[0].FromSub, 0u);
	EXPECT_EQ(result.Edges[0].FieldPath, "Rows.good.Ref");
}

TEST(SchemaFreeScanTest, OnlyTablesHaveRows)
{
	// "Rows" in a document of another type is an ordinary field.
	const SAnalyzeResult result = Scan("$type: Game.WeaponData\nRows:\n  A: 1\n");
	EXPECT_TRUE(result.SubEntries.empty());
	EXPECT_TRUE(result.Diagnostics.empty());

	SAnalyzeResult empty;
	GatherSchemaFree(reflect::CDataTree(), MakeInput(), empty);
	EXPECT_TRUE(empty.Edges.empty());
}
