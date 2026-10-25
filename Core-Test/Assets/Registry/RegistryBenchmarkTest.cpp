// The registry store's rows of the §5.11 budget table at 10k assets: a one-edit commit's main-thread time,
// GUID / AssetId64 / path lookups, transitive referencers, a substring search over names and the resident
// memory of a view. Timings mean something in optimized builds only: Debug measures and prints, never
// judges. In the whole suite the machine is busy, so the checks fail on a clear regression (twice a budget)
// and hold the exact budgets only with FRT_PERF_STRICT=1. The fixture is this test's own small generator
// (textures, materials, models with sub-entries, input files); the full §5.11 generator is G3's.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <random>

#include "Assets/Registry/RegistryTables.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"
#include "RegistryTestFixture.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::registry_test;


namespace
{
constexpr uint32 TextureCount = 6000u;
constexpr uint32 MaterialCount = 2000u;
constexpr uint32 ModelCount = 1000u;
constexpr uint32 OtherCount = 1000u;

FString TexturePath (uint32 I) { return FString::Format("/Project/Textures/Set{}/T_Rock_{}.png", I % 50u, I); }
FString MaterialPath (uint32 I) { return FString::Format("/Project/Materials/Set{}/M_Rock_{}.frtmat.yml", I % 20u, I); }
FString ModelPath (uint32 I) { return FString::Format("/Project/Models/Set{}/SM_Rock_{}.gltf", I % 20u, I); }

// 10k assets: textures; materials with three texture edges (texture 0 is used by every 20th material);
// models with three sub-entries, a .bin companion and edges to two materials; raw and input files.
SRegistryDelta MakeFixture ()
{
	SRegistryDelta delta = MakeMountsDelta();
	for (uint32 i = 0u; i < TextureCount; ++i)
	{
		AddTexture(delta, TexturePath(i), 1024, 1024);
	}
	for (uint32 i = 0u; i < MaterialCount; ++i)
	{
		const FString a = TexturePath(i % 20u == 0u ? 0u : (i * 3u) % TextureCount);
		const FString b = TexturePath((i * 3u + 1u) % TextureCount);
		const FString c = TexturePath((i * 3u + 2u) % TextureCount);
		AddMaterial(delta, MaterialPath(i), { a, b, c });
	}
	for (uint32 i = 0u; i < ModelCount; ++i)
	{
		const FString path = ModelPath(i);
		const FString bin = FString::Format("/Project/Models/Set{}/SM_Rock_{}.bin", i % 20u, i);
		delta.FileUpserts.push_back(MakeFile(path, 5000u));
		delta.FileUpserts.push_back(MakeFile(bin, 50000u, EFileClass::Unclaimed));
		SRecordUpsert model = MakeRecord(path, asset_types::Model, "Model.Gltf");
		model.Tags = { MakeIntTag("MeshCount", 2), MakeIntTag("VertexCount", 4000) };
		model.SubEntries.push_back(SSubEntryDecl { "mesh:LOD0", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 3000) } });
		model.SubEntries.push_back(SSubEntryDecl { "mesh:LOD1", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 1000) } });
		model.SubEntries.push_back(SSubEntryDecl { "material:Rock", 0u, asset_types::Material, ERecordFlags::None, {} });
		model.Edges.push_back(MakePathEdge(bin, EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
		model.Edges.push_back(MakePathEdge(MaterialPath((i * 2u) % MaterialCount), EDepFlags::Hard));
		model.Edges.push_back(MakePathEdge(MaterialPath((i * 2u + 1u) % MaterialCount), EDepFlags::Hard));
		model.Companions = { bin };
		delta.RecordUpserts.push_back(std::move(model));
	}
	for (uint32 i = 0u; i < OtherCount; ++i)
	{
		const FString path = FString::Format("/Project/Data/Set{}/D_{}.frtinputaction", i % 10u, i);
		delta.FileUpserts.push_back(MakeFile(path, 200u));
		SRecordUpsert action = MakeRecord(path, asset_types::InputAction, "Input.Legacy");
		action.Tags = { STagDecl { "ValueType", STagValue::FromName(i % 2u == 0u ? "Button" : "Axis") } };
		delta.RecordUpserts.push_back(std::move(action));
	}
	return delta;
}

struct SBudgetCheck
{
	bool bOptimized = !FRT_BUILD_DEBUG;
	double Slack = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1" ? 1.0 : 2.0;

	void Check (const char* Name, double Measured, double Budget, const char* Unit)
	{
		std::printf("[registry] %-36s %10.3f %s (budget %.3f %s)\n", Name, Measured, Unit, Budget, Unit);
		::testing::Test::RecordProperty(Name, FString::Format("{:.3f}", Measured).CStr());
		if (bOptimized)
		{
			EXPECT_LE(Measured, Budget * Slack) << Name;
		}
	}
};
}

TEST(RegistryBenchmarkTest, TheStoreMeetsItsBudgetsAt10kAssets)
{
	SBudgetCheck budget;
	CRegistryStore store;
	uint64 start = platform::ReadTimestamp();
	store.Commit(MakeFixture());
	const double buildMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	const CRegistryView& live = store.Live();
	ASSERT_EQ(live.GetRecordCount(), TextureCount + MaterialCount + ModelCount + OtherCount);
	std::printf("[registry] fixture: %u records, %u sub-entries, %u files, %u folders, committed in %.1f ms\n",
		live.GetRecordCount(), live.GetSubEntryCount(), live.GetFileCount(), live.GetFolderCount(), buildMs);
	store.CompactGraph();

	// One edit: a material re-analyzed with a changed texture. Median of 51 commits.
	std::vector<double> commits;
	for (uint32 i = 0u; i < 51u; ++i)
	{
		SRegistryDelta edit;
		AddMaterial(edit, MaterialPath(7u), { TexturePath(i), TexturePath(i + 1u), TexturePath(i + 2u) });
		store.Commit(std::move(edit));
		commits.push_back(store.GetLastCommitStats().Milliseconds);
	}
	std::sort(commits.begin(), commits.end());
	budget.Check("OneEditCommitMs", commits[commits.size() / 2u], 2.0, "ms");

	const std::shared_ptr<const CRegistryView> view = store.Acquire();
	std::vector<SAssetId> ids;
	std::vector<SAssetId64> ids64;
	std::vector<FString> paths;
	view->ForEachRecord([&] (SRecordId Record)
	{
		SAssetData data;
		view->GetRecord(Record, data);
		ids.push_back(data.Id);
		ids64.push_back(data.Id64);
		paths.push_back(data.MountPath);
	});
	std::mt19937 random(42u);
	std::shuffle(ids.begin(), ids.end(), random);
	std::shuffle(ids64.begin(), ids64.end(), random);
	std::shuffle(paths.begin(), paths.end(), random);

	const auto timeLoop = [] (uint32 Count, auto&& Body)
	{
		double best = 1e30;
		for (uint32 round = 0u; round < 5u; ++round)
		{
			const uint64 begin = platform::ReadTimestamp();
			uint32 found = 0u;
			for (uint32 i = 0u; i < Count; ++i)
			{
				found += Body(i) ? 1u : 0u;
			}
			const double ns = platform::TimestampToMilliseconds(platform::ReadTimestamp() - begin) * 1e6 / Count;
			EXPECT_EQ(found, Count);
			best = (std::min)(best, ns);
		}
		return best;
	};
	const uint32 count = static_cast<uint32>(ids.size());
	budget.Check("GuidLookupNs", timeLoop(count, [&] (uint32 I) { return view->Find(ids[I]).has_value(); }), 50.0, "ns");
	budget.Check("AssetId64LookupNs", timeLoop(count, [&] (uint32 I) { return view->Find(ids64[I]).has_value(); }), 50.0, "ns");
	budget.Check("PathLookupUs", timeLoop(count, [&] (uint32 I) { return view->FindByPath(paths[I]).has_value(); }) / 1000.0, 0.5, "us");

	// Transitive referencers of the most used texture: its materials, and the models that use those.
	const SRecordId texture = view->FindByPath(TexturePath(0u)).value_or(SRecordId {});
	std::vector<SRecordId> referencers;
	double referencersMs = 1e30;
	for (uint32 round = 0u; round < 5u; ++round)
	{
		const SRecordId roots[] = { texture };
		const uint64 begin = platform::ReadTimestamp();
		view->GetReferencerClosure(roots, EDepFlags {}, referencers);
		referencersMs = (std::min)(referencersMs, platform::TimestampToMilliseconds(platform::ReadTimestamp() - begin));
	}
	EXPECT_GT(referencers.size(), 100u);
	std::printf("[registry] transitive referencers of a shared texture: %zu records\n", referencers.size());
	budget.Check("TransitiveReferencersMs", referencersMs, 0.3, "ms");

	// A substring over every record's name, through the folded shadows of NAMES (what a query's Text does).
	const SRegistryTables& tables = GetTables(*view);
	double substringMs = 1e30;
	uint32 hits = 0u;
	for (uint32 round = 0u; round < 5u; ++round)
	{
		const uint64 begin = platform::ReadTimestamp();
		hits = 0u;
		const uint32 rows = tables.Assets.File.Size();
		for (uint32 row = 0u; row < rows; ++row)
		{
			if (tables.Assets.File[row] != InvalidRow && tables.Names.GetFolded(tables.Assets.Name[row]).Contains("rock_12"))
			{
				++hits;
			}
		}
		substringMs = (std::min)(substringMs, platform::TimestampToMilliseconds(platform::ReadTimestamp() - begin));
	}
	EXPECT_GT(hits, 0u);
	budget.Check("NameSubstringMs", substringMs, 0.2, "ms");

	const SRegistryMemoryStats memory = view->GetMemoryStats();
	std::printf("[registry] resident: files %.2f, folders %.2f, rule files %.2f, assets %.2f, sub-entries %.2f, type tables %.2f, "
		"graph %.2f, names %.2f, strings %.2f, labels %.2f, problems %.2f, indices %.2f MB\n",
		memory.Files / 1048576.0, memory.Folders / 1048576.0, memory.RuleFiles / 1048576.0, memory.Assets / 1048576.0,
		memory.SubEntries / 1048576.0, memory.TypeTables / 1048576.0, memory.Graph / 1048576.0, memory.Names / 1048576.0,
		memory.Strings / 1048576.0, memory.Labels / 1048576.0, memory.Problems / 1048576.0, memory.Indices / 1048576.0);
	budget.Check("ResidentMemoryMB", memory.Total / 1048576.0, 15.0, "MB");
}
