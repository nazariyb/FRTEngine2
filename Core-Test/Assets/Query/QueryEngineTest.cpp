// The query engine (§5.6) over a real registry store: type, folder, label, tag, text, graph and `is:` terms, the
// system columns, literals typed per key, AnyOf groups, the Records scopes (rows vs imported sub-assets),
// deterministic order with OrderBy, Limit and Offset, positioned compile errors, a query refused by a view of
// another revision, equal answers whatever order the content was committed in, and a seeded fuzz run: random
// queries over the fixture's names never crash, and every error from text has a position.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <random>
#include <thread>

#include "Assets/Registry/RegistryTables.h"
#include "QueryTestFixture.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::query_test;


namespace
{
class QueryEngineTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		CommitFixture(Store);
	}

	const CRegistryView& View () const { return Store.Live(); }

	CRegistryStore Store;
};

const std::vector<FString> AllTextures = List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png",
	"/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" });
}


TEST_F(QueryEngineTest, TypeTermsSelectTheRecordsOfThatTypeInPathOrder)
{
	EXPECT_EQ(Query(View(), "type:Texture"), AllTextures);
	EXPECT_EQ(Query(View(), "t:frt.Texture"), AllTextures);
	EXPECT_EQ(Query(View(), "type=frt.texture"), AllTextures);
	EXPECT_EQ(Query(View(), "type:InputAction"), List({ "/Project/Input/IA_Jump.frtinputaction", "/Project/Input/IA_Move.frtinputaction" }));
	EXPECT_EQ(Query(View(), "type:Raw"), List({ "/Engine/Fonts/Roboto.ttf", "/Project/Misc/A/Readme.txt", "/Project/Misc/B/Readme.txt" }));
	// Two type terms OR together.
	EXPECT_EQ(Query(View(), "type:InputActionMap type:InputAction").size(), 3u);
}

TEST_F(QueryEngineTest, AnEmptyQueryReturnsEveryAssetRecord)
{
	EXPECT_EQ(Query(View(), "").size(), View().GetRecordCount());
	SAssetQuery all;
	all.Scope(ERecordScope::All);
	EXPECT_EQ(Query(View(), all).size(), View().GetRecordCount() + View().GetSubEntryCount());
}

TEST_F(QueryEngineTest, LiteralsAreTypedByTheKey)
{
	EXPECT_EQ(Query(View(), "Width>=2048"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "Width:1024"), List({ "/Project/Textures/T_Rock.png" }));
	EXPECT_EQ(Query(View(), "Width=1024 Height=1024"), List({ "/Project/Textures/T_Rock.png" }));
	EXPECT_EQ(Query(View(), "Width!=4096 Width>600"), List({ "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "Width<1024"), List({ "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "Width<=1024"), List({ "/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Rock.png" }));
	EXPECT_EQ(Query(View(), "HasAlpha=true"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "HasAlpha:FALSE type:Texture").size(), 3u);
	// A fractional literal compares an Int64 key as a number, not by truncation.
	EXPECT_EQ(Query(View(), "Width>2047.5"), Query(View(), "Width>=2048"));
	EXPECT_EQ(Query(View(), "Width in (1024, 2048)"), List({ "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "Width in (512, 2048.0)"), List({ "/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Sand.png" }));

	// Built in code, with typed values.
	SAssetQuery built;
	built.OfType("Texture").Where("Width", ECompare::Ge, STagValue::FromInt64(4096)).Where("Channels", ECompare::Eq, STagValue::FromDouble(4.0));
	EXPECT_EQ(Query(View(), built), List({ "/Project/Textures/Hero/T_Hero.png" }));
	SAssetQuery exists;
	exists.Where("Width", ECompare::Exists);
	EXPECT_EQ(Query(View(), exists), AllTextures);
	SAssetQuery missing;
	missing.OfType("Model").Where("ImageCount", ECompare::Missing);
	EXPECT_EQ(Query(View(), missing), List({ "/Project/Models/SM_Rock.gltf" }));
}

TEST_F(QueryEngineTest, LiteralsThatDoNotFitTheKeyAreErrorsAtTheirTerm)
{
	EXPECT_EQ(QueryError(View(), "Width>='4096'").Column, 1u);
	EXPECT_EQ(QueryError(View(), "type:Texture Width>=big").Column, 14u);
	EXPECT_EQ(QueryError(View(), "HasAlpha=yes").Column, 1u);
	EXPECT_EQ(QueryError(View(), "HasAlpha>true").Column, 1u);
	EXPECT_EQ(QueryError(View(), "Width:Rif").Column, 1u);

	SAssetQuery mismatch;
	mismatch.Where("Width", ECompare::Eq, STagValue::FromName("wide"));
	SCompiledQuery compiled;
	SDiagnostic error;
	EXPECT_FALSE(CompileQuery(View(), mismatch, compiled, &error));
	EXPECT_EQ(error.Column, 0u);
	EXPECT_FALSE(compiled.IsValid());
	SAssetQuery prefix;
	prefix.Where("Width", ECompare::Prefix, STagValue::FromInt64(1));
	EXPECT_FALSE(CompileQuery(View(), prefix, compiled, &error));
	SAssetQuery noValue;
	noValue.Where("Width", ECompare::Eq);
	EXPECT_FALSE(CompileQuery(View(), noValue, compiled, &error));
}

TEST_F(QueryEngineTest, UnknownNamesAreErrorsAtTheirColumn)
{
	EXPECT_EQ(QueryError(View(), "type:Bogus").Column, 1u);
	EXPECT_EQ(QueryError(View(), "Bogus=1").Column, 1u);
	// With an explicit type, a key that type does not declare is an error (Actions belongs to InputActionMap).
	const SDiagnostic scoped = QueryError(View(), "type:Texture Actions:Jump");
	EXPECT_EQ(scoped.Column, 14u);
	EXPECT_TRUE(scoped.Message.View().Contains("frt.Texture")) << scoped.Message.CStr();
	EXPECT_EQ(QueryError(View(), "dir:/Project/Nowhere").Column, 1u);
	EXPECT_EQ(QueryError(View(), "rock $Bogus>1").Column, 6u);
	EXPECT_EQ(QueryError(View(), "ref:/Project/Textures/T_Nope.png").Column, 1u);
	EXPECT_EQ(QueryError(View(), "type:Texture dep:Nope").Column, 14u);
	EXPECT_EQ(QueryError(View(), "type:Texture order:Actions").Column, 14u);
	EXPECT_EQ(QueryError(View(), "table:Nope").Column, 1u);
	// Parse errors come through with their own position.
	EXPECT_EQ(QueryError(View(), "type:Texture (").Column, 14u);
}

TEST_F(QueryEngineTest, ABareKeyMatchesEveryTypeInScopeThatDeclaresIt)
{
	// VertexCount: frt.Model and frt.Mesh. Meshes are sub-entries, so the default scope keeps the model only...
	EXPECT_EQ(Query(View(), "VertexCount>500"), List({ "/Project/Models/SM_Rock.gltf" }));
	// ...until a type term names them.
	EXPECT_EQ(Query(View(), "type:Mesh VertexCount>500"), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	EXPECT_EQ(Query(View(), "type:Mesh"), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	SAssetQuery all;
	all.Scope(ERecordScope::All).Where("VertexCount", ECompare::Gt, STagValue::FromInt64(2000));
	EXPECT_EQ(Query(View(), all), List({ "/Project/Models/SM_Rock.gltf", "/Project/Models/SM_Rock.gltf#mesh:Rock" }));
}

TEST_F(QueryEngineTest, NamingAFileTypeKeepsItsImportedSubAssetsOut)
{
	// The model embeds an image (frt.Texture) and a material (frt.Material): types that are files of their own, so
	// naming them lists the files only, as $Type does.
	EXPECT_EQ(Query(View(), "type:Texture"), AllTextures);
	EXPECT_EQ(Query(View(), "type:Texture"), Query(View(), "$Type=Texture"));
	EXPECT_EQ(Query(View(), "type:Material"), List({ "/Project/Materials/M_Hero.frtmat.yml", "/Project/Materials/M_Rock.frtmat.yml" }));
	EXPECT_EQ(Query(View(), "type:Texture is:unused"), List({ "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "type:Texture -label:Hero"), List({ "/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "type:Texture Width=2048"), List({ "/Project/Textures/T_Sand.png" }));
	// A sub-asset-only type still brings its sub-assets in.
	EXPECT_EQ(Query(View(), "type:Mesh"), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	EXPECT_EQ(Query(View(), "type:Mesh or type:Material"), List({ "/Project/Materials/M_Hero.frtmat.yml", "/Project/Materials/M_Rock.frtmat.yml",
		"/Project/Models/SM_Rock.gltf#mesh:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	// The other scopes reach the embedded ones.
	SAssetQuery images;
	images.Scope(ERecordScope::SubAssets).OfType("Texture");
	EXPECT_EQ(Query(View(), images), List({ "/Project/Models/SM_Rock.gltf#image:Rock_BaseColor" }));
	SAssetQuery everything;
	everything.Scope(ERecordScope::All).OfType("Material");
	EXPECT_EQ(Query(View(), everything), List({ "/Project/Materials/M_Hero.frtmat.yml", "/Project/Materials/M_Rock.frtmat.yml",
		"/Project/Models/SM_Rock.gltf#material:Rock" }));
}

TEST_F(QueryEngineTest, ASubAssetIsAdmittedByTheAlternativeThatNamesItsType)
{
	// LOD1 (1000 vertices) matches only the folder alternative, which names no type: it stays out.
	EXPECT_EQ(Query(View(), "(type:Mesh VertexCount>2000) or dir:/Project/Models"), List({ "/Project/Models/SM_Rock.gltf",
		"/Project/Models/SM_Rock.gltf#mesh:Rock" }));
	EXPECT_EQ(Query(View(), "dir:/Project/Models (type:Mesh VertexCount>2000 or label:Environment)"), List({ "/Project/Models/SM_Rock.gltf",
		"/Project/Models/SM_Rock.gltf#mesh:Rock" }));
	// Named at the root, the type admits its sub-assets for every alternative.
	EXPECT_EQ(Query(View(), "type:Mesh (VertexCount>2000 or TriangleCount<500)"), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock",
		"/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	// Nested: an alternative inside an alternative admits.
	EXPECT_EQ(Query(View(), "dir:/Project/Models (label:Environment ((type:Mesh TriangleCount<500) or is:primary) or is:legacy-ref)"),
		List({ "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
}

TEST_F(QueryEngineTest, NameTagsCompareUnderThePortableFold)
{
	EXPECT_EQ(Query(View(), "ValueType=button"), List({ "/Project/Input/IA_Jump.frtinputaction" }));
	EXPECT_EQ(Query(View(), "ValueType:AX"), List({ "/Project/Input/IA_Move.frtinputaction" }));
	EXPECT_EQ(Query(View(), "ValueType in (Axis, Button)").size(), 2u);
	// MultiValue: any value matches; Ne means no value equals.
	EXPECT_EQ(Query(View(), "Actions:Mo"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	EXPECT_EQ(Query(View(), "Actions=jump"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	EXPECT_TRUE(Query(View(), "Actions!=Jump").empty());
	EXPECT_EQ(Query(View(), "Actions!=Fire"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	SAssetQuery contains;
	contains.Where("Actions", ECompare::Contains, STagValue::FromName("OV"));
	EXPECT_EQ(Query(View(), contains), List({ "/Project/Input/IMC_Default.frtinputmap" }));
}

TEST_F(QueryEngineTest, TextTermsSearchNamesAndPathsCaselessly)
{
	EXPECT_EQ(Query(View(), "rock"), List({ "/Project/Materials/M_Rock.frtmat.yml", "/Project/Models/SM_Rock.gltf", "/Project/Textures/T_Rock.png" }));
	EXPECT_EQ(Query(View(), "HERO"), List({ "/Project/Materials/M_Hero.frtmat.yml", "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	EXPECT_EQ(Query(View(), "\"textures/t_r\""), List({ "/Project/Textures/T_Rock.png" }));
	EXPECT_EQ(Query(View(), "rock gltf"), List({ "/Project/Models/SM_Rock.gltf" }));
	// A sub-entry also matches by its label.
	SAssetQuery subs;
	subs.Scope(ERecordScope::SubAssets).Text("lod1");
	EXPECT_EQ(Query(View(), subs), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
}

TEST_F(QueryEngineTest, FoldersUseTheirPreorderInterval)
{
	EXPECT_EQ(Query(View(), "dir:/Project/Textures"), AllTextures);
	EXPECT_EQ(Query(View(), "dir=/Project/Textures"), List({ "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "dir:/project/TEXTURES/hero"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	EXPECT_EQ(Query(View(), "dir:/Engine").size(), 1u);
	EXPECT_EQ(Query(View(), "dir:/Project").size(), View().GetRecordCount() - 1u);
	// Two dir: terms OR together.
	EXPECT_EQ(Query(View(), "dir:/Project/Input dir:/Project/Data").size(), 4u);
	// A folder added mid-mount renumbers the intervals; the query still sees exactly its subtree.
	SRegistryDelta later;
	AddTexture(later, "/Project/Textures/Hero/Alt/T_Hero_Alt.png", 64, 64, 3, {});
	AddTexture(later, "/Project/Aaa/T_First.png", 64, 64, 3, {});
	Store.Commit(std::move(later));
	EXPECT_EQ(Query(View(), "dir:/Project/Textures/Hero"), List({ "/Project/Textures/Hero/Alt/T_Hero_Alt.png", "/Project/Textures/Hero/T_Hero.png",
		"/Project/Textures/Hero/T_Hero_N.png" }));
}

TEST_F(QueryEngineTest, LabelsMatchUnderTheFoldAndCanBeExcluded)
{
	EXPECT_EQ(Query(View(), "label:hero"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	EXPECT_EQ(Query(View(), "type:Texture -label:Hero"), List({ "/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "label:Environment -label:Hero").size(), 3u);
	EXPECT_TRUE(Query(View(), "label:NoSuchLabel").empty());
	// Sub-entries carry their file's labels.
	EXPECT_EQ(Query(View(), "type:Mesh label:environment").size(), 2u);
}

TEST_F(QueryEngineTest, GraphTermsFollowResolvedEdges)
{
	// Referencers.
	EXPECT_EQ(Query(View(), "ref:/Project/Textures/T_Rock.png"), List({ "/Project/Materials/M_Rock.frtmat.yml" }));
	EXPECT_EQ(Query(View(), "ref:T_Rock"), List({ "/Project/Materials/M_Rock.frtmat.yml" }));
	// Transitive over files: M_Rock, the model whose material sub-entry uses it, and M_Hero, which uses the model's mesh.
	EXPECT_EQ(Query(View(), "ref:~/Project/Textures/T_Rock.png"), List({ "/Project/Materials/M_Hero.frtmat.yml", "/Project/Materials/M_Rock.frtmat.yml",
		"/Project/Models/SM_Rock.gltf" }));
	SAssetQuery transitiveAll;
	transitiveAll.Scope(ERecordScope::All).References("/Project/Textures/T_Rock.png", true);
	EXPECT_EQ(Query(View(), transitiveAll), List({ "/Project/Materials/M_Hero.frtmat.yml", "/Project/Materials/M_Rock.frtmat.yml", "/Project/Models/SM_Rock.gltf",
		"/Project/Models/SM_Rock.gltf#image:Rock_BaseColor", "/Project/Models/SM_Rock.gltf#material:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock",
		"/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	// A direct referencer that is a sub-entry: the sub-entry that declared the edge, and its file's record.
	SAssetQuery direct;
	direct.Scope(ERecordScope::All).References("/Project/Materials/M_Rock.frtmat.yml");
	EXPECT_EQ(Query(View(), direct), List({ "/Project/Models/SM_Rock.gltf", "/Project/Models/SM_Rock.gltf#material:Rock" }));
	// An edge to a sub-entry: it references that sub-entry and its file.
	EXPECT_EQ(Query(View(), "ref:/Project/Models/SM_Rock.gltf#mesh:Rock"), List({ "/Project/Materials/M_Hero.frtmat.yml" }));
	EXPECT_EQ(Query(View(), "ref:/Project/Models/SM_Rock.gltf"), List({ "/Project/Materials/M_Hero.frtmat.yml" }));
	EXPECT_TRUE(Query(View(), "ref:/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1").empty());

	// Dependencies: unresolved edges and companion files are no records.
	EXPECT_EQ(Query(View(), "dep:/Project/Materials/M_Hero.frtmat.yml"), List({ "/Project/Models/SM_Rock.gltf", "/Project/Textures/Hero/T_Hero.png",
		"/Project/Textures/Hero/T_Hero_N.png" }));
	SAssetQuery depsAll;
	depsAll.Scope(ERecordScope::All).ReferencedBy("/Project/Materials/M_Hero.frtmat.yml");
	EXPECT_EQ(Query(View(), depsAll), List({ "/Project/Models/SM_Rock.gltf", "/Project/Models/SM_Rock.gltf#mesh:Rock",
		"/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	EXPECT_EQ(Query(View(), "dep:~SM_Rock"), List({ "/Project/Materials/M_Rock.frtmat.yml", "/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "dep:IMC_Default"), List({ "/Project/Input/IA_Jump.frtinputaction", "/Project/Input/IA_Move.frtinputaction" }));
	EXPECT_EQ(Query(View(), "dep:IMC_Default ValueType=Axis"), List({ "/Project/Input/IA_Move.frtinputaction" }));
	// Masks: every bit must be on the edge.
	SAssetQuery hard;
	hard.References("/Project/Textures/T_Rock.png", false, EDepFlags::Hard);
	EXPECT_TRUE(Query(View(), hard).empty());
	SAssetQuery build;
	build.ReferencedBy("/Project/Materials/M_Hero.frtmat.yml", false, EDepFlags::Build | EDepFlags::Path);
	EXPECT_EQ(Query(View(), build).size(), 2u);
	// By id.
	SAssetQuery byId;
	byId.References(SAssetId { MakeProvisionalGuid("/Project/Textures/T_Sand.png"), 0u });
	EXPECT_EQ(Query(View(), byId), List({ "/Project/Materials/M_Rock.frtmat.yml" }));
}

TEST_F(QueryEngineTest, NamedTargetsMustBeUnique)
{
	const SDiagnostic ambiguous = QueryError(View(), "type:Material ref:Readme");
	EXPECT_EQ(ambiguous.Column, 15u);
	EXPECT_TRUE(ambiguous.Message.View().Contains("/Project/Misc/A/Readme.txt")) << ambiguous.Message.CStr();
	EXPECT_EQ(QueryError(View(), "ref:Nope").Column, 1u);
	// The exact spelling first, else the fold.
	EXPECT_EQ(Query(View(), "ref:t_rock"), List({ "/Project/Materials/M_Rock.frtmat.yml" }));
	// A row key names its row.
	EXPECT_TRUE(Query(View(), "ref:Rifle556").empty());
}

TEST(QueryEngineCaseTest, ATargetThatMatchesTwoFilesUnderTheFoldIsPath001)
{
	CRegistryStore store;
	SRegistryDelta delta;
	delta.Mounts.push_back(SMountUpsert { "Project", true, ProjectMountId });
	AddTexture(delta, "/Project/Case/Rock.png", 64, 64, 3, {});
	AddTexture(delta, "/Project/Case/rock.png", 128, 128, 3, {});
	store.Commit(std::move(delta));
	const SDiagnostic error = QueryError(store.Live(), "ref:/Project/Case/ROCK.png");
	EXPECT_EQ(error.Column, 1u);
	EXPECT_TRUE(error.Message.View().Contains("ambiguous")) << error.Message.CStr();
	// Both records come back, in fold-then-bytes order.
	EXPECT_EQ(Query(store.Live(), "type:Texture"), List({ "/Project/Case/Rock.png", "/Project/Case/rock.png" }));
	EXPECT_EQ(Query(store.Live(), "dir:/project/case").size(), 2u);
}

TEST_F(QueryEngineTest, SystemPredicatesReadFlagsEdgesAndProblems)
{
	EXPECT_EQ(Query(View(), "is:broken-ref"), List({ "/Project/Materials/M_Hero.frtmat.yml" }));
	EXPECT_EQ(Query(View(), "is:legacy-ref"), List({ "/Project/Materials/M_Rock.frtmat.yml" }));
	EXPECT_EQ(Query(View(), "is:primary"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	EXPECT_EQ(Query(View(), "is:problem"), List({ "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "is:excluded"), List({ "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "is:stale-tags"), List({ "/Project/Data/DT_Ammo.frt" }));
	EXPECT_EQ(Query(View(), "type:Texture is:unused"), List({ "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "type:Mesh is:unused"), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	// The predicates without data yet compile, match nothing and say why.
	for (const FStringView text : { FStringView("is:redirected"), FStringView("is:unsaved"), FStringView("type:Texture is:pending-meta") })
	{
		const std::optional<SAssetQuery> parsed = SAssetQuery::Parse(text);
		ASSERT_TRUE(parsed.has_value());
		SCompiledQuery compiled;
		ASSERT_TRUE(CompileQuery(View(), *parsed, compiled));
		ASSERT_EQ(compiled.Notes.size(), 1u);
		EXPECT_EQ(compiled.Notes[0].Severity, ESeverity::Info);
		EXPECT_GE(compiled.Notes[0].Column, 1u);
		std::vector<SRecordId> records;
		EXPECT_EQ(RunQuery(View(), compiled, records), 0u);
	}
	// Inside an alternative, only that alternative matches nothing.
	EXPECT_EQ(Query(View(), "is:redirected or is:primary"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	// The text entry point hands the notes on (the search box shows them).
	std::vector<SRecordId> records;
	std::vector<SDiagnostic> notes(3u);
	EXPECT_EQ(RunQueryText(View(), "is:primary or is:redirected", records, nullptr, nullptr, &notes), 1u);
	ASSERT_EQ(notes.size(), 1u);
	EXPECT_EQ(notes[0].Severity, ESeverity::Info);
	EXPECT_EQ(notes[0].Column, 15u);
	EXPECT_TRUE(notes[0].Message.View().Contains("is:redirected")) << notes[0].Message.CStr();
	EXPECT_EQ(RunQueryText(View(), "is:primary", records, nullptr, nullptr, &notes), 1u);
	EXPECT_TRUE(notes.empty());
}

TEST_F(QueryEngineTest, SystemColumnsCompareLikeTags)
{
	EXPECT_EQ(Query(View(), "$Size>=8000 type:Texture"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	EXPECT_EQ(Query(View(), "$Type=Texture"), AllTextures);
	EXPECT_EQ(Query(View(), "$Type in (InputAction, frt.InputActionMap)").size(), 3u);
	EXPECT_EQ(QueryError(View(), "$Type:Tex").Column, 1u);
	SAssetQuery typePrefix;
	typePrefix.Where("$Type", ECompare::Prefix, STagValue::FromName("FRT.Input"));
	EXPECT_EQ(Query(View(), typePrefix).size(), 3u);
	EXPECT_EQ(Query(View(), "$name:t_h"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	EXPECT_EQ(Query(View(), "$Path:/Project/Input/IA"), List({ "/Project/Input/IA_Jump.frtinputaction", "/Project/Input/IA_Move.frtinputaction" }));
	EXPECT_EQ(Query(View(), "$Folder=/project/textures/hero").size(), 2u);
	EXPECT_EQ(Query(View(), "$Labels=hero").size(), 2u);
	EXPECT_EQ(Query(View(), "$ImportStatus=AnalyzeFailed"), List({ "/Project/Misc/A/Readme.txt" }));
	EXPECT_EQ(QueryError(View(), "$ImportStatus=Broken").Column, 1u);
	EXPECT_EQ(Query(View(), "$CookRule=Never"), List({ "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "$CookRule=Auto type:Texture").size(), 4u);
	EXPECT_EQ(Query(View(), "$Chunk=3"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	EXPECT_EQ(Query(View(), "$PrimaryName=imc_default"), List({ "/Project/Input/IMC_Default.frtinputmap" }));
	EXPECT_EQ(Query(View(), "$Problems>0"), List({ "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "type:Texture $Referencers>=1"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png",
		"/Project/Textures/T_Rock.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "$Dependencies>=4"), List({ "/Project/Materials/M_Hero.frtmat.yml" }));
	SAssetQuery primary;
	primary.Where("$PrimaryName", ECompare::Exists);
	EXPECT_EQ(Query(View(), primary).size(), 1u);
}

TEST_F(QueryEngineTest, RecordScopesTellRowsFromImportedSubAssets)
{
	EXPECT_EQ(Query(View(), "rows"), List({ "/Project/Data/DT_Ammo.frt#Pistol9mm", "/Project/Data/DT_Ammo.frt#Rifle556" }));
	EXPECT_EQ(Query(View(), "table:DT_Ammo"), List({ "/Project/Data/DT_Ammo.frt#Pistol9mm", "/Project/Data/DT_Ammo.frt#Rifle556" }));
	EXPECT_EQ(Query(View(), "table:/Project/Data/DT_Ammo.frt rif"), List({ "/Project/Data/DT_Ammo.frt#Rifle556" }));
	EXPECT_TRUE(Query(View(), "table:SM_Rock").empty());
	SAssetQuery subAssets;
	subAssets.Scope(ERecordScope::SubAssets);
	EXPECT_EQ(Query(View(), subAssets), List({ "/Project/Models/SM_Rock.gltf#image:Rock_BaseColor", "/Project/Models/SM_Rock.gltf#material:Rock",
		"/Project/Models/SM_Rock.gltf#mesh:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
	EXPECT_EQ(Query(View(), "rows $Name:p"), List({ "/Project/Data/DT_Ammo.frt#Pistol9mm" }));
	// Rows are found by their table's folder and file.
	EXPECT_EQ(Query(View(), "rows dir:/Project/Data").size(), 2u);
}

TEST_F(QueryEngineTest, AnyOfGroupsOrTheirAlternatives)
{
	EXPECT_EQ(Query(View(), "type:Texture (Width>=4096 or HasAlpha=true)"), List({ "/Project/Textures/Hero/T_Hero.png",
		"/Project/Textures/Hero/T_Hero_N.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "ValueType=Axis or MeshCount>=2"), List({ "/Project/Input/IA_Move.frtinputaction", "/Project/Models/SM_Rock.gltf" }));
	EXPECT_EQ(Query(View(), "(dir:/Project/Input or dir:/Project/Models) (is:primary or gltf)"), List({ "/Project/Input/IMC_Default.frtinputmap",
		"/Project/Models/SM_Rock.gltf" }));
	// A sub-asset type named inside an alternative brings its sub-assets in.
	EXPECT_EQ(Query(View(), "type:Mesh or type:Model").size(), 3u);
	SAssetQuery built;
	built.AnyOf({ SAssetQuery().OfType("Raw").Text("readme"), SAssetQuery().WithLabel("Hero") });
	EXPECT_EQ(Query(View(), built).size(), 4u);
}

TEST_F(QueryEngineTest, OrderLimitAndOffsetShapeADeterministicResult)
{
	EXPECT_EQ(Query(View(), "type:Texture order:Width desc"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png",
		"/Project/Textures/T_Sand.png", "/Project/Textures/T_Rock.png", "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "type:Texture order:Width"), List({ "/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Rock.png",
		"/Project/Textures/T_Sand.png", "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png" }));
	// Records without the key come last in both directions, in path order.
	const std::vector<FString> ascending = Query(View(), "dir:/Project/Textures dir:/Project/Input order:Width");
	ASSERT_EQ(ascending.size(), 8u);
	EXPECT_EQ(ascending[0], "/Project/Textures/Old/T_Old.png");
	EXPECT_EQ(ascending[5], "/Project/Input/IA_Jump.frtinputaction");
	EXPECT_EQ(ascending[7], "/Project/Input/IMC_Default.frtinputmap");
	const std::vector<FString> descending = Query(View(), "dir:/Project/Textures dir:/Project/Input order:Width desc");
	EXPECT_EQ(descending[0], "/Project/Textures/Hero/T_Hero.png");
	EXPECT_EQ(descending[5], "/Project/Input/IA_Jump.frtinputaction");
	// Text order, system columns.
	EXPECT_EQ(Query(View(), "type:InputAction order:ValueType"), List({ "/Project/Input/IA_Move.frtinputaction", "/Project/Input/IA_Jump.frtinputaction" }));
	EXPECT_EQ(Query(View(), "type:Texture order:$Size limit:2"), List({ "/Project/Textures/Old/T_Old.png", "/Project/Textures/T_Rock.png" }));
	EXPECT_EQ(Query(View(), "type:Texture order:$Name limit:1"), List({ "/Project/Textures/Hero/T_Hero.png" }));
	EXPECT_EQ(Query(View(), "type:Texture order:$Name desc limit:1"), List({ "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(Query(View(), "type:Texture order:$Path desc limit:1"), List({ "/Project/Textures/T_Sand.png" }));

	// Limit and Offset cut the sorted list; Run returns the full count.
	SAssetQuery page;
	page.OfType("Texture").OrderBy("Width", false).Limit(2u, 1u);
	SCompiledQuery compiled;
	ASSERT_TRUE(CompileQuery(View(), page, compiled));
	std::vector<SRecordId> records;
	SQueryStats stats;
	EXPECT_EQ(RunQuery(View(), compiled, records, &stats), 5u);
	EXPECT_EQ(PathsOf(View(), records), List({ "/Project/Textures/Hero/T_Hero_N.png", "/Project/Textures/T_Sand.png" }));
	EXPECT_EQ(stats.Matched, 5u);
	EXPECT_EQ(stats.Returned, 2u);
	EXPECT_FALSE(stats.bWrongView);
	SAssetQuery past;
	past.OfType("Texture").Limit(10u, 7u);
	EXPECT_TRUE(Query(View(), past).empty());
	// Sub-entries follow their file's record, by label.
	SAssetQuery all;
	all.Scope(ERecordScope::All).InFolder("/Project/Models");
	EXPECT_EQ(Query(View(), all), List({ "/Project/Models/SM_Rock.gltf", "/Project/Models/SM_Rock.gltf#image:Rock_BaseColor",
		"/Project/Models/SM_Rock.gltf#material:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock", "/Project/Models/SM_Rock.gltf#mesh:Rock_LOD1" }));
}

TEST_F(QueryEngineTest, IdsRestrictTheResult)
{
	SAssetQuery ids;
	ids.WithId(SAssetId { MakeProvisionalGuid("/Project/Textures/T_Rock.png"), 0u })
		.WithId(SAssetId { MakeProvisionalGuid("/Project/Input/IA_Jump.frtinputaction"), 0u })
		.WithId(SAssetId { MakeProvisionalGuid("/Project/Gone.png"), 0u });
	EXPECT_EQ(Query(View(), ids), List({ "/Project/Input/IA_Jump.frtinputaction", "/Project/Textures/T_Rock.png" }));
	ids.OfType("Texture");
	EXPECT_EQ(Query(View(), ids), List({ "/Project/Textures/T_Rock.png" }));
	SAssetQuery sub;
	sub.Scope(ERecordScope::All).WithId(SAssetId { MakeProvisionalGuid("/Project/Models/SM_Rock.gltf"), MakeSubId("mesh:Rock") });
	EXPECT_EQ(Query(View(), sub), List({ "/Project/Models/SM_Rock.gltf#mesh:Rock" }));
}

TEST_F(QueryEngineTest, RemovedAndRetypedRecordsLeaveTheResult)
{
	SRegistryDelta change;
	change.FileRemoves.push_back("/Project/Textures/T_Rock.png");
	SRecordUpsert retyped = MakeRecord("/Project/Textures/T_Sand.png", asset_types::Raw, "Raw");
	change.RecordUpserts.push_back(std::move(retyped));
	Store.Commit(std::move(change));
	EXPECT_EQ(Query(View(), "type:Texture"), List({ "/Project/Textures/Hero/T_Hero.png", "/Project/Textures/Hero/T_Hero_N.png", "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "Width<4096"), List({ "/Project/Textures/Old/T_Old.png" }));
	EXPECT_EQ(Query(View(), "ref:T_Sand"), List({ "/Project/Materials/M_Rock.frtmat.yml" }));
	EXPECT_EQ(Query(View(), "dep:M_Rock"), List({ "/Project/Textures/T_Sand.png" }));
}

TEST_F(QueryEngineTest, AQueryCompiledAgainstAnotherRevisionIsRefused)
{
	SCompiledQuery compiled;
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("type:Texture"), compiled));
	EXPECT_EQ(compiled.Revision, View().GetRevision());
	const std::shared_ptr<const CRegistryView> before = Store.Acquire();
	SRegistryDelta later;
	AddTexture(later, "/Project/Textures/T_New.png", 64, 64, 3, {});
	Store.Commit(std::move(later));
	std::vector<SRecordId> records;
	SQueryStats stats;
	EXPECT_EQ(RunQuery(View(), compiled, records, &stats), 0u);
	EXPECT_TRUE(stats.bWrongView);
	EXPECT_TRUE(records.empty());
	// The view it was compiled against still answers, with its own records.
	EXPECT_EQ(RunQuery(*before, compiled, records), 5u);
	EXPECT_TRUE(before->IsValid(records.front()));
	EXPECT_FALSE(View().IsValid(records.front()));
	// An uncompiled query is refused too.
	EXPECT_EQ(RunQuery(View(), SCompiledQuery {}, records, &stats), 0u);
	EXPECT_TRUE(stats.bWrongView);
}

TEST_F(QueryEngineTest, AQueryCompiledAgainstAnotherStoreIsRefused)
{
	SCompiledQuery compiled;
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("dir:/Project/Textures/Hero ref:~T_Rock or type:Texture"), compiled));
	std::vector<SRecordId> records;
	SQueryStats stats;
	ASSERT_EQ(RunQuery(View(), compiled, records, &stats), 5u);
	// The same content committed the same way: the same revision, other folder rows and record ids.
	CRegistryStore twin;
	CommitFixture(twin, true);
	ASSERT_EQ(twin.Live().GetRevision(), View().GetRevision());
	EXPECT_EQ(RunQuery(twin.Live(), compiled, records, &stats), 0u);
	EXPECT_TRUE(stats.bWrongView);
	EXPECT_TRUE(records.empty());
	// A store rebuilt from this one's image at this revision (a reload).
	CRegistryStore reloaded;
	const MountId mounts[] = { EngineMountId, ProjectMountId };
	reloaded.AdoptImage(View().ExportImage(mounts), View().GetRevision());
	ASSERT_EQ(reloaded.Live().GetRevision(), View().GetRevision());
	EXPECT_EQ(RunQuery(reloaded.Live(), compiled, records, &stats), 0u);
	EXPECT_TRUE(stats.bWrongView);
	// Compiled there, it answers there as here.
	ASSERT_TRUE(CompileQuery(reloaded.Live(), *SAssetQuery::Parse("dir:/Project/Textures/Hero ref:~T_Rock or type:Texture"), compiled));
	RunQuery(reloaded.Live(), compiled, records);
	EXPECT_EQ(PathsOf(reloaded.Live(), records), AllTextures);
	// An acquired view of the compiling store at the same revision shares its binding.
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("type:Texture"), compiled));
	EXPECT_EQ(RunQuery(*Store.Acquire(), compiled, records), 5u);
}

TEST(QueryEngineOrderTest, TheAnswerDoesNotDependOnCommitOrder)
{
	CRegistryStore forward;
	CommitFixture(forward, false);
	CRegistryStore reverse;
	CommitFixture(reverse, true);
	const FStringView queries[] = { "", "type:Texture order:Width desc", "rock", "ref:~T_Rock", "dep:~SM_Rock", "is:unused", "order:$Name",
		"type:Mesh or type:Model", "rows", "label:environment order:$Size desc limit:3", "$Referencers>=1 order:$Referencers desc" };
	for (const FStringView text : queries)
	{
		EXPECT_EQ(Query(forward.Live(), text), Query(reverse.Live(), text)) << text.GetData();
	}
	SAssetQuery all;
	all.Scope(ERecordScope::All).OrderBy("$Type");
	EXPECT_EQ(Query(forward.Live(), all), Query(reverse.Live(), all));
}

namespace
{
// The documented result order without an order key, from the record paths alone ("file#label"): the file path
// folded, then its bytes, then the asset record before its sub-entries, then the label folded, then its bytes.
bool PathOrderLess (const FString& A, const FString& B)
{
	const auto split = [] (const FString& Path, FStringView& OutFile, FStringView& OutLabel)
	{
		const FStringView path = Path.View();
		const uint32 hash = path.Find('#');
		OutFile = hash == FStringView::NotFound ? path : path.Left(hash);
		OutLabel = hash == FStringView::NotFound ? FStringView() : path.Mid(hash + 1u);
	};
	FStringView fileA, labelA, fileB, labelB;
	split(A, fileA, labelA);
	split(B, fileB, labelB);
	if (const int32 folded = FString(fileA).FoldCase().View().Compare(FString(fileB).FoldCase().View()); folded != 0)
	{
		return folded < 0;
	}
	if (const int32 bytes = fileA.Compare(fileB); bytes != 0)
	{
		return bytes < 0;
	}
	if (labelA.IsEmpty() != labelB.IsEmpty())
	{
		return labelA.IsEmpty();
	}
	if (const int32 folded = FString(labelA).FoldCase().View().Compare(FString(labelB).FoldCase().View()); folded != 0)
	{
		return folded < 0;
	}
	return labelA.Compare(labelB) < 0;
}
}

TEST(QueryEngineOrderTest, ThePathOrderIsTheFoldThenTheBytesWhateverTheShapeOfThePaths)
{
	// Long shared prefixes, a folder whose name is a prefix of a sibling's, files beside subfolders, names that
	// differ in case only (the case-sensitive mount keeps them apart) and a model with many sub-entries: enough
	// hits for the sort's radix levels, with every tie it has to hand back to the string compare.
	CRegistryStore store;
	SRegistryDelta delta;
	delta.Mounts.push_back(SMountUpsert { "Engine", false, EngineMountId });
	delta.Mounts.push_back(SMountUpsert { "Project", true, ProjectMountId });
	const FStringView folders[] = { "/Project/Textures/Environment/Rocks/Set0", "/Project/Textures/Environment/Rocks/Set0/Detail",
		"/Project/Textures/Environment/Rocks/Set1", "/Project/Textures/Environment/Rocks-Old", "/Project/Textures/Environment/Rocks",
		"/Project/Textures/ENVIRONMENT/Rocks/Set0", "/Project/Textures/environment/rocks/set0", "/Project/Tex", "/Engine/Textures" };
	const FStringView stems[] = { "T_Rock_Detail_", "T_Rock_detail_", "t_rock_detail_", "T_Rock_Detail", "T_Rock_Detail_Long_Shared_Prefix_", "T_RockDetail_",
		"T_Rock_Detail-" };
	std::mt19937 random(20261026u);
	std::vector<FString> added;
	for (uint32 i = 0u; i < 400u; ++i)
	{
		const FStringView folder = folders[random() % std::size(folders)];
		const FStringView stem = stems[random() % std::size(stems)];
		const uint32 number = random() % 3u == 0u ? random() % 12u : random() % 2000u;
		FString path = FString::Format("{}/{}{}.png", folder, stem, number);
		if (std::find(added.begin(), added.end(), path) != added.end())
		{
			continue;
		}
		AddTexture(delta, path.View(), 64, 64, 3, {});
		added.push_back(std::move(path));
	}
	delta.FileUpserts.push_back(MakeFile("/Project/Textures/Environment/Rocks/SM_Rock.gltf", 100u));
	SRecordUpsert model = MakeRecord("/Project/Textures/Environment/Rocks/SM_Rock.gltf", asset_types::Model, "Model.Gltf");
	for (uint32 i = 0u; i < 40u; ++i)
	{
		const FString label = FString::Format("{}:Rock_Part_With_A_Long_Name_{}", i % 2u == 0u ? "mesh" : "material", i * 37u % 1000u);
		model.SubEntries.push_back(SSubEntryDecl { label, 0u, i % 2u == 0u ? asset_types::Mesh : asset_types::Material, ERecordFlags::None, {} });
	}
	delta.RecordUpserts.push_back(std::move(model));
	store.Commit(std::move(delta));

	SAssetQuery all;
	all.Scope(ERecordScope::All);
	SAssetQuery subs;
	subs.Scope(ERecordScope::SubAssets);
	for (const std::vector<FString>& paths : { Query(store.Live(), all), Query(store.Live(), "type:Texture"), Query(store.Live(), "dir:/Project/Textures/Environment"),
		Query(store.Live(), subs) })
	{
		ASSERT_GT(paths.size(), 12u);
		std::vector<FString> expected = paths;
		std::sort(expected.begin(), expected.end(), PathOrderLess);
		EXPECT_EQ(paths, expected);
	}
	EXPECT_EQ(Query(store.Live(), all).size(), added.size() + 41u);
}

namespace
{
// Concurrency step i: texture i arrives (Width from i), the material M_Gen points at the last three textures and
// T_Rock, and every fifth step removes the texture of four steps before.
FString GenTexturePath (uint32 Step)
{
	return FString::Format("/Project/Gen/{}/T_Gen_{}.png", Step % 8u, Step);
}

SRegistryDelta MakeQueryStep (uint32 Step)
{
	SRegistryDelta delta;
	AddTexture(delta, GenTexturePath(Step).View(), static_cast<int64>(Step * 37u % 4096u + 1u), 64, Step % 2u == 0u ? 4 : 3,
		{ Step % 3u == 0u ? FStringView("Environment") : FStringView("Gen") }, 100u + Step);
	if (Step == 1u)
	{
		delta.FileUpserts.push_back(MakeFile("/Project/Materials/M_Gen.frtmat.yml", 100u));
	}
	SRecordUpsert material = MakeRecord("/Project/Materials/M_Gen.frtmat.yml", asset_types::Material, "Material.Legacy");
	for (uint32 k = Step >= 3u ? Step - 2u : 1u; k <= Step; ++k)
	{
		material.Edges.push_back(PathEdge(GenTexturePath(k).View(), EDepFlags::Build | EDepFlags::Path));
	}
	material.Edges.push_back(PathEdge("/Project/Textures/T_Rock.png", EDepFlags::Build | EDepFlags::Path));
	delta.RecordUpserts.push_back(std::move(material));
	if (Step % 5u == 0u && Step > 4u)
	{
		delta.FileRemoves.push_back(GenTexturePath(Step - 4u));
	}
	return delta;
}

constexpr FStringView ConcurrentQueries[] = { "ref:~/Project/Textures/T_Rock.png", "dep:M_Gen", "type:Texture order:Width desc limit:7", "dir:/Project/Gen",
	"rows", "is:unused", "gen or type:Mesh", "label:Environment order:$Size desc", "type:Texture Width>2000 -label:Gen" };

// One hash per query of what it returns on View (paths in order, or the error).
std::vector<uint64> AnswersOf (const CRegistryView& View)
{
	std::vector<uint64> answers;
	std::vector<SRecordId> records;
	for (const FStringView text : ConcurrentQueries)
	{
		SDiagnostic error;
		const std::optional<uint32> count = RunQueryText(View, text, records, &error);
		FString joined = count ? FString::Format("{}:", *count) : FString::Format("error {}: {}", error.Column, error.Message);
		for (const SRecordId record : records)
		{
			SAssetData data;
			joined += View.GetRecord(record, data) ? data.MountPath.View() : FStringView("<invalid>");
			joined += '\n';
		}
		answers.push_back(joined.View().GetHash());
	}
	return answers;
}
}

TEST(QueryConcurrencyTest, QueriesOnOtherThreadsAnswerAsTheOwnerThreadDoesForTheirView)
{
	// Compile and run are pure reads of an acquired view (§5.9): four readers query while the owner commits, and
	// every answer equals the one a single thread gets from a store at that revision.
	constexpr uint32 Steps = FRT_BUILD_DEBUG ? 100u : 300u;
	CRegistryStore reference;
	CommitFixture(reference);
	const uint64 baseRevision = reference.GetRevision();
	std::vector<std::vector<uint64>> expected;
	expected.push_back(AnswersOf(reference.Live()));
	for (uint32 step = 1u; step <= Steps; ++step)
	{
		reference.Commit(MakeQueryStep(step));
		ASSERT_EQ(reference.GetRevision(), baseRevision + step);
		expected.push_back(AnswersOf(reference.Live()));
	}

	CRegistryStore store;
	CommitFixture(store);
	ASSERT_EQ(store.GetRevision(), baseRevision);
	std::atomic<bool> bDone = false;
	std::atomic<uint32> failures = 0u;
	std::atomic<uint32> checkedViews = 0u;
	std::vector<std::thread> readers;
	for (uint32 t = 0u; t < 4u; ++t)
	{
		readers.emplace_back([&] ()
		{
			bool bLast = false;
			while (!bLast)
			{
				bLast = bDone.load(std::memory_order_acquire);   // one more view after the last commit
				const std::shared_ptr<const CRegistryView> view = store.Acquire();
				const uint64 step = view->GetRevision() - baseRevision;
				if (step >= expected.size() || AnswersOf(*view) != expected[static_cast<size_t>(step)])
				{
					failures.fetch_add(1u);
				}
				checkedViews.fetch_add(1u);
			}
		});
	}
	for (uint32 step = 1u; step <= Steps; ++step)
	{
		store.Commit(MakeQueryStep(step));
	}
	bDone.store(true, std::memory_order_release);
	for (std::thread& reader : readers)
	{
		reader.join();
	}
	EXPECT_EQ(failures.load(), 0u);
	EXPECT_GE(checkedViews.load(), 4u);
	std::printf("[query] 4 readers checked %u views over %u commits\n", checkedViews.load(), Steps);
	EXPECT_EQ(AnswersOf(store.Live()), expected.back());
}

TEST_F(QueryEngineTest, ColumnValuesReadSystemColumnsAndTags)
{
	const SRecordId hero = View().FindByPath("/Project/Textures/Hero/T_Hero.png").value_or(SRecordId {});
	std::vector<STagValue> values;
	const auto read = [&] (FStringView Key)
	{
		values.clear();
		ForEachColumnValue(View(), hero, Key, [&] (const STagValue& Value) { values.push_back(Value); });
		return values;
	};
	ASSERT_EQ(read("$Size").size(), 1u);
	EXPECT_EQ(values[0], STagValue::FromInt64(9000));
	EXPECT_EQ(read("$Type")[0], STagValue::FromName("frt.Texture"));
	EXPECT_EQ(read("$Folder")[0], STagValue::FromName("/Project/Textures/Hero"));
	EXPECT_EQ(read("$Labels")[0], STagValue::FromName("Hero"));
	EXPECT_EQ(read("width")[0], STagValue::FromInt64(4096));
	EXPECT_EQ(read("$CookRule")[0], STagValue::FromName("Default"));
	EXPECT_EQ(read("$Referencers")[0], STagValue::FromInt64(1));
	EXPECT_TRUE(read("$PrimaryName").empty());
	EXPECT_TRUE(read("$Nope").empty());
	const SRecordId mesh = View().FindByPath("/Project/Models/SM_Rock.gltf#mesh:Rock").value_or(SRecordId {});
	values.clear();
	ForEachColumnValue(View(), mesh, "$Path", [&] (const STagValue& Value) { values.push_back(Value); });
	ASSERT_EQ(values.size(), 1u);
	EXPECT_EQ(values[0].Text, "/Project/Models/SM_Rock.gltf#mesh:Rock");
}

TEST_F(QueryEngineTest, TheCompiledPlanDescribesTypedTerms)
{
	SCompiledQuery compiled;
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("type:Texture Width>=4096 order:Width desc limit:5"), compiled));
	EXPECT_EQ(DescribeCompiledQuery(compiled), "records:Assets types:frt.Texture where:frt.Texture.Width Ge 4096 order:Width desc limit:5");
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("VertexCount>2.5 Name:x"), compiled) == false);
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("VertexCount>2.5"), compiled));
	EXPECT_EQ(DescribeCompiledQuery(compiled), "records:Assets where:frt.Model.VertexCount,frt.Mesh.VertexCount Gt real:2.5");
	ASSERT_TRUE(CompileQuery(View(), *SAssetQuery::Parse("ValueType:Bu dir=/Project/Input"), compiled));
	EXPECT_TRUE(DescribeCompiledQuery(compiled).View().Contains("where:frt.InputAction.ValueType Prefix \"bu\"")) << DescribeCompiledQuery(compiled).CStr();
	EXPECT_EQ(DescribeCompiledQuery(SCompiledQuery {}), "");
}

TEST_F(QueryEngineTest, RandomQueriesNeverCrashAndEveryErrorHasAPosition)
{
	// Tokens drawn from the fixture's names, keys and values plus junk, joined at random.
	const FStringView tokens[] = { "type:Texture", "type:Mesh", "t:Raw", "type:Bogus", "Width>=1024", "Width<'1'", "Width:abc", "HasAlpha=true",
		"VertexCount>10", "Actions:J", "ValueType=Axis", "dir:/Project/Textures", "dir=/Project", "dir:/Nope", "label:Hero", "-label:hero",
		"rock", "\"textures/t\"", "ref:T_Rock", "ref:~/Project/Textures/T_Rock.png", "dep:~SM_Rock", "dep:Readme", "ref:/Project/Models/SM_Rock.gltf#mesh:Rock",
		"is:unused", "is:broken-ref", "is:redirected", "is:problem", "$Size>100", "$Type=Model", "$Name:t", "$Referencers>0", "$Bogus=1",
		"rows", "table:DT_Ammo", "order:Width", "order:$Size desc", "limit:3", "or", "(", ")", "Width in (1, 2)", "$Labels in (Hero, Environment)",
		"Bogus=1", "$CookRule=Never", "$ImportStatus in (Analyzed, NotAnalyzed)", "Name:x", "'", "~", "$PrimaryName:imc" };
	std::mt19937 random(20261026u);
	std::uniform_int_distribution<size_t> pick(0u, std::size(tokens) - 1u);
	std::uniform_int_distribution<uint32> length(1u, 6u);
	uint32 ran = 0u;
	uint32 compileErrors = 0u;
	for (uint32 iteration = 0u; iteration < 3000u; ++iteration)
	{
		FString text;
		const uint32 count = length(random);
		for (uint32 i = 0u; i < count; ++i)
		{
			text += i == 0u ? "" : " ";
			text += tokens[pick(random)];
		}
		SDiagnostic error;
		const std::optional<SAssetQuery> parsed = SAssetQuery::Parse(text.View(), &error);
		if (!parsed)
		{
			continue;
		}
		SCompiledQuery compiled;
		if (!CompileQuery(View(), *parsed, compiled, &error))
		{
			++compileErrors;
			EXPECT_GE(error.Column, 1u) << text.CStr();
			EXPECT_LE(error.Column, text.Len()) << text.CStr();
			EXPECT_FALSE(error.Message.IsEmpty()) << text.CStr();
			continue;
		}
		std::vector<SRecordId> records;
		SQueryStats stats;
		const uint32 matched = RunQuery(View(), compiled, records, &stats);
		EXPECT_GE(matched, records.size()) << text.CStr();
		for (const SRecordId record : records)
		{
			EXPECT_TRUE(View().IsValid(record)) << text.CStr();
		}
		++ran;
	}
	EXPECT_GT(ran, 100u);
	EXPECT_GT(compileErrors, 100u);
}
