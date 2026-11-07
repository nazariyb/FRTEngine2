#include "QueryBenchmarkGenerator.h"

#include <iterator>

#include "Assets/AssetId.h"
#include "Assets/AssetTypes.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Platform/Timestamp.h"


namespace frt::assets::query_bench
{
namespace
{
constexpr uint32 RowsPerTable = 500u;
constexpr uint32 FilesPerFolder = 48u;

constexpr FStringLiteral Themes[] = { "Rock", "Sand", "Wood", "Metal", "Hero", "Foliage", "Brick", "Water" };
constexpr FStringLiteral Words[] = { "Albedo", "Normal", "Rough", "Mask", "Detail", "Base", "Worn", "Clean", "Dirty", "Large", "Small", "Tiled" };
constexpr FStringLiteral Labels[] = { "Environment", "Hero", "Props", "UI", "Debug", "Prototype" };
constexpr uint32 ThemeCount = static_cast<uint32>(std::size(Themes));
constexpr uint32 WordCount = static_cast<uint32>(std::size(Words));
constexpr uint32 LabelCount = static_cast<uint32>(std::size(Labels));

enum class ECategory : uint8 { Texture, Material, Model, Action, Map, Raw, Table };

// SplitMix64: one stream per record, seeded by (seed, category, index), so a record never depends on chunking.
struct SRandom
{
	uint64 State = 0u;

	SRandom (uint32 Seed, ECategory Category, uint32 Index) noexcept
		: State((static_cast<uint64>(Seed) << 32u) ^ (static_cast<uint64>(Category) << 56u) ^ Index)
	{
	}

	uint64 Next () noexcept
	{
		uint64 z = (State += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31u);
	}

	uint32 Below (uint32 Count) noexcept { return Count == 0u ? 0u : static_cast<uint32>(Next() % Count); }
};

FStringView Theme (uint32 K) noexcept { return Themes[K % ThemeCount]; }
FStringView Word (uint32 K) noexcept { return Words[(K * 2654435761u >> 7u) % WordCount]; }
uint32 Folder (uint32 K) noexcept { return K / ThemeCount / FilesPerFolder; }

FString CategoryPath (FStringView Category, FStringView Prefix, uint32 K, FStringView Extension)
{
	return FString::Format("/Project/{}/{}/S{}/{}_{}_{}_{}{}", Category, Theme(K), Folder(K), Prefix, Theme(K), Word(K), K, Extension);
}

FString ActionPath (uint32 K) { return FString::Format("/Project/Input/Actions/S{}/IA_{}_{}.frtinputaction", K / FilesPerFolder, Word(K), K); }
FString MapPath (uint32 K) { return FString::Format("/Project/Input/Maps/S{}/IMC_{}_{}.frtinputmap", K / FilesPerFolder, Word(K), K); }
FString RawPath (uint32 K) { return CategoryPath("Audio", "A", K, ".wav"); }
FString TablePath (uint32 K) { return FString::Format("/Project/Data/Tables/S{}/DT_{}_{}.frt", K / FilesPerFolder, Word(K), K); }
FString BinPath (uint32 K) { return CategoryPath("Models", "SM", K, ".bin"); }
FString MissingTexturePath (uint32 K) { return FString::Format("/Project/Textures/Missing/T_Missing_{}.png", K); }

SFileUpsert MakeFile (FString Path, uint64 Size, EFileClass Class)
{
	SFileUpsert file;
	file.MountPath = std::move(Path);
	file.Size = Size;
	file.MTime = 1000;
	file.Class = Class;
	return file;
}

SRecordUpsert MakeRecord (const FString& Path, reflect::TypeId Type, FStringView Importer, SRandom& Random)
{
	SRecordUpsert record;
	record.FileMountPath = Path;
	record.Guid = MakeProvisionalGuid(Path.View());
	record.Type = Type;
	record.Importer = FString(Importer);
	record.ImportStatus = EImportStatus::Analyzed;
	const uint32 first = Random.Below(LabelCount);
	record.RuleLabels.push_back(FString(Labels[first]));
	const uint32 second = Random.Below(2u * LabelCount);
	if (second < LabelCount && second != first)
	{
		record.RuleLabels.push_back(FString(Labels[second]));
	}
	return record;
}

STagDecl Int (FStringView Key, int64 Value) { return STagDecl { FString(Key), STagValue::FromInt64(Value) }; }

SEdgeDecl Edge (FString Target, EDepFlags Flags, FString FieldPath, uint64 FromSub = 0u)
{
	SEdgeDecl edge;
	edge.TargetPath = std::move(Target);
	edge.Flags = Flags;
	edge.FieldPath = std::move(FieldPath);
	edge.FromSub = FromSub;
	return edge;
}

class CGenerator
{
public:
	CGenerator (CRegistryStore& InStore, const SGeneratorSpec& InSpec)
		: Store(InStore)
		, Spec(InSpec)
	{
		Report.Tables = Spec.Rows > 0u ? (Spec.Rows + RowsPerTable - 1u) / RowsPerTable : 0u;
		Report.Rows = Spec.Rows;
		const uint32 assets = Spec.Assets > Report.Tables ? Spec.Assets - Report.Tables : 0u;
		Report.Textures = assets / 2u;
		Report.Materials = assets / 5u;
		Report.Models = assets / 10u;
		Report.Actions = assets / 25u;
		Report.Maps = assets / 100u;
		Report.Raws = assets - Report.Textures - Report.Materials - Report.Models - Report.Actions - Report.Maps;
	}

	SGeneratorReport Run ()
	{
		Delta.Mounts.push_back(SMountUpsert { "Engine", false, EngineMountId });
		Delta.Mounts.push_back(SMountUpsert { "Project", false, ProjectMountId });
		const uint64 start = platform::ReadTimestamp();
		for (uint32 k = 0u; k < Report.Textures; ++k) { AddTexture(k); }
		for (uint32 k = 0u; k < Report.Raws; ++k) { AddRaw(k); }
		for (uint32 k = 0u; k < Report.Actions; ++k) { AddAction(k); }
		for (uint32 k = 0u; k < Report.Materials; ++k) { AddMaterial(k); }
		for (uint32 k = 0u; k < Report.Models; ++k) { AddModel(k); }
		for (uint32 k = 0u; k < Report.Maps; ++k) { AddMap(k); }
		uint32 rowsLeft = Spec.Rows;
		for (uint32 k = 0u; k < Report.Tables; ++k)
		{
			const uint32 rows = rowsLeft < RowsPerTable ? rowsLeft : RowsPerTable;
			AddTable(k, rows);
			rowsLeft -= rows;
		}
		Flush();
		Report.BuildMilliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start) - Report.CommitMilliseconds;
		return Report;
	}

private:
	CRegistryStore& Store;
	const SGeneratorSpec& Spec;
	SGeneratorReport Report;
	SRegistryDelta Delta;
	uint32 Weight = 0u;

	void Add (SRecordUpsert&& Record)
	{
		if (Spec.bMetas)
		{
			AddMeta(Record);
		}
		Weight += 1u + static_cast<uint32>(Record.SubEntries.size());
		Delta.RecordUpserts.push_back(std::move(Record));
		if (Weight >= Spec.ChunkWeight)
		{
			Flush();
		}
	}

	void Flush ()
	{
		if (Delta.RecordUpserts.empty() && Delta.Mounts.empty())
		{
			return;
		}
		const uint64 start = platform::ReadTimestamp();
		Store.Commit(std::move(Delta));
		Report.CommitMilliseconds += platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		++Report.Commits;
		Delta = SRegistryDelta {};
		Weight = 0u;
	}

	// The record's meta: its GUID, the source row's meta columns, the meta's own FILES row and its METAS copy.
	void AddMeta (SRecordUpsert& Record)
	{
		SAssetMeta meta;
		meta.Guid = MakeDerivedGuid(SGuid {}, "query-bench-meta", Record.FileMountPath);
		meta.Importer = Record.Importer;
		meta.Labels = Record.RuleLabels;
		FString text;
		if (!EmitMeta(meta, text))
		{
			return;
		}
		const uint64 hash = HashMetaBytes(text);
		const SFileMetaState columns { EMetaState::Paired, hash };
		for (auto it = Delta.FileUpserts.rbegin(); it != Delta.FileUpserts.rend(); ++it)
		{
			if (it->MountPath == Record.FileMountPath)
			{
				it->Meta = columns;
				break;
			}
		}
		SFileUpsert row = MakeFile(GetMetaPath(Record.FileMountPath), text.Len(), EFileClass::Meta);
		row.Meta = columns;
		Delta.FileUpserts.push_back(std::move(row));
		Record.Guid = meta.Guid;
		SMetaCopy copy;
		copy.Guid = meta.Guid;
		copy.Hash = hash;
		copy.Bytes.assign(text.AsBytes().begin(), text.AsBytes().end());
		copy.SourcePath = Record.FileMountPath;
		Report.MetaBytes += copy.Bytes.size();
		++Report.Metas;
		Delta.MetaCopies.push_back(std::move(copy));
	}

	[[nodiscard]] FString RandomTexture (SRandom& Random) const
	{
		return Random.Below(100u) == 0u ? MissingTexturePath(Random.Below(1000u)) : GetTexturePath(Random.Below(Report.Textures));
	}

	void AddTexture (uint32 K)
	{
		SRandom random(Spec.Seed, ECategory::Texture, K);
		const FString path = GetTexturePath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 1024u + random.Below(16u << 20u), EFileClass::Asset));
		SRecordUpsert record = MakeRecord(path, asset_types::Texture, "Texture", random);
		const int64 width = int64(64) << random.Below(8u);
		const int64 height = int64(64) << random.Below(8u);
		const int64 channels = 3 + random.Below(2u);
		record.Tags = { Int("Width", width), Int("Height", height), Int("Channels", channels),
			STagDecl { "HasAlpha", STagValue::FromBool(channels == 4) } };
		if (random.Below(200u) == 0u)
		{
			SProblemsSet problem;
			problem.Subject = path;
			problem.Source = EProblemSource::Analyze;
			SDiagnostic warning;
			warning.Severity = ESeverity::Warning;
			warning.Subject = path;
			warning.Message = "generated warning";
			problem.Diagnostics.push_back(std::move(warning));
			Delta.Problems.push_back(std::move(problem));
		}
		Add(std::move(record));
	}

	void AddRaw (uint32 K)
	{
		SRandom random(Spec.Seed, ECategory::Raw, K);
		const FString path = RawPath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 4096u + random.Below(4u << 20u), EFileClass::Raw));
		Add(MakeRecord(path, asset_types::Raw, "Raw", random));
	}

	void AddAction (uint32 K)
	{
		SRandom random(Spec.Seed, ECategory::Action, K);
		const FString path = ActionPath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 64u + random.Below(512u), EFileClass::Asset));
		SRecordUpsert record = MakeRecord(path, asset_types::InputAction, "Input.Legacy", random);
		record.Tags = { STagDecl { "ValueType", STagValue::FromName(random.Below(2u) == 0u ? "Button" : "Axis") } };
		for (uint32 j = 1u; j <= 3u; ++j)
		{
			record.Edges.push_back(Edge(ActionPath((K + j * 37u) % Report.Actions), EDepFlags::Path, FString::Format("actions[{}]", j)));
		}
		Add(std::move(record));
	}

	void AddMaterial (uint32 K)
	{
		SRandom random(Spec.Seed, ECategory::Material, K);
		const FString path = GetMaterialPath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 256u + random.Below(2048u), EFileClass::Asset));
		SRecordUpsert record = MakeRecord(path, asset_types::Material, "Material.Legacy", random);
		if (random.Below(10u) < 3u)
		{
			record.Flags = ERecordFlags::LegacyReferences;
		}
		for (uint32 slot = 0u; slot < 14u; ++slot)
		{
			FString texture = slot == 0u && K % 20u == 0u ? GetTexturePath(0u) : RandomTexture(random);
			record.Edges.push_back(Edge(std::move(texture), EDepFlags::Build | EDepFlags::Path, FString::Format("slots[{}].texture", slot)));
		}
		Add(std::move(record));
	}

	void AddModel (uint32 K)
	{
		SRandom random(Spec.Seed, ECategory::Model, K);
		const FString path = GetModelPath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 2048u + random.Below(1u << 20u), EFileClass::Asset));
		Delta.FileUpserts.push_back(MakeFile(BinPath(K), 65536u + random.Below(64u << 20u), EFileClass::Unclaimed));
		SRecordUpsert record = MakeRecord(path, asset_types::Model, "Model.Gltf", random);
		int64 vertices = 0;
		int64 triangles = 0;
		for (uint32 lod = 0u; lod < 2u; ++lod)
		{
			const int64 v = 100 + random.Below(100000u);
			const int64 t = v / 2 + random.Below(1000u);
			vertices += v;
			triangles += t;
			record.SubEntries.push_back(SSubEntryDecl { FString::Format("mesh:{}_{}_LOD{}", Theme(K), K, lod), 0u, asset_types::Mesh, ERecordFlags::None,
				{ Int("VertexCount", v), Int("TriangleCount", t) } });
		}
		record.Edges.push_back(Edge(BinPath(K), EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
		for (uint32 m = 0u; m < 2u; ++m)
		{
			const FString label = FString::Format("material:{}{}", Theme(K), m);
			record.SubEntries.push_back(SSubEntryDecl { label, 0u, asset_types::Material, ERecordFlags::None, {} });
			record.Edges.push_back(Edge(GetMaterialPath(random.Below(Report.Materials)), EDepFlags::Hard, FString::Format("materials[{}]", m),
				MakeSubId(label.View())));
		}
		for (uint32 m = 0u; m < 8u; ++m)
		{
			record.Edges.push_back(Edge(GetMaterialPath(random.Below(Report.Materials)), EDepFlags::Hard, FString::Format("variants[{}]", m)));
		}
		record.Tags = { Int("MeshCount", 2), Int("MaterialCount", 2), Int("ImageCount", random.Below(4u)), Int("VertexCount", vertices),
			Int("TriangleCount", triangles) };
		record.Companions = { BinPath(K) };
		Add(std::move(record));
	}

	void AddMap (uint32 K)
	{
		SRandom random(Spec.Seed, ECategory::Map, K);
		const FString path = MapPath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 128u + random.Below(1024u), EFileClass::Asset));
		SRecordUpsert record = MakeRecord(path, asset_types::InputActionMap, "Input.Legacy", random);
		record.Flags = ERecordFlags::Primary;
		record.PrimaryName = FString::Format("IMC_{}_{}", Word(K), K);
		for (uint32 j = 0u; j < 4u; ++j)
		{
			const uint32 action = Report.Actions != 0u ? random.Below(Report.Actions) : 0u;
			record.Tags.push_back(STagDecl { "Actions", STagValue::FromName(FString::Format("{}_{}", Word(action), action)) });
			record.Edges.push_back(Edge(ActionPath(action), EDepFlags::Path, FString::Format("actions[{}]", j)));
		}
		Add(std::move(record));
	}

	void AddTable (uint32 K, uint32 Rows)
	{
		SRandom random(Spec.Seed, ECategory::Table, K);
		const FString path = TablePath(K);
		Delta.FileUpserts.push_back(MakeFile(path, 64u * Rows + random.Below(4096u), EFileClass::Asset));
		SRecordUpsert record = MakeRecord(path, asset_types::Unknown, FStringView(), random);
		record.Flags = ERecordFlags::Schemaless;
		record.SubEntries.reserve(Rows);
		for (uint32 row = 0u; row < Rows; ++row)
		{
			const FString key = FString::Format("Row_{}", row);
			const uint64 subId = MakeSubId(FString::Format("row:{}", key).View());
			record.SubEntries.push_back(SSubEntryDecl { key, subId, asset_types::Model, ERecordFlags::Schemaless,
				{ Int("MeshCount", random.Below(9u)), Int("MaterialCount", random.Below(5u)), Int("ImageCount", random.Below(5u)),
					Int("VertexCount", random.Below(100000u)), Int("TriangleCount", random.Below(200000u)) } });
			record.Edges.push_back(Edge(GetTexturePath(random.Below(Report.Textures)), EDepFlags::Schemaless, FString::Format("Rows.{}.Icon", key), subId));
		}
		Add(std::move(record));
	}
};
}


FString GetTexturePath (uint32 K)
{
	return CategoryPath("Textures", "T", K, ".png");
}

FString GetMaterialPath (uint32 K)
{
	return CategoryPath("Materials", "M", K, ".frtmat.yml");
}

FString GetModelPath (uint32 K)
{
	return CategoryPath("Models", "SM", K, ".gltf");
}

SGeneratorReport Generate (CRegistryStore& Store, const SGeneratorSpec& Spec)
{
	CGenerator generator(Store, Spec);
	return generator.Run();
}
}
