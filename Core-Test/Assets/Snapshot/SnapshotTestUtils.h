#pragma once

// Helpers for the snapshot and journal tests (H3): a scratch directory per test, a registry with everything a
// snapshot carries, comparisons that look past the text dump, and byte surgery on snapshot files.

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

#include "../Registry/RegistryTestFixture.h"
#include "Assets/ByteStream.h"
#include "Assets/Registry/Journal.h"
#include "Assets/Registry/RegistryTables.h"
#include "Assets/Registry/Snapshot.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"


namespace frt::assets::snapshot_test
{
using namespace registry_test;

// A scratch directory per test under the temp folder, with non-ASCII names.
class SnapshotTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Знімок 快照 {}", platform::GetCurrentProcessId()).ToPath()
			/ FStringView(test->name()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	std::filesystem::path Dir;
};

inline SDiagnostic MakeWarning (FStringView Subject, FStringView Message, uint32 Code = 0u)
{
	SDiagnostic diagnostic;
	diagnostic.Severity = ESeverity::Warning;
	diagnostic.Code = Code;
	diagnostic.Subject = FString(Subject);
	diagnostic.Message = FString(Message);
	return diagnostic;
}

// A file upsert with machine facts a dump never shows: mtime, identity, content hash.
inline SFileUpsert MakeStatFile (FStringView Path, uint64 Size, uint32 Seed, EFileClass Class = EFileClass::Asset)
{
	SFileUpsert file = MakeFile(Path, Size, Class);
	file.MTime = 133000000000000000ll + Seed;
	file.Identity.Bytes[0] = static_cast<uint8>(Seed);
	file.Identity.Bytes[23] = static_cast<uint8>(Seed >> 8u);
	file.ContentHash = hash::HashContent(FString::Format("content {}", Seed).AsBytes());
	file.Meta = SFileMetaState { EMetaState::PendingMeta, 0x1000u + Seed };
	return file;
}

inline SGuid MakeTestGuid (FStringView Label)
{
	return MakeDerivedGuid(SGuid {}, "snapshot-test", Label);
}

// A meta copy whose hash is its bytes' (the store keeps no other).
inline SMetaCopy MakeMetaCopy (const SGuid& Guid, FStringView SourcePath)
{
	const FString text = FString::Format("FormatVersion: 1\nGuid: {}\n", Guid.ToString());
	SMetaCopy copy;
	copy.Guid = Guid;
	copy.Bytes.assign(text.AsBytes().begin(), text.AsBytes().end());
	copy.Hash = hash::Xxh3_64(copy.Bytes);
	copy.SourcePath = FString(SourcePath);
	return copy;
}

/**
 * Identity state (M4): a texture with a meta-given GUID that moved (FormerPaths) and absorbed another GUID (Aliases), a
 * material naming it by the old path and the absorbed id (both redirected, REF006), a renamed sub-asset of the model
 * and an id edge to its new label, meta copies in two mounts and tombstones in two mounts (removed just now, so a
 * snapshot keeps them).
 */
inline void AddIdentityContent (SRegistryDelta& Delta)
{
	Delta.FileUpserts.push_back(MakeStatFile("/Project/T/Moved.png", 1200u, 96u));
	Delta.FileUpserts.back().Meta = SFileMetaState { EMetaState::Paired, MakeMetaCopy(MakeTestGuid("moved"), "/Project/T/Moved.png").Hash };
	SRecordUpsert moved = MakeRecord("/Project/T/Moved.png");
	moved.Guid = MakeTestGuid("moved");
	moved.Aliases = { SAssetId { MakeTestGuid("absorbed"), 0u } };
	moved.FormerPaths = { "/Project/T/Stone.png" };
	moved.AuthoredLabels = { "Stone" };
	Delta.RecordUpserts.push_back(std::move(moved));

	Delta.FileUpserts.push_back(MakeStatFile("/Project/M/old.frtmat.yml", 310u, 97u));
	SRecordUpsert material = MakeRecord("/Project/M/old.frtmat.yml", asset_types::Material, "Material.Legacy");
	material.Edges.push_back(MakePathEdge("/Project/T/stone.png", EDepFlags::Build | EDepFlags::Path, "albedo.texture"));
	SEdgeDecl absorbed;
	absorbed.Target = SAssetId { MakeTestGuid("absorbed"), 0u };
	absorbed.Flags = EDepFlags::Declared;
	material.Edges.push_back(absorbed);
	SEdgeDecl renamed;
	renamed.Target = SAssetId { MakeProvisionalGuid("/Project/Hero.gltf"), MakeSubId("mesh:Body") };
	renamed.Flags = EDepFlags::Declared;
	material.Edges.push_back(renamed);
	Delta.RecordUpserts.push_back(std::move(material));

	// The model's mesh:Body keeps the id of its old label, mesh:Torso; its own label's id is an alias.
	for (SRecordUpsert& record : Delta.RecordUpserts)
	{
		if (record.FileMountPath == "/Project/Hero.gltf")
		{
			for (SSubEntryDecl& sub : record.SubEntries)
			{
				sub.SubId = sub.Label == "mesh:Body" ? MakeSubId("mesh:Torso") : sub.SubId;
			}
			AppendRenamedSubAliases(record.Guid, record.SubEntries, record.Aliases);
		}
	}

	Delta.MetaCopies.push_back(MakeMetaCopy(MakeTestGuid("moved"), "/Project/T/Moved.png"));
	Delta.MetaCopies.push_back(MakeMetaCopy(MakeTestGuid("engine white"), "/Engine/Textures/White.png"));
	const int64 now = GetFileTimeNow();
	STombstone deleted { MakeTestGuid("deleted"), "/Project/T/Deleted Видалено.png", hash::HashContent(FStringView("deleted").AsBytes()), {}, now - 1000 };
	deleted.Identity.Bytes[3] = 9u;
	Delta.Tombstones.push_back(deleted);
	Delta.Tombstones.push_back(STombstone { MakeTestGuid("felled"), "/Plugin.Trees/Felled.png", hash::SContentHash {}, {}, now });
}

// The identity state of two views agrees (M4): each record's aliases and former paths, the meta copies, the tombstones.
inline void ExpectSameIdentity (const CRegistryView& Expected, const CRegistryView& Actual)
{
	EXPECT_EQ(Actual.GetMetaCopyCount(), Expected.GetMetaCopyCount());
	EXPECT_EQ(Actual.GetTombstoneCount(), Expected.GetTombstoneCount());
	Expected.ForEachMetaCopy([&] (const SMetaCopyView& Copy)
	{
		const std::optional<SMetaCopyView> other = Actual.FindMetaCopy(Copy.Guid);
		ASSERT_TRUE(other.has_value()) << Copy.Guid.ToString().CStr();
		EXPECT_EQ(other->Hash, Copy.Hash);
		EXPECT_EQ(other->SourcePath, Copy.SourcePath);
		EXPECT_TRUE(std::equal(other->Bytes.begin(), other->Bytes.end(), Copy.Bytes.begin(), Copy.Bytes.end()));
	});
	Expected.ForEachTombstone([&] (const STombstoneView& Tombstone)
	{
		const std::optional<STombstoneView> other = Actual.FindTombstone(Tombstone.Guid);
		ASSERT_TRUE(other.has_value()) << Tombstone.Guid.ToString().CStr();
		EXPECT_EQ(other->LastPath, Tombstone.LastPath);
		EXPECT_EQ(other->Hash, Tombstone.Hash);
		EXPECT_EQ(other->Identity, Tombstone.Identity);
		EXPECT_EQ(other->RemovedFileTime, Tombstone.RemovedFileTime);
	});
	Expected.ForEachRecord([&] (SRecordId Record)
	{
		SAssetData data;
		ASSERT_TRUE(Expected.GetRecord(Record, data));
		const SRecordId other = Actual.Find(data.Id).value_or(SRecordId {});
		ASSERT_TRUE(other.IsValid()) << data.MountPath.CStr();
		std::vector<SAssetId> a;
		std::vector<SAssetId> b;
		Expected.ForEachAlias(Record, [&] (const SAssetId& Alias) { a.push_back(Alias); });
		Actual.ForEachAlias(other, [&] (const SAssetId& Alias) { b.push_back(Alias); });
		EXPECT_EQ(a, b) << data.MountPath.CStr();
		std::vector<FString> c;
		std::vector<FString> d;
		Expected.ForEachFormerPath(Record, [&] (FStringView Path) { c.emplace_back(Path); });
		Actual.ForEachFormerPath(other, [&] (FStringView Path) { d.emplace_back(Path); });
		EXPECT_EQ(c, d) << data.MountPath.CStr();
		for (const SAssetId& alias : a)
		{
			bool bRedirected = false;
			const std::optional<SRecordId> found = Actual.Find(alias, &bRedirected);
			EXPECT_TRUE(found.has_value() && bRedirected) << data.MountPath.CStr();
		}
	});
}

/**
 * Everything a snapshot carries: /Engine, /Project and a case-sensitive plugin; nested and empty folders; rule
 * files; files with stats and hashes; textures with tags; legacy materials with path edges (resolved across
 * mounts, in another case for REF006, unresolved); a model with sub-entries, a companion and edges from a
 * sub-entry and by id (into its own mount set, and - bCrossMountIdEdge - into /Engine); an input map with a
 * MultiValue Name tag and a primary name; a record of an unavailable importer; rule labels; problems of the Scan,
 * Rules and Analyze sources.
 *
 * An edge by id into a mount another snapshot file holds is written by the target's mount path (§5.3,
 * ExportImage) and so comes back as a path edge: content split over two files leaves it out.
 */
inline SRegistryDelta MakeContent (bool bCrossMountIdEdge = true)
{
	SRegistryDelta delta = MakeMountsDelta();
	delta.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, true));
	uint32 seed = 1u;
	const auto addTexture = [&] (FStringView Path, int64 Width)
	{
		delta.FileUpserts.push_back(MakeStatFile(Path, 1000u + seed, seed));
		++seed;
		SRecordUpsert record = MakeRecord(Path);
		record.Tags = { MakeIntTag("Width", Width), MakeIntTag("Height", Width / 2), STagDecl { "HasAlpha", STagValue::FromBool(Width > 4) } };
		record.RuleLabels = { "Textures" };
		record.AnalyzeStamp = 0xABCDu + seed;
		delta.RecordUpserts.push_back(std::move(record));
	};
	const auto addMaterial = [&] (FStringView Path, std::initializer_list<FStringView> Textures)
	{
		delta.FileUpserts.push_back(MakeStatFile(Path, 300u + seed, seed));
		++seed;
		SRecordUpsert record = MakeRecord(Path, asset_types::Material, "Material.Legacy");
		record.Flags = ERecordFlags::LegacyReferences;
		for (const FStringView texture : Textures)
		{
			record.Edges.push_back(MakePathEdge(texture, EDepFlags::Build | EDepFlags::Path, "albedo.texture"));
		}
		delta.RecordUpserts.push_back(std::move(record));
	};
	addTexture("/Engine/Textures/White.png", 4);
	addTexture("/Project/T/Rock.png", 64);
	addTexture("/Project/T/Deep/Er/Grass Трава.png", 8);
	addTexture("/Plugin.Trees/Bark.png", 16);
	addMaterial("/Project/M/m.frtmat.yml", { "/Engine/textures/white.png", "/Project/T/Rock.png", "/Project/T/gone.png" });
	addMaterial("/Plugin.Trees/Oak.frtmat.yml", { "/Project/T/Deep/Er/Grass Трава.png", "/Plugin.Trees/Bark.png" });

	delta.FileUpserts.push_back(MakeStatFile("/Project/Hero.gltf", 5000u, 90u));
	delta.FileUpserts.push_back(MakeStatFile("/Project/Hero.bin", 50000u, 91u, EFileClass::Unclaimed));
	delta.FileUpserts.push_back(MakeStatFile("/Project/notes.txt", 7u, 92u, EFileClass::Unclaimed));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.Companions.push_back("/Project/Hero.bin");
	model.RuleLabels = { "Hero", "Characters" };
	model.AnalyzeStamp = 0x1234u;
	model.CookRule = ECookRule::Always;
	model.Chunk = 3u;
	model.Tags = { MakeIntTag("MeshCount", 1), MakeIntTag("VertexCount", 9) };
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 9) } });
	model.SubEntries.push_back(SSubEntryDecl { "material:Skin", 0u, asset_types::Material, ERecordFlags::None, {} });
	model.SubEntries.push_back(SSubEntryDecl { "image:#0", 0u, asset_types::Texture, ERecordFlags::None, { MakeIntTag("Width", 2) } });
	model.Edges.push_back(MakePathEdge("/Project/Hero.bin", EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
	SEdgeDecl skin = MakePathEdge("/Project/M/m.frtmat.yml", EDepFlags::Hard, "materials[0]");
	skin.FromSub = MakeSubId("material:Skin");
	model.Edges.push_back(skin);
	SEdgeDecl byId;
	byId.Target = SAssetId { MakeProvisionalGuid("/Plugin.Trees/Oak.frtmat.yml"), 0u };
	byId.Flags = EDepFlags::Hard | EDepFlags::Declared;
	model.Edges.push_back(byId);
	if (bCrossMountIdEdge)
	{
		SEdgeDecl engineById;
		engineById.Target = SAssetId { MakeProvisionalGuid("/Engine/Textures/White.png"), 0u };
		engineById.Flags = EDepFlags::Declared;
		model.Edges.push_back(engineById);
	}
	delta.RecordUpserts.push_back(std::move(model));

	delta.FileUpserts.push_back(MakeStatFile("/Project/Input/Map.frtinputmap", 80u, 93u));
	SRecordUpsert map = MakeRecord("/Project/Input/Map.frtinputmap", asset_types::InputActionMap, "Input.Legacy");
	map.Tags = { STagDecl { "Actions", STagValue::FromName("Jump") }, STagDecl { "Actions", STagValue::FromName("Fire") } };
	map.Flags = ERecordFlags::Primary;
	map.PrimaryName = "Map";
	map.Edges.push_back(MakePathEdge("/Project/Input/Jump.frtinputaction", EDepFlags::Path));
	delta.RecordUpserts.push_back(std::move(map));
	delta.FileUpserts.push_back(MakeStatFile("/Project/Input/Jump.frtinputaction", 20u, 94u));
	SRecordUpsert action = MakeRecord("/Project/Input/Jump.frtinputaction", asset_types::InputAction, "Input.Legacy");
	action.Tags = { STagDecl { "ValueType", STagValue::FromName("Button") } };
	delta.RecordUpserts.push_back(std::move(action));

	delta.FileUpserts.push_back(MakeStatFile("/Project/Future.frt", 10u, 95u));
	SRecordUpsert future = MakeRecord("/Project/Future.frt", asset_types::Raw, "Document");
	future.ImportStatus = EImportStatus::ImporterUnavailable;
	delta.RecordUpserts.push_back(std::move(future));

	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Engine", "", 21u });
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "", 11u });
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/T", 12u });
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Plugin.Trees", "", 31u });
	delta.FolderUpserts.push_back(SFolderUpsert { "/Project/Empty/Inner" });
	delta.Problems.push_back(SProblemsSet { "/Project/T/Rock.png", EProblemSource::Scan, { MakeWarning("/Project/T/Rock.png", "long", DiagnosticCode("PATH003")) } });
	delta.Problems.push_back(SProblemsSet { "/Project/T", EProblemSource::Rules, { MakeWarning("/Project/T", "unknown key", DiagnosticCode("RULE001")) } });
	delta.Problems.push_back(SProblemsSet { "/Project/T", EProblemSource::Unclaimed, { MakeWarning("/Project/T", "no importer for .docx", DiagnosticCode("RULE002")) } });
	delta.Problems.push_back(SProblemsSet { "/Project/Hero.gltf#mesh:Body", EProblemSource::Analyze, { MakeWarning("/Project/Hero.gltf#mesh:Body", "degenerate") } });
	delta.Problems.push_back(SProblemsSet { "/Engine/Textures/White.png", EProblemSource::Analyze, { MakeWarning("/Engine/Textures/White.png", "tiny") } });
	AddIdentityContent(delta);
	return delta;
}

// The dump plus what a dump never shows: file stats and hashes, ids, lookups under each mount's rule, referencers.
inline void ExpectSameContent (const CRegistryView& Expected, const CRegistryView& Actual)
{
	EXPECT_EQ(Dump(Actual), Dump(Expected));
	ExpectSameIdentity(Expected, Actual);
	EXPECT_EQ(Actual.GetRecordCount(), Expected.GetRecordCount());
	EXPECT_EQ(Actual.GetSubEntryCount(), Expected.GetSubEntryCount());
	EXPECT_EQ(Actual.GetFileCount(), Expected.GetFileCount());
	EXPECT_EQ(Actual.GetFolderCount(), Expected.GetFolderCount());
	for (const SMountInfo& mount : Expected.GetMounts())
	{
		const std::optional<SMountInfo> other = Actual.FindMount(mount.Name);
		ASSERT_TRUE(other.has_value()) << FString(mount.Name).CStr();
		EXPECT_EQ(other->Id, mount.Id);
		EXPECT_EQ(other->bCaseSensitive, mount.bCaseSensitive);
	}
	Expected.ForEachFile([&] (uint32 Row)
	{
		SFileData a;
		SFileData b;
		ASSERT_TRUE(Expected.GetFile(Row, a));
		const std::optional<uint32> other = Actual.FindFileExact(a.MountPath);
		ASSERT_TRUE(other.has_value()) << FString(a.MountPath).CStr();
		ASSERT_TRUE(Actual.GetFile(*other, b));
		EXPECT_EQ(a.Size, b.Size);
		EXPECT_EQ(a.MTime, b.MTime);
		EXPECT_EQ(a.Identity, b.Identity);
		EXPECT_EQ(a.ContentHash, b.ContentHash);
		EXPECT_EQ(a.MetaHash, b.MetaHash);
		EXPECT_EQ(a.Class, b.Class);
		EXPECT_EQ(a.MetaState, b.MetaState);
	});
	Expected.ForEachRecord([&] (SRecordId Record)
	{
		SAssetData a;
		ASSERT_TRUE(Expected.GetRecord(Record, a));
		const SRecordId other = Actual.Find(a.Id).value_or(SRecordId {});
		ASSERT_TRUE(other.IsValid()) << a.MountPath.CStr();
		SAssetData b;
		ASSERT_TRUE(Actual.GetRecord(other, b));
		EXPECT_EQ(a.MountPath, b.MountPath);
		EXPECT_EQ(a.Id64, b.Id64);
		EXPECT_EQ(a.Flags, b.Flags) << a.MountPath.CStr();
		EXPECT_EQ(GetReferencers(Expected, Record).size(), GetReferencers(Actual, other).size()) << a.MountPath.CStr();
		EXPECT_EQ(GetDependencies(Expected, Record).size(), GetDependencies(Actual, other).size()) << a.MountPath.CStr();
	});
}

// The flags and AnalyzeStamp of a record by path.
inline SAssetData GetData (const CRegistryView& View, FStringView Path)
{
	SAssetData data;
	View.GetRecord(FindRecord(View, Path), data);
	return data;
}

inline uint64 GetAnalyzeStamp (const CRegistryView& View, FStringView Path)
{
	const SRecordId record = FindRecord(View, Path);
	return record.IsValid() ? GetTables(View).Assets.AnalyzeStamp[record.Value] : 0u;
}

inline std::vector<uint8> ReadBytes (const std::filesystem::path& Path)
{
	std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path);
	return bytes ? std::move(*bytes) : std::vector<uint8>();
}

inline void WriteBytes (const std::filesystem::path& Path, std::span<const uint8> Bytes)
{
	ASSERT_TRUE(platform::WriteFileBytes(Path, Bytes));
}

// Where a section lies in a snapshot file's bytes (offset, size), from its section table.
inline std::optional<std::pair<size_t, size_t>> FindSection (std::span<const uint8> Bytes, ESnapshotSection Section)
{
	const uint64 count = binary::ReadLittleEndianAt(Bytes, 12u, 4u);
	for (uint64 i = 0u; i < count; ++i)
	{
		const size_t at = SnapshotHeaderBytes + static_cast<size_t>(i) * SnapshotSectionEntryBytes;
		if (binary::ReadLittleEndianAt(Bytes, at, 4u) == static_cast<uint32>(Section))
		{
			return std::pair(static_cast<size_t>(binary::ReadLittleEndianAt(Bytes, at + 8u, 8u)),
				static_cast<size_t>(binary::ReadLittleEndianAt(Bytes, at + 16u, 8u)));
		}
	}
	return std::nullopt;
}

// Flips one byte in the middle of a section of the snapshot file at Path (its checksum then fails).
inline void DamageSection (const std::filesystem::path& Path, ESnapshotSection Section)
{
	std::vector<uint8> bytes = ReadBytes(Path);
	const std::optional<std::pair<size_t, size_t>> section = FindSection(bytes, Section);
	ASSERT_TRUE(section.has_value());
	ASSERT_GT(section->second, 0u);
	bytes[section->first + section->second / 2u] ^= 0x5Au;
	WriteBytes(Path, bytes);
}

inline bool HasDropped (const SSnapshotFileReport& Report, ESnapshotSection Section)
{
	return std::find(Report.DroppedSections.begin(), Report.DroppedSections.end(), Section) != Report.DroppedSections.end();
}
}
