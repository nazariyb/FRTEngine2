// The snapshot rows of the §5.11 budget table, measured on G3's generator project (Query/QueryBenchmarkGenerator.h:
// the exact §5.2 schema through the real store): a full snapshot write from a view (export and encode on the worker,
// and the whole row as §5.11 words it, the flushed atomic write to disk included), a journal append per commit, the
// journal restart after a full write (the owner thread's part), and the load a start-up waits for - snapshot and
// journal read, checked, decoded and adopted with every index rebuilt (§5.11 "snapshot + journal load, all
// indices"), on the engine's worker count as at start-up. Beside the best load: the first load in the process (a
// fresh process pays page faults the later rounds do not), the same load on one thread, the file read past the OS
// cache (a cold read's approximation: the data, not the metadata), the engine's and the project's files loaded
// together in both orders, and the loaded store's resident memory, right after the load and with its change ring
// full. Core-Test runs 10k assets; the DISABLED_ tests run 100k and 100k + 500k rows on demand
// (--gtest_also_run_disabled_tests). How a row is judged: ../Registry/BenchmarkBudget.h.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "../Query/QueryBenchmarkGenerator.h"
#include "../Registry/BenchmarkBudget.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"
#include "SnapshotTestUtils.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::bench;
using namespace frt::assets::snapshot_test;


namespace
{
struct SSnapshotBudgets
{
	SBudget Load;          // snapshot + journal load, all indices (on the pool)
	SBudget WriteCpu;      // full snapshot write from a view: export + encode (worker)
	SBudget WriteTotal;    // ... with the flushed atomic write to disk: §5.11's row as worded
	SBudget Append;        // journal append per commit
	SBudget Restart;       // journal restart after a full write, the owner thread's part (not in §5.11)
	SBudget Memory;        // resident memory of the loaded store with a full change ring
};

void Check (const char* Name, double Measured, const SBudget& Budget, const char* Unit, const char* Note = "")
{
	CheckBudget("[snapshot]", Name, Measured, Budget, Unit, Note);
}

FString DescribeLoad (const SSnapshotLoadResult& Load)
{
	return FString::Format("read {:.2f} + decode {:.2f} + adopt {:.2f} ({}) + journal replay {:.2f} ms", Load.ReadMs, Load.DecodeMs, Load.AdoptMs,
		Load.bAdoptedInBulk ? "bulk" : "replayed", Load.ReplayMs);
}

void MeasureSnapshot (const std::filesystem::path& Dir, uint32 Assets, uint32 Rows, const SSnapshotBudgets& Budgets)
{
	CRegistryStore store;
	query_bench::SGeneratorSpec spec;
	spec.Assets = Assets;
	spec.Rows = Rows;
	query_bench::Generate(store, spec);
	std::printf("[snapshot] generated %u assets, %u rows: %u records, %u sub-entries, %u files, %u edges\n", Assets, Rows,
		store.Live().GetRecordCount(), store.Live().GetSubEntryCount(), store.Live().GetFileCount(), GetTables(store.Live()).Graph.LiveEdges);
	// The engine's content for the two-file load: textures under /Engine, written to a file of their own.
	{
		SRegistryDelta engine;
		for (uint32 i = 0u; i < (std::max)(200u, Assets / 50u); ++i)
		{
			AddTexture(engine, FString::Format("/Engine/Textures/S{}/T_Engine_{}.png", i / 48u, i), 64 << (i % 4u), 64);
		}
		store.Commit(std::move(engine));
	}
	const MountId projectMounts[] = { ProjectMountId };
	const MountId engineMounts[] = { EngineMountId };
	const std::filesystem::path path = Dir / "AssetRegistry.snapshot";
	const std::filesystem::path enginePath = Dir / "Engine.snapshot";
	const SGuid projectId = SGuid::NewRandom();
	ASSERT_TRUE(WriteSnapshot(store.Live(), engineMounts, enginePath, projectId, 0u));

	// A full write of the project from a view: the best of three (the machine is shared).
	SSnapshotWriteStats write;
	write.ExportMs = 1e30;
	for (uint32 round = 0u; round < 3u; ++round)
	{
		SSnapshotWriteStats stats;
		FString error;
		ASSERT_TRUE(WriteSnapshot(*store.Acquire(), projectMounts, path, projectId, 0u, &error, &stats)) << error.CStr();
		if (stats.ExportMs + stats.EncodeMs < write.ExportMs + write.EncodeMs)
		{
			write = stats;
		}
	}
	std::printf("[snapshot] file: %.2f MB; export %.2f ms, encode %.2f ms, write to disk %.2f ms\n", write.Bytes / 1048576.0, write.ExportMs,
		write.EncodeMs, write.WriteMs);
	Check("SnapshotWriteCpuMs", write.ExportMs + write.EncodeMs, Budgets.WriteCpu, "ms", "(export + encode, worker)");
	Check("SnapshotWriteTotalMs", write.ExportMs + write.EncodeMs + write.WriteMs, Budgets.WriteTotal, "ms",
		FString::Format("(the row as 5.11 words it: export + encode + flushed atomic write {:.2f} ms)", write.WriteMs).CStr());

	// The journal: one-edit commits as F3 appends them (journal first), median of 101.
	CRegistryJournal journal;
	FString error;
	ASSERT_TRUE(journal.Open(GetJournalPath(path), projectId, store.GetRevision(), { "Project" }, &error)) << error.CStr();
	std::vector<double> appends;
	for (uint32 i = 0u; i < 101u; ++i)
	{
		SRegistryDelta edit;
		AddMaterial(edit, query_bench::GetMaterialPath(7u), { query_bench::GetTexturePath(i), query_bench::GetTexturePath(i + 1u) });
		ASSERT_TRUE(journal.Append(edit, store.GetRevision() + 1u, &error)) << error.CStr();
		appends.push_back(journal.GetStats().LastAppendMs);
		store.Commit(std::move(edit));
	}
	journal.Close();
	std::sort(appends.begin(), appends.end());
	Check("JournalAppendMs", appends[appends.size() / 2u], Budgets.Append, "ms", "(median of 101 one-edit commits)");

	// The load: the project's snapshot + journal into a fresh store on the engine's worker count (at start-up the
	// load runs on a worker and splits its work over the pool, RegistryParallel.h): the first load in this process
	// and the best of three.
	jobs::CWorkerPool pool;
	const SSnapshotSource source { path, projectId };
	const SSnapshotSource engineSource { enginePath, projectId };
	SSnapshotLoadResult first;
	SSnapshotLoadResult best;
	best.TotalMs = 1e30;
	for (uint32 round = 0u; round < 3u; ++round)
	{
		CRegistryStore loaded;
		SSnapshotLoadResult result = LoadSnapshots(loaded, std::span(&source, 1u), {}, &pool);
		ASSERT_EQ(result.Files[0].State, ESnapshotFileState::Loaded) << result.Files[0].Message.CStr();
		ASSERT_EQ(result.Files[0].JournalCommits, 101u);
		ASSERT_EQ(loaded.Live().GetRecordCount(), Assets);
		if (round == 0u)
		{
			first = result;
		}
		if (result.TotalMs < best.TotalMs)
		{
			best = std::move(result);
		}
	}
	std::printf("[snapshot] best load: %s\n", DescribeLoad(best).CStr());
	Check("SnapshotLoadMs", best.TotalMs, Budgets.Load, "ms", FString::Format("(best of 3 on {} workers)", pool.GetThreadCount()).CStr());
	Check("SnapshotLoadFirstMs", first.TotalMs, SBudget {}, "ms", FString::Format("(the first load in this process: {})", DescribeLoad(first)).CStr());
	{
		double oneThreadMs = 1e30;
		for (uint32 round = 0u; round < 3u; ++round)
		{
			CRegistryStore loaded;
			oneThreadMs = (std::min)(oneThreadMs, LoadSnapshots(loaded, std::span(&source, 1u)).TotalMs);
		}
		Check("SnapshotLoadOneThreadMs", oneThreadMs, SBudget {}, "ms", "(the same load without a pool, best of 3)");
	}
	{
		double uncachedMs = 1e30;
		for (uint32 round = 0u; round < 3u; ++round)
		{
			const uint64 start = platform::ReadTimestamp();
			ASSERT_TRUE(platform::ReadFileBytesUncached(path).has_value());
			uncachedMs = (std::min)(uncachedMs, platform::TimestampToMilliseconds(platform::ReadTimestamp() - start));
		}
		Check("SnapshotReadUncachedMs", uncachedMs, SBudget {}, "ms",
			FString::Format("(the file read past the OS cache: a cold read of its data; the load above read it warm in {:.2f} ms)", best.ReadMs).CStr());
	}

	// The engine's and the project's files together, as at start-up, in either order (the larger image is merged
	// first either way; the bulk adoption merges the two sorted runs of files): equal to the store they were written
	// from. The rounds interleave the project's file alone with both pairs, so all three see the same process
	// state: later loads in a process run slower than its first ones (its heap has grown and been handed back).
	{
		const SSnapshotSource engineFirst[] = { engineSource, source };
		const SSnapshotSource projectFirst[] = { source, engineSource };
		const std::span<const SSnapshotSource> configs[] = { projectFirst, engineFirst, std::span(&source, 1u) };
		SSnapshotLoadResult bestOf[3];
		for (SSnapshotLoadResult& result : bestOf)
		{
			result.TotalMs = 1e30;
		}
		for (uint32 round = 0u; round < 3u; ++round)
		{
			for (uint32 config = 0u; config < 3u; ++config)
			{
				CRegistryStore loaded;
				SSnapshotLoadResult result = LoadSnapshots(loaded, configs[config], {}, &pool);
				ASSERT_TRUE(result.bAdoptedInBulk);
				if (round == 0u && config < 2u)
				{
					EXPECT_EQ(Dump(loaded.Live()), Dump(store.Live()));
				}
				if (result.TotalMs < bestOf[config].TotalMs)
				{
					bestOf[config] = std::move(result);
				}
			}
		}
		Check("SnapshotLoadAloneLaterMs", bestOf[2].TotalMs, SBudget {}, "ms",
			FString::Format("(AssetRegistry.snapshot alone, in the rounds beside the pairs: {})", DescribeLoad(bestOf[2])).CStr());
		Check("SnapshotLoadEngineFirstMs", bestOf[1].TotalMs, SBudget {}, "ms",
			FString::Format("(Engine.snapshot, then AssetRegistry.snapshot: {})", DescribeLoad(bestOf[1])).CStr());
		Check("SnapshotLoadProjectFirstMs", bestOf[0].TotalMs, SBudget {}, "ms",
			FString::Format("(AssetRegistry.snapshot, then Engine.snapshot: {})", DescribeLoad(bestOf[0])).CStr());
	}

	// The loaded store's resident memory (§5.11 counts the change ring full): right after the load, as start-up
	// leaves it, and after re-upserting the project until the ring has wrapped.
	{
		CRegistryStore loaded;
		ASSERT_TRUE(LoadSnapshots(loaded, std::span(&source, 1u), {}, &pool).AnyLoaded());
		const double afterLoadMB = loaded.GetMemoryStats().Total / 1048576.0;
		const uint64 loadedRevision = loaded.GetRevision();   // the ring reports nothing older (AdoptImage)
		for (uint32 round = 0u; round < 16u && loaded.ForEachChangeSince(loadedRevision, [] (const SRecordChange&) {}); ++round)
		{
			query_bench::Generate(loaded, spec);
		}
		const SRegistryMemoryStats memory = loaded.GetMemoryStats();
		Check("LoadedMemoryMB", afterLoadMB, SBudget {}, "MB", "(right after the load)");
		Check("LoadedMemoryFullRingMB", memory.Total / 1048576.0, Budgets.Memory, "MB",
			FString::Format("(change ring full: {:.2f} MB of it)", memory.ChangeRing / 1048576.0).CStr());
	}

	// The journal restart after a full write (§5.3: writing never stalls the owner thread; F3's Pump has 2 ms a
	// frame): BeginRestart where the view is taken, Prepare on the worker after the snapshot (the new journal
	// written, renamed over the old one, opened), FinishRestart in the completion. 101 commits appended, the view
	// at the 96th, the last 5 made while the worker ran (3 before its rename, 2 after it). A journal file of its
	// own, so the snapshot's stays as the load read it. The check holds the owner thread's whole part - work and
	// file (the handle swap, the old handle closed, one append of the commits since BeginRestart) - the worst of 5
	// rounds; the worker's part and the one-call Restart (all of it on the owner thread) are printed.
	double ownerMs = 0.0;
	double ownerFileMs = 0.0;
	double workerMs = 0.0;
	double oneCallMs = 0.0;
	const auto makeEdit = [] (uint32 I)
	{
		SRegistryDelta edit;
		AddMaterial(edit, query_bench::GetMaterialPath(7u), { query_bench::GetTexturePath(I), query_bench::GetTexturePath(I + 1u) });
		return edit;
	};
	for (uint32 round = 0u; round < 5u; ++round)
	{
		CRegistryJournal restarted;
		ASSERT_TRUE(restarted.Open(Dir / "Restart.journal", projectId, std::nullopt, {}, &error)) << error.CStr();
		for (uint32 i = 0u; i < 96u; ++i)
		{
			ASSERT_TRUE(restarted.Append(makeEdit(i), 1000u + i, &error)) << error.CStr();
		}
		CJournalRestart restart = restarted.BeginRestart(1000u + 95u);
		for (uint32 i = 96u; i < 99u; ++i)
		{
			ASSERT_TRUE(restarted.Append(makeEdit(i), 1000u + i, &error)) << error.CStr();
		}
		ASSERT_TRUE(restart.Prepare(&error)) << error.CStr();
		for (uint32 i = 99u; i < 101u; ++i)
		{
			ASSERT_TRUE(restarted.Append(makeEdit(i), 1000u + i, &error)) << error.CStr();
		}
		ASSERT_TRUE(restarted.FinishRestart(std::move(restart), &error)) << error.CStr();
		const SJournalStats& stats = restarted.GetStats();
		ownerMs = (std::max)(ownerMs, stats.LastRestartMs);
		ownerFileMs = (std::max)(ownerFileMs, stats.LastRestartFileMs);
		workerMs = (std::max)(workerMs, stats.LastRestartWorkerMs);
		restarted.Close();
		EXPECT_EQ(ReadJournal(Dir / "Restart.journal").Deltas.size(), 5u);

		CRegistryJournal oneCall;
		ASSERT_TRUE(oneCall.Open(Dir / "OneCall.journal", projectId, std::nullopt, {}, &error)) << error.CStr();
		for (uint32 i = 0u; i < 101u; ++i)
		{
			ASSERT_TRUE(oneCall.Append(makeEdit(i), 1000u + i, &error)) << error.CStr();
		}
		ASSERT_TRUE(oneCall.Restart(1000u + 95u, &error)) << error.CStr();
		oneCallMs = (std::max)(oneCallMs, oneCall.GetStats().LastRestartMs);
	}
	Check("JournalRestartOwnerMs", ownerMs, Budgets.Restart, "ms", FString::Format("(owner thread, worst of 5; file work {:.3f} ms of it)", ownerFileMs).CStr());
	Check("JournalRestartWorkerMs", workerMs, SBudget {}, "ms", "(Prepare on the worker, worst of 5)");
	Check("JournalRestartOneCallMs", oneCallMs, SBudget {}, "ms", "(Restart, all on the owner thread, worst of 5; a full write does not use it)");
}
}

/**
 * §5.11's write row as worded includes the flushed atomic write, which the CPU figure leaves out: MISSED at 10k and
 * 100k with it (proposed for the written revision: the row as worker CPU, the flushed write reported beside it).
 * The load cannot meet §5.11's 5 / 30 / 120 ms (a thin-schema figure): the exact schema's load decodes every section
 * and rebuilds every index, the CSR, the file order and every target's resolution; on the worker pool the
 * independent parts run at once, but the records are written by one thread (they append to shared pools in record
 * order). The proposed revisions are for an optimized build (Release): Development, the build that runs the live
 * registry, is about twice as slow and is reported beside them. Measured (i7-13700K, affinity 0xFFFFFC, warm; the
 * figures of P3's review): see the M3 report.
 */
TEST_F(SnapshotTest, SnapshotAndJournalMeetTheirBudgetsAt10kAssets)
{
	SSnapshotBudgets budgets;
	budgets.Load = SBudget { 5.0, 20.0, 20.0, 50.0 };
	budgets.WriteCpu = SBudget { 10.0, -1.0, 9.0, 22.0 };
	budgets.WriteTotal = SBudget { 10.0 };
	budgets.Append = SBudget { 0.1, -1.0, 0.05, 0.1 };
	budgets.Restart = SBudget { -1.0, 1.0, 1.0, 2.0 };
	budgets.Memory = SBudget { 15.0, -1.0, 14.0, 14.0 };
	MeasureSnapshot(Dir, 10000u, 0u, budgets);
}

TEST_F(SnapshotTest, DISABLED_SnapshotAndJournalAt100kAssets)
{
	SSnapshotBudgets budgets;
	budgets.Load = SBudget { 30.0, 200.0, 220.0, 450.0 };
	budgets.WriteCpu = SBudget { 80.0, -1.0, 90.0, 180.0 };
	budgets.WriteTotal = SBudget { 80.0 };
	budgets.Append = SBudget { 0.1, -1.0, 0.05, 0.1 };
	budgets.Restart = SBudget { -1.0, 1.0, 1.0, 2.0 };
	MeasureSnapshot(Dir, 100000u, 0u, budgets);
}

TEST_F(SnapshotTest, DISABLED_SnapshotAndJournalAt100kAssetsAnd500kRows)
{
	SSnapshotBudgets budgets;
	budgets.Load = SBudget { 120.0, 450.0, 520.0, 1000.0 };
	budgets.WriteCpu = SBudget { -1.0, -1.0, 300.0, 600.0 };
	budgets.Append = SBudget { 0.1, -1.0, 0.05, 0.1 };
	budgets.Restart = SBudget { -1.0, 1.0, 1.0, 2.0 };
	budgets.Memory = SBudget { 150.0, -1.0, 155.0, 155.0 };
	MeasureSnapshot(Dir, 100000u, 500000u, budgets);
}
