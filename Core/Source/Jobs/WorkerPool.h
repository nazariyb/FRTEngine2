#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Functional/UniqueFunction.h"
#include "Platform/Thread.h"


/**
 * The engine's worker threads (AssetManager-Design §7.8, §15 I11): decodes, builds, hashing and
 * decompression run here, off the main thread, and hand their results back to it.
 *
 * Engine-owned std::threads, not the Windows thread pool: the same code on every platform, a fixed
 * thread count (deterministic builders partition their work by it, §7.7), threads that profilers
 * and debuggers show by name ("FRT Worker 3", through platform::ConfigureThread), and a drain the
 * engine controls (the module barrier). At most logical cores - 2 threads, so the main thread and
 * the OS keep a core each, and at least one.
 *
 * Not a job system: tasks have no dependencies on each other and no work stealing. A task is a work
 * callable, run on some worker, and an optional completion callable, run afterwards on the owner
 * thread (the thread that constructed the pool, the main thread for the engine's pool) when it pumps
 * completions. A task that needs a follow-up submits it from its completion. An engine job system is
 * a separate project and can replace the pool behind this interface.
 *
 * Priorities. Three FIFO queues; a free worker takes the oldest task of the highest non-empty one.
 *
 * Cancel groups. A task may belong to a 64-bit group (CreateCancelGroup). Cancel(Group) drops the
 * group's tasks that have not started (neither callable runs), lets running ones see IsCancelled()
 * so they can stop early, and discards their completions, as well as completions already waiting
 * for the pump. It reaches the tasks in the pool when it is called: later tasks of the group run.
 *
 * Completions. Finished tasks go into one multi-producer queue tagged with the sequence number
 * Submit gave them, and PumpCompletions runs them on the owner thread in sequence order: what has
 * arrived, sorted (ECompletionOrder::Batch, interactive), or strictly in submission order, a
 * completion waiting for every earlier task to finish or be dropped (ECompletionOrder::Strict:
 * deterministic runs and reproducible tools, §7.8 "Commit order").
 *
 * Epochs. Every task carries the epoch it was submitted in. BumpEpoch starts a new one, and
 * completions of older epochs are discarded when pumped, never run: the result of work built
 * against types that have since been reloaded (§6.7). A client that still wants such a result
 * submits the work again, which it can tell from GetEpoch() having moved past the epoch it
 * submitted in.
 *
 * Waits are pump loops. The owner thread never blocks on a task directly - there is no WaitAll and
 * no future: what it waits for may need a completion to run first (a completion that submits the
 * next step, as a build that discovers an input does), and completions run only on the owner thread.
 * Every wait is
 *
 *     Pool.PumpUntil([&] { return bDone; });
 *
 * which is, spelled out (CStreamableManager::PumpUntil, §8.4, will drive the I/O queue and the build
 * scheduler around the same two calls):
 *
 *     for (;;)
 *     {
 *         Pool.PumpCompletions();
 *         if (bDone) break;
 *         Pool.WaitForCompletions();   // sleeps until a task finishes or a completion arrives
 *     }
 *
 * The module barrier. CancelAllAndDrain cancels everything, waits for the running work and destroys
 * every callable the pool holds, so a module's code can be unloaded once it returns.
 *
 * Threads and memory. Submit, Cancel, CreateCancelGroup, GetEpoch, GetStats and IsCancelled may be
 * called from any thread; PumpCompletions, WaitForCompletions, PumpUntil, BumpEpoch,
 * SetCompletionOrder, CancelAllAndDrain and the destructor only from the owner thread. Work runs on
 * workers and so must never touch TLSF, TArray, TRefShared or the world (§8.8): std containers,
 * FString and the CRT heap. Completions run on the owner thread and may use anything. Neither may
 * block on the other side: work never waits for the owner thread.
 */
namespace frt::jobs
{
enum class EWorkPriority : uint8
{
	High,     // someone is waiting for it (a blocking load, a scripted frame)
	Normal,   // what is visible: a texture of the scene, a visible thumbnail
	Low,      // background: warming the cache, cooking
};

inline constexpr uint32 WorkPriorityCount = 3u;

// The group of a task that belongs to none: Cancel never reaches it, CancelAllAndDrain does.
inline constexpr uint64 NoCancelGroup = 0u;

enum class ECompletionOrder : uint8
{
	Batch,    // each pump runs the completions that have arrived, in sequence order (the default)
	Strict,   // a completion runs only after every task submitted before it has finished or been dropped
};

struct SWorkerPoolDesc
{
	// 0: GetDefaultThreadCount(). Tests ask for an exact count.
	uint32 ThreadCount = 0u;
	// Threads are named "<Name> <index>", index from 0.
	FString Name = "FRT Worker";
	platform::EThreadPriority ThreadPriority = platform::EThreadPriority::Normal;
};

// Counts at the moment of the call. The totals count since construction.
struct SWorkerPoolStats
{
	uint32 Queued = 0u;              // waiting for a worker, every priority
	uint32 Running = 0u;             // on a worker
	uint32 CompletionsWaiting = 0u;  // finished, their completion not yet pumped
	uint64 Submitted = 0u;
	uint64 CompletionsRun = 0u;
	uint64 Dropped = 0u;             // cancelled before they started: neither callable ran
	uint64 Discarded = 0u;           // completions destroyed unrun: cancelled, an older epoch, drained
};

class FRT_CORE_API CWorkerPool
{
public:
	// Logical cores - 2, at least 1.
	[[nodiscard]] static uint32 GetDefaultThreadCount () noexcept;

	// Starts the threads; the calling thread becomes the owner.
	explicit CWorkerPool (const SWorkerPoolDesc& Desc = {});
	// CancelAllAndDrain, then joins the threads. Owner thread.
	~CWorkerPool ();

	CWorkerPool (const CWorkerPool&) = delete;
	CWorkerPool& operator= (const CWorkerPool&) = delete;

	[[nodiscard]] uint32 GetThreadCount () const noexcept { return static_cast<uint32>(Threads.size()); }
	[[nodiscard]] bool IsOwnerThread () const noexcept;

	// ---- tasks ----

	// A fresh group id, never NoCancelGroup and never handed out twice.
	[[nodiscard]] uint64 CreateCancelGroup () noexcept;

	// Queues Work at Priority; Completion, when set, runs on the owner thread after Work has finished,
	// in a later PumpCompletions. Returns the task's sequence number (from 1, in submission order),
	// or 0 when the task was dropped at once because CancelAllAndDrain is running.
	uint64 Submit (EWorkPriority Priority, uint64 CancelGroup, TUniqueFunction<void ()> Work,
		TUniqueFunction<void ()> Completion = nullptr);

	// Cancels the group's tasks now in the pool (see the class comment). Returns how many were
	// dropped before they started. NoCancelGroup reaches nothing.
	uint32 Cancel (uint64 CancelGroup);

	// From inside a work callable: its task has been cancelled (Cancel, CancelAllAndDrain). Long work
	// polls it to stop early; its completion will not run either way. False anywhere else.
	[[nodiscard]] static bool IsCancelled () noexcept;
	// The calling thread is a worker of some pool.
	[[nodiscard]] static bool IsWorkerThread () noexcept;

	// ---- completions (owner thread) ----

	// Runs up to MaxCount completions that are ready, in the order SetCompletionOrder says, and
	// returns how many ran (discarded ones do not count). A completion may submit, cancel, bump the
	// epoch or pump again; a completion it cancels or makes stale is discarded, not run.
	uint32 PumpCompletions (uint32 MaxCount = 0xFFFFFFFFu);
	// Sleeps until something happened since the last PumpCompletions - a task finished or was
	// dropped, a completion arrived - or one is ready already; at most TimeoutSeconds (negative:
	// no limit). False on timeout.
	bool WaitForCompletions (double TimeoutSeconds = -1.0);
	// The wait (see the class comment): pumps until Done() holds and returns true. False when
	// TimeoutSeconds (negative: no limit) runs out, or at once when Done() is false and the pool
	// holds nothing that could still change that - no task queued, running or waiting for the pump:
	// such a wait could never end.
	bool PumpUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds = -1.0);

	void SetCompletionOrder (ECompletionOrder Order);
	[[nodiscard]] ECompletionOrder GetCompletionOrder () const noexcept { return Order; }

	// Starts a new epoch and returns it; completions of earlier epochs are discarded when pumped.
	uint64 BumpEpoch () noexcept;
	[[nodiscard]] uint64 GetEpoch () const noexcept { return Epoch.load(std::memory_order_acquire); }

	// The module barrier: drops every queued task, cancels the running ones, waits until they have
	// finished, discards every completion and starts a new epoch. Tasks submitted meanwhile, from
	// other threads or by the running work, are dropped too. The pool takes work again afterwards.
	void CancelAllAndDrain ();

	// ---- state ----
	[[nodiscard]] SWorkerPoolStats GetStats () const;
	// Nothing queued, running or waiting for the pump.
	[[nodiscard]] bool IsIdle () const;

private:
	struct STask
	{
		uint64 Sequence = 0u;
		uint64 CancelGroup = NoCancelGroup;
		uint64 Epoch = 0u;
		EWorkPriority Priority = EWorkPriority::Normal;
		TUniqueFunction<void ()> Work;
		TUniqueFunction<void ()> Completion;
		// Set under QueueMutex (and read by the finishing worker under both mutexes), polled
		// without a lock by the work through IsCancelled.
		std::atomic<bool> bCancelled = false;
	};
	using TaskPtr = std::unique_ptr<STask>;

	void WorkerMain (uint32 Index);
	void RunTask (uint32 Index, TaskPtr Task);
	[[nodiscard]] bool HasQueuedLocked () const noexcept;
	[[nodiscard]] TaskPtr PopQueuedLocked ();

	// Retirement: a task retires when its completion is taken by a pump or discarded, or, without
	// one, when its work finished or it was dropped. Strict order runs a completion only when its
	// sequence is the oldest one that has not retired. CompletionMutex held.
	void RetireLocked (uint64 Sequence);
	void PushReadyLocked (TaskPtr Task);
	[[nodiscard]] bool HasPumpableLocked () const noexcept;
	[[nodiscard]] TaskPtr TakeNextCompletionLocked ();
	// Moves the group's waiting completions (every one for NoCancelGroup with bAll) into Out.
	void ExtractCompletionsLocked (uint64 CancelGroup, bool bAll, std::vector<TaskPtr>& Out);

	const uint64 OwnerThreadId;
	FString Name;
	platform::EThreadPriority ThreadPriority = platform::EThreadPriority::Normal;
	std::vector<std::thread> Threads;
	std::atomic<uint64> NextCancelGroup = 1u;
	std::atomic<uint64> Epoch = 1u;
	ECompletionOrder Order = ECompletionOrder::Batch;   // owner thread, changed under CompletionMutex

	// Lock order: QueueMutex before CompletionMutex, wherever both are held.
	mutable std::mutex QueueMutex;
	std::condition_variable WorkAvailable;      // workers: a task was queued, or the pool stops
	std::condition_variable WorkersIdle;        // CancelAllAndDrain: RunningCount reached 0
	std::deque<TaskPtr> Queues[WorkPriorityCount];
	std::vector<STask*> RunningTasks;           // per worker: its task until it finishes, for Cancel
	uint32 RunningCount = 0u;                   // workers busy with a task, its clean-up included
	uint64 NextSequence = 1u;
	bool bDraining = false;
	bool bStopping = false;

	mutable std::mutex CompletionMutex;
	std::condition_variable CompletionEvent;    // the owner: WakeCount changed
	std::vector<TaskPtr> Ready;                 // finished tasks with a completion: a heap, lowest sequence on top
	std::deque<TaskPtr> Batch;                  // Batch order: the arrivals the current pump runs, by sequence
	std::vector<uint64> RetiredAhead;           // retired sequences above NextToRetire: a heap, lowest on top
	uint64 NextToRetire = 1u;                   // every sequence below it has retired
	uint64 WakeCount = 0u;                      // bumped for every finish, drop and arrival
	uint64 WakeCountAtPump = 0u;                // WakeCount when PumpCompletions last looked

	// The totals of SWorkerPoolStats.
	std::atomic<uint64> SubmittedCount = 0u;
	std::atomic<uint64> CompletionsRunCount = 0u;
	std::atomic<uint64> DroppedCount = 0u;
	std::atomic<uint64> DiscardedCount = 0u;
};
}
