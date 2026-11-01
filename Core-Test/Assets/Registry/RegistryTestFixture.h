#pragma once

// Small builders for registry deltas, shared by the registry store's tests (B3). They spell deltas the way
// the scan, the rules engine and the importers will produce them, so each test reads as a list of facts.

#include <initializer_list>
#include <vector>

#include "Assets/AssetId.h"
#include "Assets/AssetTypes.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryStore.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Registry/RegistryView.h"
#include "Containers/String.h"


namespace frt::assets::registry_test
{
inline SMountUpsert MakeMount (FStringView Name, MountId Id, bool bCaseSensitive = false)
{
	return SMountUpsert { FString(Name), bCaseSensitive, Id };
}

// /Engine (0) and /Project (1), both case-insensitive.
inline SRegistryDelta MakeMountsDelta ()
{
	SRegistryDelta delta;
	delta.Mounts.push_back(MakeMount("Engine", EngineMountId));
	delta.Mounts.push_back(MakeMount("Project", ProjectMountId));
	return delta;
}

inline SFileUpsert MakeFile (FStringView Path, uint64 Size = 100u, EFileClass Class = EFileClass::Asset)
{
	SFileUpsert file;
	file.MountPath = FString(Path);
	file.Size = Size;
	file.MTime = 1000;
	file.Class = Class;
	return file;
}

inline SRecordUpsert MakeRecord (FStringView Path, reflect::TypeId Type = asset_types::Texture, FStringView Importer = "Texture")
{
	SRecordUpsert record;
	record.FileMountPath = FString(Path);
	record.Guid = MakeProvisionalGuid(Path);
	record.Type = Type;
	record.Importer = FString(Importer);
	record.ImportStatus = EImportStatus::Analyzed;
	return record;
}

inline STagDecl MakeIntTag (FStringView Key, int64 Value)
{
	return STagDecl { FString(Key), STagValue::FromInt64(Value) };
}

inline SEdgeDecl MakePathEdge (FStringView Target, EDepFlags Flags = EDepFlags::Build | EDepFlags::Path, FStringView FieldPath = FStringView())
{
	SEdgeDecl edge;
	edge.TargetPath = FString(Target);
	edge.Flags = Flags;
	edge.FieldPath = FString(FieldPath);
	return edge;
}

// A texture file and its record (Width/Height tags).
inline void AddTexture (SRegistryDelta& Delta, FStringView Path, int64 Width = 64, int64 Height = 64)
{
	Delta.FileUpserts.push_back(MakeFile(Path));
	SRecordUpsert record = MakeRecord(Path);
	record.Tags.push_back(MakeIntTag("Width", Width));
	record.Tags.push_back(MakeIntTag("Height", Height));
	Delta.RecordUpserts.push_back(std::move(record));
}

// A legacy material file and its record with one Build|Path edge per texture.
inline void AddMaterial (SRegistryDelta& Delta, FStringView Path, std::initializer_list<FStringView> Textures)
{
	Delta.FileUpserts.push_back(MakeFile(Path));
	SRecordUpsert record = MakeRecord(Path, asset_types::Material, "Material.Legacy");
	for (const FStringView texture : Textures)
	{
		record.Edges.push_back(MakePathEdge(texture, EDepFlags::Build | EDepFlags::Path, "albedo.texture"));
	}
	Delta.RecordUpserts.push_back(std::move(record));
}

inline FString Dump (const CRegistryView& View, EDumpFlags Flags = EDumpFlags::None)
{
	FString text;
	View.DumpText(text, Flags);
	return text;
}

inline SRecordId FindRecord (const CRegistryView& View, FStringView Path)
{
	return View.FindByPath(Path).value_or(SRecordId {});
}

// The diagnostics of a record with that code.
inline std::vector<SDiagnostic> GetDiagnostics (const CRegistryView& View, SRecordId Record, uint32 Code)
{
	std::vector<SDiagnostic> found;
	View.ForEachDiagnostic(Record, [&] (const SDiagnostic& Diagnostic)
	{
		if (Diagnostic.Code == Code)
		{
			found.push_back(Diagnostic);
		}
	});
	return found;
}

inline std::vector<SDependencyView> GetDependencies (const CRegistryView& View, SRecordId Record)
{
	std::vector<SDependencyView> edges;
	View.ForEachDependency(Record, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge) { edges.push_back(Edge); });
	return edges;
}

inline std::vector<SRecordId> GetReferencers (const CRegistryView& View, SRecordId Record)
{
	std::vector<SRecordId> sources;
	View.ForEachReferencer(Record, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge) { sources.push_back(Edge.From); });
	return sources;
}
}
