// The query rows of the §5.11 budget table, measured on the generator's project (QueryBenchmarkGenerator.h:
// the exact §5.2 schema through the real store): text parse + compile, an unindexed predicate over one type,
// a 3-predicate row query over one row type, OrderBy + Limit 50 over every asset, a substring search over names,
// transitive referencers, and the store's resident memory (B3's budget, printed). Core-Test runs 10k assets
// with 50k rows; the DISABLED_ tests run 100k, 1M and 100k + 500k rows on demand (--gtest_also_run_disabled_tests).
//
// How a row is judged: ../Registry/BenchmarkBudget.h. Every row judges the whole run a caller waits for
// (SQueryStats::Milliseconds): the scan and the deterministic order §5.6 step 5 gives every result (the path sort of
// an unordered query, the partial sort of OrderBy + Limit); the split is printed beside it, and the first run of the
// query (its per-thread scratch still cold) beside the best. 1M has no budget column: printed only.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

#include "../Registry/BenchmarkBudget.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/RegistryTables.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"
#include "QueryBenchmarkGenerator.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::bench;
using namespace frt::assets::query_bench;


namespace
{
enum class EBudgetColumn : uint8 { Assets10k, Assets100k, Reported };

struct SBudgetCheck
{
	EBudgetColumn Column = EBudgetColumn::Assets10k;

	// At10k / At100k: the row's budgets in each column (the 1M column prints the 100k one, unjudged).
	void Check (const char* Name, double Measured, const SBudget& At10k, const SBudget& At100k, const char* Unit, const char* Note = "")
	{
		const SBudget& budget = Column == EBudgetColumn::Assets10k ? At10k : At100k;
		CheckBudget("[query]", Name, Measured, budget, Unit, Note, Column != EBudgetColumn::Reported);
	}
};

// The best of Rounds runs of a compiled query (the machine is shared: the minimum is the query's own cost).
struct STimedRun
{
	double TotalMs = 1e30;
	double ScanMs = 1e30;
	double SortMs = 1e30;
	double FirstMs = 0.0;   // the first run (cold per-thread scratch)
	uint32 Matched = 0u;
	uint32 Candidates = 0u;
};

STimedRun TimeQuery (const CRegistryView& View, const SAssetQuery& Query, uint32 Rounds = 10u)
{
	SCompiledQuery compiled;
	SDiagnostic error;
	EXPECT_TRUE(View.Compile(Query, compiled, &error)) << Query.Describe().CStr() << ": " << error.Message.CStr();
	STimedRun timed;
	std::vector<SRecordId> records;
	for (uint32 round = 0u; round < Rounds; ++round)
	{
		SQueryStats stats;
		timed.Matched = View.Run(compiled, records, &stats);
		timed.Candidates = stats.Candidates;
		if (round == 0u)
		{
			timed.FirstMs = stats.Milliseconds;
		}
		timed.TotalMs = (std::min)(timed.TotalMs, stats.Milliseconds);
		timed.ScanMs = (std::min)(timed.ScanMs, stats.ScanMilliseconds);
		timed.SortMs = (std::min)(timed.SortMs, stats.SortMilliseconds);
	}
	return timed;
}

STimedRun TimeQuery (const CRegistryView& View, FStringView Text, uint32 Rounds = 10u)
{
	SDiagnostic error;
	const std::optional<SAssetQuery> query = SAssetQuery::Parse(Text, &error);
	EXPECT_TRUE(query.has_value()) << Text.GetData() << ": " << error.Message.CStr();
	return query ? TimeQuery(View, *query, Rounds) : STimedRun {};
}

FString Detail (const STimedRun& Run)
{
	return FString::Format("[{} hits of {} candidates; run {:.3f} ms = scan {:.3f} + sort {:.3f}; first run {:.3f} ms]", Run.Matched, Run.Candidates,
		Run.TotalMs, Run.ScanMs, Run.SortMs, Run.FirstMs);
}

void RunSuite (uint32 Assets, uint32 Rows, EBudgetColumn Column)
{
	SBudgetCheck budget;
	budget.Column = Column;
	CRegistryStore store;
	SGeneratorSpec spec;
	spec.Assets = Assets;
	spec.Rows = Rows;
	const SGeneratorReport report = Generate(store, spec);
	store.CompactGraph();   // as after a snapshot load: the edges in the CSR
	const std::shared_ptr<const CRegistryView> view = store.Acquire();
	const SRegistryTables& tables = GetTables(*view);
	std::printf("[query] generated %u assets (%u textures, %u materials, %u models, %u actions, %u maps, %u raw, %u tables), %u sub-entries "
		"(%u rows), %u files, %u folders, %u edges (%.2f per asset); built %.0f ms, %u commits %.0f ms\n",
		view->GetRecordCount(), report.Textures, report.Materials, report.Models, report.Actions, report.Maps, report.Raws, report.Tables,
		view->GetSubEntryCount(), report.Rows, view->GetFileCount(), view->GetFolderCount(), tables.Graph.LiveEdges,
		static_cast<double>(tables.Graph.LiveEdges) / (std::max)(1u, view->GetRecordCount()), report.BuildMilliseconds, report.Commits,
		report.CommitMilliseconds);
	EXPECT_EQ(view->GetRecordCount(), Assets);

	// Text parse + compile (§5.11: 50 us), averaged over a batch, best batch. The row holds for queries whose ref:/dep:
	// targets are mount paths (a hash lookup each); a target named by $Name is found by scanning every ASSETS and
	// SUBENTRY row at compile time, linear in the registry, outside this budget.
	{
		const FStringView text = "type:Texture Width>=4096 -label:Debug dir:/Project/Textures order:Width desc limit:50";
		constexpr uint32 Batch = 200u;
		double bestUs = 1e30;
		for (uint32 round = 0u; round < 5u; ++round)
		{
			const uint64 start = platform::ReadTimestamp();
			uint32 compiled = 0u;
			for (uint32 i = 0u; i < Batch; ++i)
			{
				const std::optional<SAssetQuery> query = SAssetQuery::Parse(text);
				SCompiledQuery plan;
				compiled += query && view->Compile(*query, plan) ? 1u : 0u;
			}
			EXPECT_EQ(compiled, Batch);
			bestUs = (std::min)(bestUs, platform::TimestampToMilliseconds(platform::ReadTimestamp() - start) * 1000.0 / Batch);
		}
		budget.Check("ParseAndCompileUs", bestUs, SBudget { 50.0, -1.0, 10.0, 12.0 }, SBudget { 50.0, -1.0, 10.0, 20.0 }, "us");
	}

	// An unindexed predicate over one type: one column scan of the texture table (1 in 8 widths match).
	{
		const STimedRun run = TimeQuery(*view, "type:Texture Width>=8192");
		EXPECT_GT(run.Matched, 0u);
		budget.Check("UnindexedPredicateMs", run.TotalMs, SBudget { 0.1, -1.0, 0.04, 0.05 }, SBudget { 0.5, -1.0, 0.3, 0.6 }, "ms", Detail(run).CStr());
	}

	// Three predicates over the rows of one row type (three column bitmasks).
	if (Rows > 0u)
	{
		const STimedRun run = TimeQuery(*view, "rows type:Model MeshCount>=4 VertexCount<50000 TriangleCount>=100000");
		EXPECT_GT(run.Matched, 0u);
		// §5.11 budgets 3 ms over 500k rows; the 10k column's 50k rows are judged pro rata (0.3 ms: a reading of
		// §5.11, which has no 10k figure for this row).
		const double rowBudget = 3.0 * static_cast<double>(Rows) / 500000.0;
		budget.Check("ThreePredicateRowMs", run.TotalMs, SBudget { rowBudget, -1.0, 0.46, 0.55 }, SBudget { rowBudget, -1.0, 3.5, 7.0 }, "ms",
			FString::Format("{} over {} rows", Detail(run), Rows).CStr());
		const STimedRun limited = TimeQuery(*view, "rows type:Model MeshCount>=4 VertexCount<50000 TriangleCount>=100000 limit:50");
		budget.Check("ThreePredicateRowLimit50Ms", limited.TotalMs, SBudget {}, SBudget {}, "ms", Detail(limited).CStr());
	}

	// OrderBy + Limit 50 over every asset record (a partial sort of all hits).
	{
		const STimedRun run = TimeQuery(*view, "order:$Size desc limit:50");
		EXPECT_EQ(run.Matched, Assets);
		budget.Check("OrderByLimit50Ms", run.TotalMs, SBudget { 0.5, -1.0, 0.28, 0.4 }, SBudget { 2.0, -1.0, 1.5, 3.0 }, "ms", Detail(run).CStr());
		const STimedRun tag = TimeQuery(*view, "type:Texture order:Width desc limit:50");
		budget.Check("OrderByTagLimit50Ms", tag.TotalMs, SBudget {}, SBudget {}, "ms", Detail(tag).CStr());
	}

	// A substring over names ($Name, folded) and the text term (name or path).
	{
		SAssetQuery names;
		names.Where("$Name", ECompare::Contains, STagValue::FromName("rock_detail_1"));
		const STimedRun run = TimeQuery(*view, names);
		budget.Check("NameSubstringMs", run.TotalMs, SBudget { 0.2, -1.0, 0.22, 0.3 }, SBudget { 2.0, -1.0, 1.8, 3.6 }, "ms", Detail(run).CStr());
		const STimedRun text = TimeQuery(*view, "rock_detail_1");
		budget.Check("TextTermMs", text.TotalMs, SBudget {}, SBudget {}, "ms", Detail(text).CStr());
		// The same search as a bare loop over ASSETS (no engine): the floor the engine's overhead is measured from.
		const FString needle("rock_detail_1");
		double loopMs = 1e30;
		uint32 loopHits = 0u;
		for (uint32 round = 0u; round < 10u; ++round)
		{
			const uint64 start = platform::ReadTimestamp();
			loopHits = 0u;
			for (uint32 row = 0u; row < tables.Assets.File.Size(); ++row)
			{
				loopHits += tables.Assets.File[row] != InvalidRow && tables.Names.GetFolded(tables.Assets.Name[row]).Contains(needle.View()) ? 1u : 0u;
			}
			loopMs = (std::min)(loopMs, platform::TimestampToMilliseconds(platform::ReadTimestamp() - start));
		}
		EXPECT_EQ(loopHits, run.Matched);
		budget.Check("NameSubstringBareLoopMs", loopMs, SBudget {}, SBudget {}, "ms");
	}

	// Transitive referencers of the most used texture (B3 owns the row; through a query here).
	{
		SAssetQuery referencers;
		referencers.References(GetTexturePath(0u), true);
		const STimedRun run = TimeQuery(*view, referencers);
		EXPECT_GT(run.Matched, 10u);
		budget.Check("TransitiveReferencersMs", run.TotalMs, SBudget { 0.3, -1.0, 0.18, 0.32 }, SBudget { 5.0, -1.0, 2.0, 4.0 }, "ms", Detail(run).CStr());
		const STimedRun deps = TimeQuery(*view, FString::Format("dep:~{}", GetModelPath(0u)).View());
		budget.Check("TransitiveDependenciesMs", deps.TotalMs, SBudget {}, SBudget {}, "ms", Detail(deps).CStr());
	}

	// Graph and system predicates over everything (no budget row; printed for the record).
	{
		budget.Check("IsBrokenRefMs", TimeQuery(*view, "is:broken-ref", 3u).TotalMs, SBudget {}, SBudget {}, "ms");
		budget.Check("IsUnusedTexturesMs", TimeQuery(*view, "type:Texture is:unused", 3u).TotalMs, SBudget {}, SBudget {}, "ms");
		budget.Check("LabelAndFolderMs", TimeQuery(*view, "label:Hero dir:/Project/Materials").TotalMs, SBudget {}, SBudget {}, "ms");
	}

	const SRegistryMemoryStats memory = store.GetMemoryStats();
	std::printf("[query] resident memory %.1f MB (type tables %.1f, sub-entries %.1f, graph %.1f, names %.1f, strings %.1f, indices %.1f); "
		"§5.11: 15 MB at 10k assets, 150 MB at 100k + 500k rows (B3's row)\n", memory.Total / 1048576.0, memory.TypeTables / 1048576.0,
		memory.SubEntries / 1048576.0, memory.Graph / 1048576.0, memory.Names / 1048576.0, memory.Strings / 1048576.0, memory.Indices / 1048576.0);
	// §5.11's 10k memory cell has no row count (the load row's reads "no rows"; RegistryBenchmarkTest and
	// SnapshotBenchmarkTest judge it so): with this suite's 50k rows it is printed against 15 MB, unjudged.
	CheckBudget("[query]", "ResidentMemoryWithRowsMB", memory.Total / 1048576.0, SBudget { Assets == 10000u ? 15.0 : (Rows == 500000u ? 150.0 : -1.0) },
		"MB", FString::Format("({} assets, {} rows; generated, change ring {:.2f} MB)", Assets, Rows, memory.ChangeRing / 1048576.0).CStr(), false);
}
}

TEST(QueryBenchmarkTest, TheQueryEngineMeetsItsBudgetsAt10kAssets)
{
	RunSuite(10000u, 50000u, EBudgetColumn::Assets10k);
}

TEST(QueryBenchmarkTest, DISABLED_TheQueryEngineAt100kAssets)
{
	RunSuite(100000u, 0u, EBudgetColumn::Assets100k);
}

TEST(QueryBenchmarkTest, DISABLED_TheQueryEngineAt100kAssetsAnd500kRows)
{
	RunSuite(100000u, 500000u, EBudgetColumn::Assets100k);
}

TEST(QueryBenchmarkTest, DISABLED_TheQueryEngineAt1MAssets)
{
	RunSuite(1000000u, 0u, EBudgetColumn::Reported);
}

TEST(QueryBenchmarkTest, TheGeneratorIsDeterministic)
{
	// Two generations of the same spec - one in a single delta, one in small chunks - dump identically.
	CRegistryStore whole;
	SGeneratorSpec spec;
	spec.Assets = 400u;
	spec.Rows = 1000u;
	spec.ChunkWeight = 1u << 30u;
	Generate(whole, spec);
	CRegistryStore chunked;
	spec.ChunkWeight = 97u;
	const SGeneratorReport report = Generate(chunked, spec);
	EXPECT_GT(report.Commits, 5u);
	FString a;
	FString b;
	whole.Live().DumpText(a);
	chunked.Live().DumpText(b);
	EXPECT_EQ(a, b);
	EXPECT_EQ(whole.Live().GetRecordCount(), 400u);
	EXPECT_EQ(whole.Live().GetSubEntryCount(), report.Models * 4u + 1000u);
	std::vector<SRecordId> records;
	EXPECT_EQ(whole.Live().RunText("rows", records), 1000u);
	EXPECT_EQ(whole.Live().RunText("is:broken-ref", records), chunked.Live().RunText("is:broken-ref", records));
}
