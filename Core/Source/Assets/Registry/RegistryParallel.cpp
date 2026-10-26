#include "Assets/Registry/RegistryParallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "Jobs/WorkerPool.h"


namespace frt::assets
{
namespace
{
/**
 * What the caller and its helpers share. A helper may start after the call has returned (it was queued behind
 * other work): it then finds no task left and touches only this state, which it co-owns. Tasks is read only by a
 * thread that claimed a task, and the call does not return before every claimed task has finished.
 */
struct SRunState
{
	const TFunctionRef<void ()>* Tasks = nullptr;
	uint32 Count = 0u;
	std::atomic<uint32> Next = 0u;
	std::atomic<uint32> Done = 0u;
	std::mutex Mutex;
	std::condition_variable Finished;

	// Runs tasks until none is left unclaimed.
	void Drain ()
	{
		for (uint32 task = Next.fetch_add(1u, std::memory_order_acq_rel); task < Count; task = Next.fetch_add(1u, std::memory_order_acq_rel))
		{
			Tasks[task]();
			if (Done.fetch_add(1u, std::memory_order_acq_rel) + 1u == Count)
			{
				const std::lock_guard<std::mutex> lock(Mutex);
				Finished.notify_all();
			}
		}
	}
};
}

void RunTasks (jobs::CWorkerPool* Pool, std::span<const TFunctionRef<void ()>> Tasks)
{
	if (Pool == nullptr || Tasks.size() < 2u)
	{
		for (const TFunctionRef<void ()>& task : Tasks)
		{
			task();
		}
		return;
	}
	const std::shared_ptr<SRunState> state = std::make_shared<SRunState>();
	state->Tasks = Tasks.data();
	state->Count = static_cast<uint32>(Tasks.size());
	const uint32 helpers = (std::min)(state->Count - 1u, Pool->GetThreadCount());
	for (uint32 i = 0u; i < helpers; ++i)
	{
		Pool->Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup, [state] { state->Drain(); });
	}
	state->Drain();
	std::unique_lock<std::mutex> lock(state->Mutex);
	state->Finished.wait(lock, [&] { return state->Done.load(std::memory_order_acquire) == state->Count; });
}

std::vector<SChunkRange> SplitIntoChunks (jobs::CWorkerPool* Pool, uint32 Count, uint32 MinChunk, uint32 Align)
{
	std::vector<SChunkRange> chunks;
	if (Count == 0u)
	{
		return chunks;
	}
	const uint32 align = (std::max)(Align, 1u);
	const uint32 units = Count / align + (Count % align != 0u ? 1u : 0u);
	const uint32 threads = Pool != nullptr ? Pool->GetThreadCount() + 1u : 1u;
	const uint32 byWork = (std::max)(1u, Count / (std::max)(MinChunk, 1u));
	const uint32 count = (std::max)(1u, (std::min)({ threads, byWork, units }));
	chunks.reserve(count);
	for (uint32 c = 0u; c < count; ++c)
	{
		const uint32 begin = static_cast<uint32>(static_cast<uint64>(units) * c / count) * align;
		const uint32 end = c + 1u == count ? Count : static_cast<uint32>(static_cast<uint64>(units) * (c + 1u) / count) * align;
		chunks.push_back(SChunkRange { (std::min)(begin, Count), (std::min)(end, Count) });
	}
	return chunks;
}

void RunChunks (jobs::CWorkerPool* Pool, std::span<const SChunkRange> Chunks, TFunctionRef<void (uint32, uint32, uint32)> Body)
{
	struct SChunkTask
	{
		const TFunctionRef<void (uint32, uint32, uint32)>* Body = nullptr;
		uint32 Chunk = 0u;
		SChunkRange Range;
		void operator() () const { (*Body)(Chunk, Range.Begin, Range.End); }
	};
	std::vector<SChunkTask> bodies;
	bodies.reserve(Chunks.size());
	for (uint32 c = 0u; c < Chunks.size(); ++c)
	{
		bodies.push_back(SChunkTask { &Body, c, Chunks[c] });
	}
	std::vector<TFunctionRef<void ()>> tasks;
	tasks.reserve(bodies.size());
	for (const SChunkTask& task : bodies)
	{
		tasks.emplace_back(task);
	}
	RunTasks(Pool, tasks);
}
}
