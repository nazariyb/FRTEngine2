#pragma once

#include <span>
#include <vector>

#include "CoreTypes.h"
#include "Functional/FunctionRef.h"


namespace frt::jobs
{
class CWorkerPool;
}

/**
 * The registry's fork-join for rebuilding a loaded store and decoding a snapshot (design §5.3: indices "rebuilt
 * on load, never stored, in parallel where it pays"). jobs::CWorkerPool is not a job system - its tasks never
 * wait for each other and there is no WaitAll - so a load that wants to split its work does it here: the calling
 * thread (a worker at start-up, or the owner) takes tasks too and runs every task no worker has started, so it
 * never waits for a worker that is busy elsewhere (a pool of one thread, or every worker inside such a call, a
 * task that calls RunTasks again included): it waits only for tasks that are already running. No pool, or one
 * task: every task runs on the caller, in order.
 *
 * Tasks run on any thread at once: each touches only what no other task of the call writes, and std containers,
 * FString and the CRT heap only (the pool's rule for work).
 */
namespace frt::assets
{
// Runs every task once and returns when all have run.
void RunTasks (jobs::CWorkerPool* Pool, std::span<const TFunctionRef<void ()>> Tasks);

struct SChunkRange
{
	uint32 Begin = 0u;
	uint32 End = 0u;
};

// [0, Count) split for RunChunks: at most the pool's threads + 1 chunks (one without a pool), each of at least
// MinChunk items unless there is only one, every bound but Count a multiple of Align (a paged column's page, so
// that two chunks never write one page). Empty for Count 0.
[[nodiscard]] std::vector<SChunkRange> SplitIntoChunks (jobs::CWorkerPool* Pool, uint32 Count, uint32 MinChunk, uint32 Align = 1u);

// Body(Chunk, Begin, End) for every chunk, each a task of RunTasks.
void RunChunks (jobs::CWorkerPool* Pool, std::span<const SChunkRange> Chunks, TFunctionRef<void (uint32, uint32, uint32)> Body);
}
