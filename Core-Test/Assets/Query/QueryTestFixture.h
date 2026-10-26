#pragma once

// A small registry the query engine's tests (G3) read: every record kind M3 has - textures, legacy materials with
// path edges (one unresolved, one to a sub-entry), a glTF model with mesh, material and image sub-entries (the last
// two of file types: frt.Material, frt.Texture) and a companion,
// input actions and a primary input map with a MultiValue tag, a table with two rows, raw files with a shared stem,
// an excluded texture, a problem, labels, cook rules and chunks - so each query reads as a list of expected paths.

#include <algorithm>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

#include "Assets/AssetId.h"
#include "Assets/AssetTypes.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryStore.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Registry/RegistryView.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


namespace frt::assets::query_test
{
inline SFileUpsert MakeFile (FStringView Path, uint64 Size, EFileClass Class = EFileClass::Asset)
{
	SFileUpsert file;
	file.MountPath = FString(Path);
	file.Size = Size;
	file.MTime = 1000;
	file.Class = Class;
	return file;
}

inline SRecordUpsert MakeRecord (FStringView Path, reflect::TypeId Type, FStringView Importer)
{
	SRecordUpsert record;
	record.FileMountPath = FString(Path);
	record.Guid = MakeProvisionalGuid(Path);
	record.Type = Type;
	record.Importer = FString(Importer);
	record.ImportStatus = EImportStatus::Analyzed;
	return record;
}

inline STagDecl Int (FStringView Key, int64 Value) { return STagDecl { FString(Key), STagValue::FromInt64(Value) }; }
inline STagDecl Bool (FStringView Key, bool bValue) { return STagDecl { FString(Key), STagValue::FromBool(bValue) }; }
inline STagDecl Name (FStringView Key, FStringView Value) { return STagDecl { FString(Key), STagValue::FromName(Value) }; }

inline SEdgeDecl PathEdge (FStringView Target, EDepFlags Flags, uint64 FromSub = 0u)
{
	SEdgeDecl edge;
	edge.TargetPath = FString(Target);
	edge.Flags = Flags;
	edge.FromSub = FromSub;
	return edge;
}

inline void AddTexture (SRegistryDelta& Delta, FStringView Path, int64 Width, int64 Height, int64 Channels, std::initializer_list<FStringView> Labels,
	uint64 Size = 4096u)
{
	Delta.FileUpserts.push_back(MakeFile(Path, Size));
	SRecordUpsert record = MakeRecord(Path, asset_types::Texture, "Texture");
	record.Tags = { Int("Width", Width), Int("Height", Height), Int("Channels", Channels), Bool("HasAlpha", Channels == 4) };
	for (const FStringView label : Labels)
	{
		record.RuleLabels.push_back(FString(label));
	}
	Delta.RecordUpserts.push_back(std::move(record));
}

// Every record of the fixture, one delta per group so tests can commit them in any order.
inline std::vector<SRegistryDelta> MakeFixtureDeltas ()
{
	std::vector<SRegistryDelta> deltas;

	SRegistryDelta mounts;
	mounts.Mounts.push_back(SMountUpsert { "Engine", false, EngineMountId });
	mounts.Mounts.push_back(SMountUpsert { "Project", false, ProjectMountId });
	deltas.push_back(std::move(mounts));

	SRegistryDelta textures;
	AddTexture(textures, "/Project/Textures/T_Rock.png", 1024, 1024, 3, { "Environment" }, 3000u);
	AddTexture(textures, "/Project/Textures/T_Sand.png", 2048, 1024, 4, { "Environment" }, 6000u);
	AddTexture(textures, "/Project/Textures/Hero/T_Hero.png", 4096, 4096, 4, { "Hero" }, 9000u);
	AddTexture(textures, "/Project/Textures/Hero/T_Hero_N.png", 4096, 4096, 3, { "Hero" }, 8000u);
	AddTexture(textures, "/Project/Textures/Old/T_Old.png", 512, 512, 3, {}, 500u);
	textures.FileUpserts.back().Class = EFileClass::Excluded;
	textures.RecordUpserts.back().Flags = ERecordFlags::TypeExcluded;
	textures.RecordUpserts.back().CookRule = ECookRule::Never;
	SProblemsSet problem;
	problem.Subject = "/Project/Textures/T_Sand.png";
	problem.Source = EProblemSource::Analyze;
	problem.Diagnostics.push_back(SDiagnostic { ESeverity::Warning, 0u, "/Project/Textures/T_Sand.png", FString(), 0u, 0u, FString(), "a test warning" });
	textures.Problems.push_back(std::move(problem));
	deltas.push_back(std::move(textures));

	SRegistryDelta materials;
	materials.FileUpserts.push_back(MakeFile("/Project/Materials/M_Rock.frtmat.yml", 200u));
	SRecordUpsert rock = MakeRecord("/Project/Materials/M_Rock.frtmat.yml", asset_types::Material, "Material.Legacy");
	rock.Flags = ERecordFlags::LegacyReferences;
	rock.Edges = { PathEdge("/Project/Textures/T_Rock.png", EDepFlags::Build | EDepFlags::Path),
		PathEdge("/Project/Textures/T_Sand.png", EDepFlags::Build | EDepFlags::Path) };
	materials.RecordUpserts.push_back(std::move(rock));
	materials.FileUpserts.push_back(MakeFile("/Project/Materials/M_Hero.frtmat.yml", 210u));
	SRecordUpsert hero = MakeRecord("/Project/Materials/M_Hero.frtmat.yml", asset_types::Material, "Material.Legacy");
	hero.Edges = { PathEdge("/Project/Textures/Hero/T_Hero.png", EDepFlags::Build | EDepFlags::Path),
		PathEdge("/Project/Textures/Hero/T_Hero_N.png", EDepFlags::Build | EDepFlags::Path),
		PathEdge("/Project/Textures/T_Missing.png", EDepFlags::Build | EDepFlags::Path),
		PathEdge("/Project/Models/SM_Rock.gltf#mesh:Rock", EDepFlags::Hard) };
	materials.RecordUpserts.push_back(std::move(hero));
	deltas.push_back(std::move(materials));

	SRegistryDelta models;
	models.FileUpserts.push_back(MakeFile("/Project/Models/SM_Rock.gltf", 5000u));
	models.FileUpserts.push_back(MakeFile("/Project/Models/SM_Rock.bin", 50000u, EFileClass::Unclaimed));
	SRecordUpsert model = MakeRecord("/Project/Models/SM_Rock.gltf", asset_types::Model, "Model.Gltf");
	model.Tags = { Int("MeshCount", 2), Int("MaterialCount", 1), Int("VertexCount", 4000), Int("TriangleCount", 1300) };
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Rock", 0u, asset_types::Mesh, ERecordFlags::None, { Int("VertexCount", 3000), Int("TriangleCount", 1000) } });
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Rock_LOD1", 0u, asset_types::Mesh, ERecordFlags::None, { Int("VertexCount", 1000), Int("TriangleCount", 300) } });
	model.SubEntries.push_back(SSubEntryDecl { "material:Rock", 0u, asset_types::Material, ERecordFlags::None, {} });
	model.SubEntries.push_back(SSubEntryDecl { "image:Rock_BaseColor", 0u, asset_types::Texture, ERecordFlags::None,
		{ Int("Width", 2048), Int("Height", 2048), Int("Channels", 4), Bool("HasAlpha", true) } });
	model.Edges = { PathEdge("/Project/Models/SM_Rock.bin", EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared),
		PathEdge("/Project/Materials/M_Rock.frtmat.yml", EDepFlags::Hard, MakeSubId("material:Rock")) };
	model.Companions = { "/Project/Models/SM_Rock.bin" };
	model.RuleLabels = { "Environment" };
	models.RecordUpserts.push_back(std::move(model));
	deltas.push_back(std::move(models));

	SRegistryDelta input;
	input.FileUpserts.push_back(MakeFile("/Project/Input/IA_Jump.frtinputaction", 40u));
	SRecordUpsert jump = MakeRecord("/Project/Input/IA_Jump.frtinputaction", asset_types::InputAction, "Input.Legacy");
	jump.Tags = { Name("ValueType", "Button") };
	input.RecordUpserts.push_back(std::move(jump));
	input.FileUpserts.push_back(MakeFile("/Project/Input/IA_Move.frtinputaction", 41u));
	SRecordUpsert move = MakeRecord("/Project/Input/IA_Move.frtinputaction", asset_types::InputAction, "Input.Legacy");
	move.Tags = { Name("ValueType", "Axis") };
	input.RecordUpserts.push_back(std::move(move));
	input.FileUpserts.push_back(MakeFile("/Project/Input/IMC_Default.frtinputmap", 90u));
	SRecordUpsert map = MakeRecord("/Project/Input/IMC_Default.frtinputmap", asset_types::InputActionMap, "Input.Legacy");
	map.Tags = { Name("Actions", "Jump"), Name("Actions", "Move") };
	map.Flags = ERecordFlags::Primary;
	map.PrimaryName = "IMC_Default";
	map.Chunk = 3u;
	map.Edges = { PathEdge("/Project/Input/IA_Jump.frtinputaction", EDepFlags::Path), PathEdge("/Project/Input/IA_Move.frtinputaction", EDepFlags::Path) };
	input.RecordUpserts.push_back(std::move(map));
	deltas.push_back(std::move(input));

	SRegistryDelta data;
	data.FileUpserts.push_back(MakeFile("/Project/Data/DT_Ammo.frt", 300u));
	SRecordUpsert table = MakeRecord("/Project/Data/DT_Ammo.frt", asset_types::Unknown, FStringView());
	table.Flags = ERecordFlags::Schemaless | ERecordFlags::StaleTags;
	table.SubEntries.push_back(SSubEntryDecl { "Pistol9mm", MakeSubId("row:Pistol9mm"), asset_types::Unknown, ERecordFlags::Schemaless, {} });
	table.SubEntries.push_back(SSubEntryDecl { "Rifle556", MakeSubId("row:Rifle556"), asset_types::Unknown, ERecordFlags::Schemaless, {} });
	data.RecordUpserts.push_back(std::move(table));
	data.FileUpserts.push_back(MakeFile("/Project/Misc/A/Readme.txt", 10u, EFileClass::Raw));
	SRecordUpsert readmeA = MakeRecord("/Project/Misc/A/Readme.txt", asset_types::Raw, "Raw");
	readmeA.ImportStatus = EImportStatus::AnalyzeFailed;
	data.RecordUpserts.push_back(std::move(readmeA));
	data.FileUpserts.push_back(MakeFile("/Project/Misc/B/Readme.txt", 11u, EFileClass::Raw));
	data.RecordUpserts.push_back(MakeRecord("/Project/Misc/B/Readme.txt", asset_types::Raw, "Raw"));
	data.FileUpserts.push_back(MakeFile("/Engine/Fonts/Roboto.ttf", 70000u, EFileClass::Raw));
	data.RecordUpserts.push_back(MakeRecord("/Engine/Fonts/Roboto.ttf", asset_types::Raw, "Raw"));
	deltas.push_back(std::move(data));
	return deltas;
}

// The fixture committed group by group (mounts first), in the given order of the other groups.
inline void CommitFixture (CRegistryStore& Store, bool bReverse = false)
{
	std::vector<SRegistryDelta> deltas = MakeFixtureDeltas();
	Store.Commit(std::move(deltas.front()));
	if (bReverse)
	{
		std::reverse(deltas.begin() + 1, deltas.end());
	}
	for (size_t i = 1u; i < deltas.size(); ++i)
	{
		Store.Commit(std::move(deltas[i]));
	}
}

// The mount paths of records, in order.
inline std::vector<FString> PathsOf (const CRegistryView& View, const std::vector<SRecordId>& Records)
{
	std::vector<FString> paths;
	for (const SRecordId record : Records)
	{
		SAssetData data;
		EXPECT_TRUE(View.GetRecord(record, data));
		paths.push_back(data.MountPath);
	}
	return paths;
}

// The paths a text query returns (an error fails the test).
inline std::vector<FString> Query (const CRegistryView& View, FStringView Text)
{
	std::vector<SRecordId> records;
	SDiagnostic error;
	const std::optional<uint32> count = View.RunText(Text, records, &error);
	EXPECT_TRUE(count.has_value()) << Text.GetData() << " -> column " << error.Column << ": " << error.Message.CStr();
	return PathsOf(View, records);
}

// The paths a built query returns.
inline std::vector<FString> Query (const CRegistryView& View, const SAssetQuery& Built)
{
	SCompiledQuery compiled;
	SDiagnostic error;
	EXPECT_TRUE(View.Compile(Built, compiled, &error)) << Built.Describe().CStr() << " -> " << error.Message.CStr();
	std::vector<SRecordId> records;
	View.Run(compiled, records);
	return PathsOf(View, records);
}

// The error a text query fails with (parse or compile).
inline SDiagnostic QueryError (const CRegistryView& View, FStringView Text)
{
	std::vector<SRecordId> records;
	SDiagnostic error;
	EXPECT_FALSE(View.RunText(Text, records, &error).has_value()) << Text.GetData();
	EXPECT_EQ(error.Severity, ESeverity::Error);
	EXPECT_EQ(error.Line, 1u);
	EXPECT_FALSE(error.Message.IsEmpty());
	return error;
}

inline std::vector<FString> List (std::initializer_list<FStringView> Paths)
{
	std::vector<FString> list;
	for (const FStringView path : Paths)
	{
		list.push_back(FString(path));
	}
	return list;
}
}
