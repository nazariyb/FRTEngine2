// The registry store's rows of the §5.11 budget table: a one-edit commit's main-thread time, GUID / AssetId64 /
// path lookups, transitive referencers, a substring search over names and the resident memory of the store (its
// view's tables, the writer's state and a full change ring). Timings mean something in optimized builds only:
// Debug measures and prints, never judges. In the whole suite the machine is busy, so the checks fail on a clear
// regression (twice a budget) and hold the exact budgets only with FRT_PERF_STRICT=1.
//
// Two fixtures. TheStoreMeetsItsBudgetsAt10kAssets uses this test's own small generator (textures, materials,
// models with sub-entries, input files) with §5.3's density of about 4.5 edges per asset and rule labels on every
// record. The scale tests run G3's §5.11 generator (Query/QueryBenchmarkGenerator.h, the exact §5.2 schema): 10k
// assets without rows in Core-Test (the 10k memory cell is "no rows", as the load row says), and on demand
// (--gtest_also_run_disabled_tests) 100k, 100k + 500k rows (every row field a column) and 1M + 5M rows, whose
// memory §5.11 reports without a budget. (No M3 type has a String key, so there are no STRINGS values.)
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <random>

#include "../Query/QueryBenchmarkGenerator.h"
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

FString ActionPath (uint32 I) { return FString::Format("/Project/Data/Set{}/D_{}.frtinputaction", I % 10u, I); }

// Two rule labels per record, as a mount file's Labels rules would give them.
std::vector<FString> MakeLabels (FStringView Kind, uint32 Set)
{
	return { FString(Kind), FString::Format("Set{}", Set) };
}

// 10k assets, 45k edges: textures; materials with twelve texture edges (texture 0 is used by every 20th
// material); models with three sub-entries, a .bin companion and edges to it and fourteen materials;
// input actions with six edges to other actions.
SRegistryDelta MakeFixture ()
{
	SRegistryDelta delta = MakeMountsDelta();
	for (uint32 i = 0u; i < TextureCount; ++i)
	{
		AddTexture(delta, TexturePath(i), 1024, 1024);
		delta.RecordUpserts.back().RuleLabels = MakeLabels("Texture", i % 50u);
	}
	for (uint32 i = 0u; i < MaterialCount; ++i)
	{
		delta.FileUpserts.push_back(MakeFile(MaterialPath(i)));
		SRecordUpsert material = MakeRecord(MaterialPath(i), asset_types::Material, "Material.Legacy");
		material.RuleLabels = MakeLabels("Material", i % 20u);
		for (uint32 k = 0u; k < 12u; ++k)
		{
			const uint32 texture = k == 0u && i % 20u == 0u ? 0u : (i * 12u + k + 1u) % TextureCount;
			material.Edges.push_back(MakePathEdge(TexturePath(texture), EDepFlags::Build | EDepFlags::Path, FString::Format("slots[{}].texture", k)));
		}
		delta.RecordUpserts.push_back(std::move(material));
	}
	for (uint32 i = 0u; i < ModelCount; ++i)
	{
		const FString path = ModelPath(i);
		const FString bin = FString::Format("/Project/Models/Set{}/SM_Rock_{}.bin", i % 20u, i);
		delta.FileUpserts.push_back(MakeFile(path, 5000u));
		delta.FileUpserts.push_back(MakeFile(bin, 50000u, EFileClass::Unclaimed));
		SRecordUpsert model = MakeRecord(path, asset_types::Model, "Model.Gltf");
		model.RuleLabels = MakeLabels("Model", i % 20u);
		model.Tags = { MakeIntTag("MeshCount", 2), MakeIntTag("VertexCount", 4000) };
		model.SubEntries.push_back(SSubEntryDecl { "mesh:LOD0", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 3000) } });
		model.SubEntries.push_back(SSubEntryDecl { "mesh:LOD1", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 1000) } });
		model.SubEntries.push_back(SSubEntryDecl { "material:Rock", 0u, asset_types::Material, ERecordFlags::None, {} });
		model.Edges.push_back(MakePathEdge(bin, EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
		for (uint32 k = 0u; k < 14u; ++k)
		{
			model.Edges.push_back(MakePathEdge(MaterialPath((i * 14u + k) % MaterialCount), EDepFlags::Hard, FString::Format("materials[{}]", k)));
		}
		model.Companions = { bin };
		delta.RecordUpserts.push_back(std::move(model));
	}
	for (uint32 i = 0u; i < OtherCount; ++i)
	{
		const FString path = ActionPath(i);
		delta.FileUpserts.push_back(MakeFile(path, 200u));
		SRecordUpsert action = MakeRecord(path, asset_types::InputAction, "Input.Legacy");
		action.RuleLabels = MakeLabels("Input", i % 10u);
		action.Tags = { STagDecl { "ValueType", STagValue::FromName(i % 2u == 0u ? "Button" : "Axis") } };
		for (uint32 k = 1u; k <= 6u; ++k)
		{
			action.Edges.push_back(MakePathEdge(ActionPath((i + k * 37u) % OtherCount), EDepFlags::Path));
		}
		delta.RecordUpserts.push_back(std::move(action));
	}
	return delta;
}

struct SBudgetCheck
{
	bool bOptimized = !FRT_BUILD_DEBUG;
	double Slack = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1" ? 1.0 : 2.0;

	// Budget < 0: printed only (no §5.11 figure at this size).
	void Check (const char* Name, double Measured, double Budget, const char* Unit)
	{
		if (Budget < 0.0)
		{
			std::printf("[registry] %-36s %10.3f %s (not budgeted)\n", Name, Measured, Unit);
		}
		else
		{
			std::printf("[registry] %-36s %10.3f %s (budget %.3f %s)\n", Name, Measured, Unit, Budget, Unit);
		}
		::testing::Test::RecordProperty(Name, FString::Format("{:.3f}", Measured).CStr());
		if (bOptimized && Budget >= 0.0)
		{
			EXPECT_LE(Measured, Budget * Slack) << Name;
		}
	}
};

// The best of five timed loops over Count items, in nanoseconds per item; every item must be found.
template <typename TBody>
double TimeLookups (uint32 Count, TBody&& Body)
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
}

void PrintMemory (const SRegistryMemoryStats& Memory)
{
	std::printf("[registry] resident: files %.2f, folders %.2f, rule files %.2f, assets %.2f, sub-entries %.2f, type tables %.2f, "
		"graph %.2f, names %.2f, strings %.2f, labels %.2f, problems %.2f, indices %.2f, writer %.2f, change ring %.2f MB\n",
		Memory.Files / 1048576.0, Memory.Folders / 1048576.0, Memory.RuleFiles / 1048576.0, Memory.Assets / 1048576.0,
		Memory.SubEntries / 1048576.0, Memory.TypeTables / 1048576.0, Memory.Graph / 1048576.0, Memory.Names / 1048576.0,
		Memory.Strings / 1048576.0, Memory.Labels / 1048576.0, Memory.Problems / 1048576.0, Memory.Indices / 1048576.0,
		Memory.Writer / 1048576.0, Memory.ChangeRing / 1048576.0);
}

// The §5.11 budgets of one scale; < 0: printed only.
struct SScaleBudgets
{
	double OneEditCommitMs = 2.0;
	double GuidLookupNs = 50.0;
	double PathLookupUs = 0.5;
	double ResidentMemoryMB = -1.0;
	bool bTimings = true;          // false: every timing printed only (1M + 5M rows, which §5.11 does not budget)
};

/**
 * The store's rows at one scale of G3's generator: the generated project committed, the graph compacted (as after a
 * snapshot load), then a one-edit commit (a material re-analyzed with other textures, the median of 51; journal
 * appends are H3's), GUID / AssetId64 / path lookups over every asset record in a shuffled order (and AssetId64 over
 * a fifth of the sub-entries, printed), and the resident memory (§5.11: the view's tables, the writer's state and
 * the change ring).
 */
void MeasureStoreAtScale (uint32 Assets, uint32 Rows, const SScaleBudgets& Budgets)
{
	SBudgetCheck budget;
	const auto timed = [&] (double Budget) { return Budgets.bTimings ? Budget : -1.0; };
	CRegistryStore store;
	query_bench::SGeneratorSpec spec;
	spec.Assets = Assets;
	spec.Rows = Rows;
	const query_bench::SGeneratorReport report = query_bench::Generate(store, spec);
	store.CompactGraph();
	std::printf("[registry] generated %u assets, %u rows: %u records, %u sub-entries, %u files, %u edges; %u commits %.0f ms\n", Assets, Rows,
		store.Live().GetRecordCount(), store.Live().GetSubEntryCount(), store.Live().GetFileCount(), GetTables(store.Live()).Graph.LiveEdges,
		report.Commits, report.CommitMilliseconds);
	ASSERT_EQ(store.Live().GetRecordCount(), Assets);

	// One edit: a material re-analyzed with a changed texture list. Median of 51 commits.
	std::vector<double> commits;
	const FString material = query_bench::GetMaterialPath(7u);
	for (uint32 i = 0u; i < 51u; ++i)
	{
		SRegistryDelta edit;
		const FString textures[] = { query_bench::GetTexturePath(i), query_bench::GetTexturePath(i + 1u), query_bench::GetTexturePath(i + 2u) };
		AddMaterial(edit, material, { textures[0].View(), textures[1].View(), textures[2].View() });
		store.Commit(std::move(edit));
		commits.push_back(store.GetLastCommitStats().Milliseconds);
	}
	std::sort(commits.begin(), commits.end());
	budget.Check("OneEditCommitMs", commits[commits.size() / 2u], timed(Budgets.OneEditCommitMs), "ms");

	const std::shared_ptr<const CRegistryView> view = store.Acquire();
	const SRegistryTables& tables = GetTables(*view);
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
	std::vector<SAssetId64> subIds64;
	for (uint32 sub = 0u; sub < tables.SubEntries.Owner.Size(); sub += 5u)
	{
		if (tables.SubEntries.Owner[sub] != InvalidRow)
		{
			subIds64.push_back(SAssetId64 { tables.SubEntries.AssetId64[sub] });
		}
	}
	std::mt19937 random(42u);
	std::shuffle(ids.begin(), ids.end(), random);
	std::shuffle(ids64.begin(), ids64.end(), random);
	std::shuffle(paths.begin(), paths.end(), random);
	std::shuffle(subIds64.begin(), subIds64.end(), random);
	const uint32 count = static_cast<uint32>(ids.size());
	budget.Check("GuidLookupNs", TimeLookups(count, [&] (uint32 I) { return view->Find(ids[I]).has_value(); }), timed(Budgets.GuidLookupNs), "ns");
	budget.Check("AssetId64LookupNs", TimeLookups(count, [&] (uint32 I) { return view->Find(ids64[I]).has_value(); }), timed(Budgets.GuidLookupNs), "ns");
	if (!subIds64.empty())
	{
		budget.Check("SubEntryId64LookupNs", TimeLookups(static_cast<uint32>(subIds64.size()), [&] (uint32 I) { return view->Find(subIds64[I]).has_value(); }),
			-1.0, "ns");
	}
	budget.Check("PathLookupUs", TimeLookups(count, [&] (uint32 I) { return view->FindByPath(paths[I]).has_value(); }) / 1000.0,
		timed(Budgets.PathLookupUs), "us");

	const SRegistryMemoryStats memory = store.GetMemoryStats();
	PrintMemory(memory);
	budget.Check("ResidentMemoryMB", memory.Total / 1048576.0, Budgets.ResidentMemoryMB, "MB");
}
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
	std::printf("[registry] fixture: %u records, %u sub-entries, %u files, %u folders, %u edges, committed in %.1f ms\n",
		live.GetRecordCount(), live.GetSubEntryCount(), live.GetFileCount(), live.GetFolderCount(), GetTables(live).Graph.LiveEdges, buildMs);
	store.CompactGraph();

	// Full re-evaluations (a mount rules edit re-upserts every record) until the change ring has wrapped:
	// unchanged records reuse their rows, lists and edges, so nothing piles up; F3 compacts when due.
	const uint32 rounds = FRT_BUILD_DEBUG ? 1u : 6u;
	double reupsertMs = 0.0;
	for (uint32 round = 0u; round < rounds; ++round)
	{
		store.Commit(MakeFixture());
		reupsertMs = (std::max)(reupsertMs, store.GetLastCommitStats().Milliseconds);
		if (store.NeedsCompaction())
		{
			store.Compact();
		}
	}
	std::printf("[registry] %u full re-upserts, the slowest %.1f ms; change ring %s\n", rounds, reupsertMs,
		store.ForEachChangeSince(0u, [] (const SRecordChange&) {}) ? "not full" : "full");

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

	const uint32 count = static_cast<uint32>(ids.size());
	budget.Check("GuidLookupNs", TimeLookups(count, [&] (uint32 I) { return view->Find(ids[I]).has_value(); }), 50.0, "ns");
	budget.Check("AssetId64LookupNs", TimeLookups(count, [&] (uint32 I) { return view->Find(ids64[I]).has_value(); }), 50.0, "ns");
	budget.Check("PathLookupUs", TimeLookups(count, [&] (uint32 I) { return view->FindByPath(paths[I]).has_value(); }) / 1000.0, 0.5, "us");

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

	const SRegistryMemoryStats memory = store.GetMemoryStats();
	PrintMemory(memory);
	EXPECT_EQ(memory.Total, ComputeMemoryStats(GetTables(store.Live())).Total + memory.Writer + memory.ChangeRing);
	budget.Check("ResidentMemoryMB", memory.Total / 1048576.0, 15.0, "MB");

	// For the snapshot work (H3 owns its budgets): exporting the tables and rebuilding a store from them.
	start = platform::ReadTimestamp();
	SRegistryImage image = view->ExportImage({});
	const double exportMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	CRegistryStore adopted;
	adopted.AdoptImage(std::move(image), view->GetRevision());
	std::printf("[registry] image: export %.2f ms, adopt (every index rebuilt, targets resolved, CSR built) %.2f ms\n",
		exportMs, adopted.GetLastCommitStats().Milliseconds);
	EXPECT_EQ(adopted.Live().GetRecordCount(), view->GetRecordCount());
}

TEST(RegistryBenchmarkTest, TheStoreMeetsItsBudgetsAt10kGeneratedAssets)
{
	SScaleBudgets budgets;
	budgets.ResidentMemoryMB = 15.0;
	MeasureStoreAtScale(10000u, 0u, budgets);
}

TEST(RegistryBenchmarkTest, DISABLED_TheStoreAt100kAssets)
{
	MeasureStoreAtScale(100000u, 0u, SScaleBudgets {});
}

TEST(RegistryBenchmarkTest, DISABLED_TheStoreAt100kAssetsAnd500kRows)
{
	SScaleBudgets budgets;
	budgets.ResidentMemoryMB = 150.0;
	MeasureStoreAtScale(100000u, 500000u, budgets);
}

TEST(RegistryBenchmarkTest, DISABLED_TheStoreAt1MAssetsAnd5MRows)
{
	SScaleBudgets budgets;
	budgets.bTimings = false;
	MeasureStoreAtScale(1000000u, 5000000u, budgets);
}
