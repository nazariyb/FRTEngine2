#include "Jobs/WorkerPool.h"

#include <algorithm>
#include <chrono>
#include <functional>

#include "Asserts.h"
#include "Profiler/CpuProfiler.h"


namespace frt::jobs
{
namespace
{
// The cancel flag of the task the calling worker runs; null outside a work callable.
thread_local const std::atomic<bool>* tCancelFlag = nullptr;
thread_local bool tIsWorker = false;

// Heap order for std::push_heap / pop_heap: the lowest sequence on top.
template <typename TPtr>
bool SequenceGreater (const TPtr& A, const TPtr& B) noexcept
{
	return A->Sequence > B->Sequence;
}
}


uint32 CWorkerPool::GetDefaultThreadCount () noexcept
{
	// hardware_concurrency may be 0 (unknown): one worker then, as on a machine of one or two cores.
	const uint32 cores = std::thread::hardware_concurrency();
	return cores > 2u ? cores - 2u : 1u;
}

CWorkerPool::CWorkerPool (const SWorkerPoolDesc& Desc)
	: OwnerThreadId(platform::GetCurrentThreadId())
	, Name(Desc.Name)
	, ThreadPriority(Desc.ThreadPriority)
{
	const uint32 count = Desc.ThreadCount != 0u ? Desc.ThreadCount : GetDefaultThreadCount();
	// Sized before the first thread starts: each worker owns its slot from then on.
	RunningTasks.assign(count, nullptr);
	Threads.reserve(count);
	for (uint32 index = 0u; index < count; ++index)
	{
		Threads.emplace_back(&CWorkerPool::WorkerMain, this, index);
	}
}

CWorkerPool::~CWorkerPool ()
{
	CancelAllAndDrain();
	{
		std::lock_guard lock(QueueMutex);
		bStopping = true;
	}
	WorkAvailable.notify_all();
	for (std::thread& thread : Threads)
	{
		thread.join();
	}
}

bool CWorkerPool::IsOwnerThread () const noexcept
{
	return platform::GetCurrentThreadId() == OwnerThreadId;
}

uint64 CWorkerPool::CreateCancelGroup () noexcept
{
	return NextCancelGroup.fetch_add(1u, std::memory_order_relaxed);
}

bool CWorkerPool::IsCancelled () noexcept
{
	return tCancelFlag != nullptr && tCancelFlag->load(std::memory_order_relaxed);
}

bool CWorkerPool::IsWorkerThread () noexcept
{
	return tIsWorker;
}


// ---- Workers ----

void CWorkerPool::WorkerMain (uint32 Index)
{
	platform::ConfigureThread(FString::Format("{} {}", Name, Index), ThreadPriority);
	tIsWorker = true;
	for (;;)
	{
		TaskPtr task;
		{
			std::unique_lock lock(QueueMutex);
			WorkAvailable.wait(lock, [this] () { return bStopping || (!bDraining && HasQueuedLocked()); });
			if (bStopping)
			{
				// The destructor drained first: nothing is queued.
				return;
			}
			task = PopQueuedLocked();
			RunningTasks[Index] = task.get();
			++RunningCount;
		}
		RunTask(Index, std::move(task));
	}
}

void CWorkerPool::RunTask (uint32 Index, TaskPtr Task)
{
	tCancelFlag = &Task->bCancelled;
	{
		FRT_CPU_SCOPE("Jobs.Work");
		Task->Work();
	}
	tCancelFlag = nullptr;
	// The work callable dies here, on the worker, before the task counts as finished: once a drain
	// returns, no callable of a module about to be unloaded is left anywhere.
	Task->Work = nullptr;

	// A cancelled task's completion: destroyed out of the locks, but before the worker counts as idle,
	// since a drain waits for that too.
	TaskPtr discarded;
	bool bAllIdle = false;
	{
		// Both locks for the hand-over, so Cancel sees the task either running (and flags it) or with
		// its completion waiting (and extracts it), never between the two.
		std::lock_guard queueLock(QueueMutex);
		std::lock_guard completionLock(CompletionMutex);
		RunningTasks[Index] = nullptr;
		if (Task->Completion && !Task->bCancelled.load(std::memory_order_relaxed))
		{
			PushReadyLocked(std::move(Task));
		}
		else
		{
			RetireLocked(Task->Sequence);
			if (Task->Completion)
			{
				DiscardedCount.fetch_add(1u, std::memory_order_relaxed);
				discarded = std::move(Task);
			}
		}
		if (!discarded)
		{
			--RunningCount;
			bAllIdle = RunningCount == 0u;
		}
		++WakeCount;
	}
	if (discarded)
	{
		discarded.reset();
		std::lock_guard queueLock(QueueMutex);
		std::lock_guard completionLock(CompletionMutex);
		--RunningCount;
		bAllIdle = RunningCount == 0u;
		// The owner may be waiting for the pool to go idle (PumpUntil): this is an event as well.
		++WakeCount;
	}
	CompletionEvent.notify_all();
	if (bAllIdle)
	{
		WorkersIdle.notify_all();
	}
}

bool CWorkerPool::HasQueuedLocked () const noexcept
{
	for (const std::deque<TaskPtr>& queue : Queues)
	{
		if (!queue.empty())
		{
			return true;
		}
	}
	return false;
}

CWorkerPool::TaskPtr CWorkerPool::PopQueuedLocked ()
{
	for (std::deque<TaskPtr>& queue : Queues)
	{
		if (!queue.empty())
		{
			TaskPtr task = std::move(queue.front());
			queue.pop_front();
			return task;
		}
	}
	return nullptr;
}


// ---- Tasks ----

uint64 CWorkerPool::Submit (EWorkPriority Priority, uint64 CancelGroup, TUniqueFunction<void ()> Work,
	TUniqueFunction<void ()> Completion)
{
	frt_assert(Work.IsSet());
	frt_assert(static_cast<uint32>(Priority) < WorkPriorityCount);

	auto task = std::make_unique<STask>();
	task->CancelGroup = CancelGroup;
	task->Priority = Priority;
	task->Work = std::move(Work);
	task->Completion = std::move(Completion);

	uint64 sequence = 0u;
	{
		std::lock_guard lock(QueueMutex);
		if (!bDraining && !bStopping)
		{
			sequence = NextSequence++;
			task->Sequence = sequence;
			task->Epoch = Epoch.load(std::memory_order_acquire);
			Queues[static_cast<uint32>(Priority)].push_back(std::move(task));
		}
	}
	SubmittedCount.fetch_add(1u, std::memory_order_relaxed);
	if (sequence == 0u)
	{
		// Dropped: the callables die here, out of the lock.
		DroppedCount.fetch_add(1u, std::memory_order_relaxed);
		return 0u;
	}
	WorkAvailable.notify_one();
	return sequence;
}

uint32 CWorkerPool::Cancel (uint64 CancelGroup)
{
	if (CancelGroup == NoCancelGroup)
	{
		return 0u;
	}

	std::vector<TaskPtr> removed;
	uint32 dropped = 0u;
	{
		std::lock_guard queueLock(QueueMutex);
		for (std::deque<TaskPtr>& queue : Queues)
		{
			std::deque<TaskPtr> kept;
			for (TaskPtr& task : queue)
			{
				if (task->CancelGroup == CancelGroup)
				{
					removed.push_back(std::move(task));
				}
				else
				{
					kept.push_back(std::move(task));
				}
			}
			queue.swap(kept);
		}
		dropped = static_cast<uint32>(removed.size());
		for (STask* running : RunningTasks)
		{
			if (running != nullptr && running->CancelGroup == CancelGroup)
			{
				running->bCancelled.store(true, std::memory_order_relaxed);
			}
		}

		std::lock_guard completionLock(CompletionMutex);
		for (const TaskPtr& task : removed)
		{
			RetireLocked(task->Sequence);
		}
		ExtractCompletionsLocked(CancelGroup, false, removed);
		++WakeCount;
	}
	DroppedCount.fetch_add(dropped, std::memory_order_relaxed);
	CompletionEvent.notify_all();
	// Destroyed here, out of the locks: dropped tasks with both callables, waiting completions.
	removed.clear();
	return dropped;
}


// ---- Completions ----

void CWorkerPool::RetireLocked (uint64 Sequence)
{
	if (Sequence != NextToRetire)
	{
		RetiredAhead.push_back(Sequence);
		std::push_heap(RetiredAhead.begin(), RetiredAhead.end(), std::greater<uint64>());
		return;
	}
	++NextToRetire;
	while (!RetiredAhead.empty() && RetiredAhead.front() == NextToRetire)
	{
		std::pop_heap(RetiredAhead.begin(), RetiredAhead.end(), std::greater<uint64>());
		RetiredAhead.pop_back();
		++NextToRetire;
	}
}

void CWorkerPool::PushReadyLocked (TaskPtr Task)
{
	Ready.push_back(std::move(Task));
	std::push_heap(Ready.begin(), Ready.end(), &SequenceGreater<TaskPtr>);
}

bool CWorkerPool::HasPumpableLocked () const noexcept
{
	if (Order == ECompletionOrder::Strict)
	{
		return !Ready.empty() && Ready.front()->Sequence == NextToRetire;
	}
	return !Batch.empty() || !Ready.empty();
}

CWorkerPool::TaskPtr CWorkerPool::TakeNextCompletionLocked ()
{
	TaskPtr task;
	if (Order == ECompletionOrder::Strict)
	{
		if (Ready.empty() || Ready.front()->Sequence != NextToRetire)
		{
			return nullptr;
		}
		std::pop_heap(Ready.begin(), Ready.end(), &SequenceGreater<TaskPtr>);
		task = std::move(Ready.back());
		Ready.pop_back();
	}
	else
	{
		if (Batch.empty())
		{
			return nullptr;
		}
		task = std::move(Batch.front());
		Batch.pop_front();
	}
	RetireLocked(task->Sequence);
	return task;
}

void CWorkerPool::ExtractCompletionsLocked (uint64 CancelGroup, bool bAll, std::vector<TaskPtr>& Out)
{
	const auto matches = [CancelGroup, bAll] (const TaskPtr& Task) { return bAll || Task->CancelGroup == CancelGroup; };
	const size_t before = Out.size();

	std::vector<TaskPtr> keptReady;
	for (TaskPtr& task : Ready)
	{
		(matches(task) ? Out : keptReady).push_back(std::move(task));
	}
	Ready.swap(keptReady);
	std::make_heap(Ready.begin(), Ready.end(), &SequenceGreater<TaskPtr>);

	std::deque<TaskPtr> keptBatch;
	for (TaskPtr& task : Batch)
	{
		if (matches(task))
		{
			Out.push_back(std::move(task));
		}
		else
		{
			keptBatch.push_back(std::move(task));
		}
	}
	Batch.swap(keptBatch);

	for (size_t i = before; i < Out.size(); ++i)
	{
		RetireLocked(Out[i]->Sequence);
	}
	DiscardedCount.fetch_add(Out.size() - before, std::memory_order_relaxed);
}

uint32 CWorkerPool::PumpCompletions (uint32 MaxCount)
{
	frt_assert(IsOwnerThread());
	{
		std::lock_guard lock(CompletionMutex);
		WakeCountAtPump = WakeCount;
		if (Order == ECompletionOrder::Batch && Batch.empty())
		{
			// This pump's batch: everything that has arrived, lowest sequence first. A pump nested in
			// a completion continues the same batch.
			while (!Ready.empty())
			{
				std::pop_heap(Ready.begin(), Ready.end(), &SequenceGreater<TaskPtr>);
				Batch.push_back(std::move(Ready.back()));
				Ready.pop_back();
			}
		}
	}

	uint32 ran = 0u;
	while (ran < MaxCount)
	{
		TaskPtr task;
		{
			std::lock_guard lock(CompletionMutex);
			task = TakeNextCompletionLocked();
		}
		if (!task)
		{
			break;
		}
		// Checked one completion at a time: an earlier completion of this pump may have bumped the
		// epoch or cancelled this one's group.
		if (task->Epoch != Epoch.load(std::memory_order_acquire) || task->bCancelled.load(std::memory_order_relaxed))
		{
			DiscardedCount.fetch_add(1u, std::memory_order_relaxed);
		}
		else
		{
			task->Completion();
			CompletionsRunCount.fetch_add(1u, std::memory_order_relaxed);
			++ran;
		}
		// The completion dies on the owner thread, where it ran.
		task.reset();
	}
	return ran;
}

bool CWorkerPool::WaitForCompletions (double TimeoutSeconds)
{
	frt_assert(IsOwnerThread());
	std::unique_lock lock(CompletionMutex);
	const auto woken = [this] () { return WakeCount != WakeCountAtPump || HasPumpableLocked(); };
	if (TimeoutSeconds < 0.0)
	{
		CompletionEvent.wait(lock, woken);
		return true;
	}
	return CompletionEvent.wait_for(lock, std::chrono::duration<double>(TimeoutSeconds), woken);
}

bool CWorkerPool::PumpUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds)
{
	frt_assert(IsOwnerThread());
	const auto start = std::chrono::steady_clock::now();
	for (;;)
	{
		PumpCompletions();
		// Idle is read before Done: whatever the last task did happened before this read, so a Done
		// that is still false afterwards can never become true through the pool.
		const bool bIdle = IsIdle();
		if (Done())
		{
			return true;
		}
		if (bIdle)
		{
			return false;
		}
		if (TimeoutSeconds < 0.0)
		{
			WaitForCompletions();
			continue;
		}
		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (elapsed >= TimeoutSeconds)
		{
			return false;
		}
		WaitForCompletions(TimeoutSeconds - elapsed);
	}
}

void CWorkerPool::SetCompletionOrder (ECompletionOrder InOrder)
{
	frt_assert(IsOwnerThread());
	std::lock_guard lock(CompletionMutex);
	if (InOrder == ECompletionOrder::Strict)
	{
		// What a batch still holds goes back to wait its turn.
		for (TaskPtr& task : Batch)
		{
			PushReadyLocked(std::move(task));
		}
		Batch.clear();
	}
	Order = InOrder;
}

uint64 CWorkerPool::BumpEpoch () noexcept
{
	frt_assert(IsOwnerThread());
	return Epoch.fetch_add(1u, std::memory_order_acq_rel) + 1u;
}

void CWorkerPool::CancelAllAndDrain ()
{
	frt_assert(IsOwnerThread());

	std::vector<TaskPtr> removed;
	{
		std::lock_guard queueLock(QueueMutex);
		// Until the end: Submit drops, workers take nothing.
		bDraining = true;
		for (std::deque<TaskPtr>& queue : Queues)
		{
			for (TaskPtr& task : queue)
			{
				removed.push_back(std::move(task));
			}
			queue.clear();
		}
		DroppedCount.fetch_add(removed.size(), std::memory_order_relaxed);
		for (STask* running : RunningTasks)
		{
			if (running != nullptr)
			{
				running->bCancelled.store(true, std::memory_order_relaxed);
			}
		}
		Epoch.fetch_add(1u, std::memory_order_acq_rel);

		std::lock_guard completionLock(CompletionMutex);
		for (const TaskPtr& task : removed)
		{
			RetireLocked(task->Sequence);
		}
		ExtractCompletionsLocked(NoCancelGroup, true, removed);
		++WakeCount;
	}
	removed.clear();

	{
		std::unique_lock queueLock(QueueMutex);
		WorkersIdle.wait(queueLock, [this] () { return RunningCount == 0u; });

		// The work that was running was cancelled, so its workers destroyed its completions; this
		// catches nothing unless a task finished between the two steps above.
		std::lock_guard completionLock(CompletionMutex);
		ExtractCompletionsLocked(NoCancelGroup, true, removed);
		bDraining = false;
	}
	removed.clear();
}


// ---- State ----

SWorkerPoolStats CWorkerPool::GetStats () const
{
	SWorkerPoolStats stats;
	{
		std::lock_guard queueLock(QueueMutex);
		std::lock_guard completionLock(CompletionMutex);
		for (const std::deque<TaskPtr>& queue : Queues)
		{
			stats.Queued += static_cast<uint32>(queue.size());
		}
		stats.Running = RunningCount;
		stats.CompletionsWaiting = static_cast<uint32>(Ready.size() + Batch.size());
	}
	stats.Submitted = SubmittedCount.load(std::memory_order_relaxed);
	stats.CompletionsRun = CompletionsRunCount.load(std::memory_order_relaxed);
	stats.Dropped = DroppedCount.load(std::memory_order_relaxed);
	stats.Discarded = DiscardedCount.load(std::memory_order_relaxed);
	return stats;
}

bool CWorkerPool::IsIdle () const
{
	std::lock_guard queueLock(QueueMutex);
	std::lock_guard completionLock(CompletionMutex);
	return !HasQueuedLocked() && RunningCount == 0u && Ready.empty() && Batch.empty();
}
}
