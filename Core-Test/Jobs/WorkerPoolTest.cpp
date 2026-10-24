// jobs::CWorkerPool (AssetManager-Design §7.8, I11): tasks start by priority and, within one, in
// submission order; completions run on the owner thread in sequence order (sorted per pump, or
// strictly); cancel groups drop what has not started and discard what has; completions of an older
// epoch never run; CancelAllAndDrain leaves no task, running work or completion behind, however much
// is in flight; and an owner thread that waits by pumping never deadlocks, even when completions
// submit the next step of what it waits for.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "Jobs/WorkerPool.h"
#include "Platform/Thread.h"

using namespace frt;
using namespace frt::jobs;


namespace
{
constexpr double TestTimeoutSeconds = 60.0;

// Opened once by the test; work waits on it. Declared before the pool, so it outlives the work:
// locals die in reverse order, and the pool's destructor drains the work before the gate goes.
struct SGate
{
	std::atomic<bool> bOpen = false;
	std::atomic<bool> bReached = false;

	void Open ()
	{
		bOpen.store(true);
	}

	// Returns once the gate opens or the task is cancelled: a test that fails before it opens the
	// gate then ends in the pool's drain instead of hanging there.
	void Wait ()
	{
		bReached.store(true);
		bReached.notify_all();
		while (!bOpen.load() && !CWorkerPool::IsCancelled())
		{
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	}
};

// Holds the only worker of a one-thread pool until the gate opens, so the tasks submitted meanwhile
// queue up behind it. Returns once it runs.
void BlockTheWorker (CWorkerPool& Pool, SGate& Gate)
{
	Pool.Submit(EWorkPriority::High, NoCancelGroup, [&Gate] () { Gate.Wait(); });
	Gate.bReached.wait(false);
}

// Work that runs until its task is cancelled (or a minute passes, so a broken test cannot hang).
void RunUntilCancelled ()
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!CWorkerPool::IsCancelled() && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
}

// Waits, without pumping, until no task is queued or running: their completions are left waiting.
bool WaitForWorkToFinish (const CWorkerPool& Pool)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(TestTimeoutSeconds);
	for (;;)
	{
		const SWorkerPoolStats stats = Pool.GetStats();
		if (stats.Queued == 0u && stats.Running == 0u)
		{
			return true;
		}
		if (std::chrono::steady_clock::now() > deadline)
		{
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// A few microseconds of CPU, more or less at random, and now and then a millisecond's sleep: work
// that finishes out of order.
void Busy (uint32 Seed)
{
	std::minstd_rand random(Seed);
	if (random() % 64u == 0u)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return;
	}
	volatile uint32 sink = 0u;
	const uint32 spins = random() % 4000u;
	for (uint32 i = 0u; i < spins; ++i)
	{
		sink = sink + i;
	}
}

// Where the completions built by WitnessedCompletion died: on the owner thread, run or not.
struct SDeathSites
{
	const uint64 Owner = platform::GetCurrentThreadId();
	std::atomic<uint32> OnOwner = 0u;
	std::atomic<uint32> Elsewhere = 0u;
};

// A capture that tells its SDeathSites which thread destroyed it. A move disarms the source, so only
// the copy the pool ends up holding reports.
struct SDeathWitness
{
	SDeathSites* Sites = nullptr;

	explicit SDeathWitness (SDeathSites& InSites) : Sites(&InSites) {}
	SDeathWitness (SDeathWitness&& Other) noexcept : Sites(std::exchange(Other.Sites, nullptr)) {}
	SDeathWitness& operator= (SDeathWitness&&) = delete;
	~SDeathWitness ()
	{
		if (Sites != nullptr)
		{
			(platform::GetCurrentThreadId() == Sites->Owner ? Sites->OnOwner : Sites->Elsewhere).fetch_add(1u);
		}
	}
};

// A completion that sets *bRan when it runs and reports where it dies to Sites.
TUniqueFunction<void ()> WitnessedCompletion (SDeathSites& Sites, bool* bRan = nullptr)
{
	return [witness = SDeathWitness(Sites), bRan] ()
	{
		(void)witness;
		if (bRan != nullptr)
		{
			*bRan = true;
		}
	};
}
}


// ---------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------

TEST(WorkerPool, TheDefaultThreadCountLeavesTwoCoresAndIsAtLeastOne)
{
	const uint32 cores = std::thread::hardware_concurrency();
	const uint32 expected = cores > 2u ? cores - 2u : 1u;
	EXPECT_EQ(CWorkerPool::GetDefaultThreadCount(), expected);
	EXPECT_GE(CWorkerPool::GetDefaultThreadCount(), 1u);

	CWorkerPool pool;
	EXPECT_EQ(pool.GetThreadCount(), expected);
}

TEST(WorkerPool, AnExplicitThreadCountBoundsHowManyTasksRunAtOnce)
{
	constexpr uint32 ThreadCount = 3u;
	CWorkerPool pool({ .ThreadCount = ThreadCount });
	ASSERT_EQ(pool.GetThreadCount(), ThreadCount);

	// All three threads are in use at once: three tasks that each wait for the other two.
	std::latch allRunning(ThreadCount);
	std::atomic<uint32> met = 0u;
	for (uint32 i = 0u; i < ThreadCount; ++i)
	{
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] ()
		{
			allRunning.arrive_and_wait();
			met.fetch_add(1u);
		});
	}
	ASSERT_TRUE(pool.PumpUntil([&] () { return met.load() == ThreadCount; }, TestTimeoutSeconds));

	// And never a fourth.
	std::atomic<uint32> running = 0u;
	std::atomic<uint32> maxRunning = 0u;
	std::atomic<uint32> finished = 0u;
	for (uint32 i = 0u; i < 4u * ThreadCount; ++i)
	{
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] ()
		{
			const uint32 now = running.fetch_add(1u) + 1u;
			uint32 seen = maxRunning.load();
			while (now > seen && !maxRunning.compare_exchange_weak(seen, now))
			{
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			running.fetch_sub(1u);
			finished.fetch_add(1u);
		});
	}
	ASSERT_TRUE(pool.PumpUntil([&] () { return finished.load() == 4u * ThreadCount; }, TestTimeoutSeconds));
	EXPECT_LE(maxRunning.load(), ThreadCount);
	EXPECT_GE(maxRunning.load(), 1u);
}

TEST(WorkerPool, WorkersAreNamedFrtWorkerAndTheirIndex)
{
	constexpr uint32 ThreadCount = 3u;
	CWorkerPool pool({ .ThreadCount = ThreadCount });

	// One task per thread for sure: each waits until all three run.
	std::latch allRunning(ThreadCount);
	std::mutex mutex;
	std::vector<FString> names;
	std::vector<bool> bOnWorker;
	for (uint32 i = 0u; i < ThreadCount; ++i)
	{
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] ()
		{
			allRunning.arrive_and_wait();
			std::lock_guard lock(mutex);
			names.push_back(platform::GetCurrentThreadName());
			bOnWorker.push_back(CWorkerPool::IsWorkerThread());
		});
	}
	const auto allNamed = [&] ()
	{
		std::lock_guard lock(mutex);
		return names.size() == ThreadCount;
	};
	ASSERT_TRUE(pool.PumpUntil(allNamed, TestTimeoutSeconds));

	std::sort(names.begin(), names.end());
	EXPECT_EQ(names[0], "FRT Worker 0");
	EXPECT_EQ(names[1], "FRT Worker 1");
	EXPECT_EQ(names[2], "FRT Worker 2");
	EXPECT_EQ(std::count(bOnWorker.begin(), bOnWorker.end(), true), static_cast<std::ptrdiff_t>(ThreadCount));
	EXPECT_FALSE(CWorkerPool::IsWorkerThread());
}


// ---------------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------------

TEST(WorkerPool, TasksOfOnePriorityStartInSubmissionOrder)
{
	SGate gate;
	CWorkerPool pool({ .ThreadCount = 1u });
	BlockTheWorker(pool, gate);

	// Only the one worker touches it until the pool is idle again.
	std::vector<uint32> started;
	for (uint32 i = 0u; i < 100u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&started, i] () { started.push_back(i); });
	}
	gate.Open();
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));

	ASSERT_EQ(started.size(), 100u);
	for (uint32 i = 0u; i < 100u; ++i)
	{
		EXPECT_EQ(started[i], i);
	}
}

TEST(WorkerPool, HigherPriorityTasksStartFirst)
{
	SGate gate;
	CWorkerPool pool({ .ThreadCount = 1u });
	BlockTheWorker(pool, gate);

	std::vector<FString> started;
	const auto submit = [&] (EWorkPriority Priority, FStringLiteral Name)
	{
		pool.Submit(Priority, NoCancelGroup, [&started, Name] () { started.emplace_back(Name); });
	};
	submit(EWorkPriority::Low, "L0");
	submit(EWorkPriority::Normal, "N0");
	submit(EWorkPriority::High, "H0");
	submit(EWorkPriority::Low, "L1");
	submit(EWorkPriority::Normal, "N1");
	submit(EWorkPriority::High, "H1");
	submit(EWorkPriority::Normal, "N2");
	gate.Open();
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));

	const std::vector<FString> expected = { "H0", "H1", "N0", "N1", "N2", "L0", "L1" };
	EXPECT_EQ(started, expected);
}

TEST(WorkerPool, CompletionsRunOnTheOwnerThreadAfterTheirWork)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	EXPECT_TRUE(pool.IsOwnerThread());
	const uint64 owner = platform::GetCurrentThreadId();

	std::atomic<uint64> workThread = 0u;
	uint64 completionThread = 0u;
	bool bWorkWasDone = false;
	const uint64 sequence = pool.Submit(EWorkPriority::Normal, NoCancelGroup,
		[&] () { workThread.store(platform::GetCurrentThreadId()); },
		[&] ()
		{
			completionThread = platform::GetCurrentThreadId();
			bWorkWasDone = workThread.load() != 0u;
		});
	EXPECT_EQ(sequence, 1u);
	ASSERT_TRUE(pool.PumpUntil([&] () { return completionThread != 0u; }, TestTimeoutSeconds));

	EXPECT_EQ(completionThread, owner);
	EXPECT_NE(workThread.load(), owner);
	EXPECT_TRUE(bWorkWasDone);
	const SWorkerPoolStats stats = pool.GetStats();
	EXPECT_EQ(stats.Submitted, 1u);
	EXPECT_EQ(stats.CompletionsRun, 1u);
	EXPECT_EQ(stats.Dropped, 0u);
	EXPECT_EQ(stats.Discarded, 0u);
}

TEST(WorkerPool, EachPumpRunsTheArrivedCompletionsInSequenceOrder)
{
	CWorkerPool pool({ .ThreadCount = 4u });
	ASSERT_EQ(pool.GetCompletionOrder(), ECompletionOrder::Batch);

	constexpr uint32 TaskCount = 400u;
	struct SRun
	{
		uint32 Pump = 0u;
		uint64 Sequence = 0u;
	};
	std::vector<SRun> runs;
	uint32 pump = 0u;
	for (uint32 i = 0u; i < TaskCount; ++i)
	{
		const auto priority = static_cast<EWorkPriority>(i % WorkPriorityCount);
		// A fresh pool numbers its tasks from 1, in submission order.
		const uint64 sequence = i + 1u;
		EXPECT_EQ(pool.Submit(priority, NoCancelGroup, [i] () { Busy(i); },
			[&runs, &pump, sequence] () { runs.push_back({ pump, sequence }); }), sequence);
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(TestTimeoutSeconds);
	for (;;)
	{
		pool.PumpCompletions();
		++pump;
		if (runs.size() == TaskCount || std::chrono::steady_clock::now() > deadline)
		{
			break;
		}
		pool.WaitForCompletions(1.0);
	}
	ASSERT_EQ(runs.size(), TaskCount);

	std::vector<bool> seen(TaskCount + 1u, false);
	for (size_t i = 0u; i < runs.size(); ++i)
	{
		ASSERT_GE(runs[i].Sequence, 1u);
		ASSERT_LE(runs[i].Sequence, TaskCount);
		EXPECT_FALSE(seen[runs[i].Sequence]) << "sequence " << runs[i].Sequence << " ran twice";
		seen[runs[i].Sequence] = true;
		if (i > 0u && runs[i].Pump == runs[i - 1u].Pump)
		{
			EXPECT_GT(runs[i].Sequence, runs[i - 1u].Sequence) << "within pump " << runs[i].Pump;
		}
	}
}

TEST(WorkerPool, StrictOrderRunsCompletionsInSubmissionOrder)
{
	CWorkerPool pool({ .ThreadCount = 4u });
	pool.SetCompletionOrder(ECompletionOrder::Strict);

	constexpr uint32 TaskCount = 300u;
	std::vector<uint32> ran;
	for (uint32 i = 0u; i < TaskCount; ++i)
	{
		const auto priority = static_cast<EWorkPriority>((i * 7u) % WorkPriorityCount);
		pool.Submit(priority, NoCancelGroup, [i] () { Busy(i * 31u); }, [&ran, i] () { ran.push_back(i); });
	}
	ASSERT_TRUE(pool.PumpUntil([&] () { return ran.size() == TaskCount; }, TestTimeoutSeconds));
	for (uint32 i = 0u; i < TaskCount; ++i)
	{
		ASSERT_EQ(ran[i], i);
	}
}

TEST(WorkerPool, StrictOrderHoldsLaterCompletionsBackUntilTheEarlierTaskFinishes)
{
	// Both workers held: the first task's (which has a completion) until the end, the second's (which
	// has none) until the queue below is in place.
	SGate first;
	SGate second;
	std::atomic<uint32> holding = 0u;
	CWorkerPool pool({ .ThreadCount = 2u });
	pool.SetCompletionOrder(ECompletionOrder::Strict);

	std::vector<uint32> ran;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] () { holding.fetch_add(1u); first.Wait(); },
		[&ran] () { ran.push_back(0u); });
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] () { holding.fetch_add(1u); second.Wait(); });
	while (holding.load() != 2u)
	{
		std::this_thread::yield();
	}
	// A task without a completion, and one dropped by a cancel, retire their sequences all the same.
	const uint64 group = pool.CreateCancelGroup();
	pool.Submit(EWorkPriority::Normal, group, [] () {}, [&ran] () { ran.push_back(99u); });
	for (uint32 i = 1u; i <= 5u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&ran, i] () { ran.push_back(i); });
	}
	EXPECT_EQ(pool.Cancel(group), 1u);
	second.Open();
	const auto finished = [&pool] ()
	{
		const SWorkerPoolStats stats = pool.GetStats();
		return stats.Queued == 0u && stats.CompletionsWaiting == 5u;
	};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(TestTimeoutSeconds);
	while (!finished() && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(finished());

	EXPECT_EQ(pool.PumpCompletions(), 0u);
	EXPECT_FALSE(pool.WaitForCompletions(0.0));
	first.Open();
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));
	const std::vector<uint32> expected = { 0u, 1u, 2u, 3u, 4u, 5u };
	EXPECT_EQ(ran, expected);
}

TEST(WorkerPool, PumpCompletionsStopsAtMaxCountAndTheNextPumpGoesOn)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	std::vector<uint32> ran;
	for (uint32 i = 0u; i < 5u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&ran, i] () { ran.push_back(i); });
	}
	ASSERT_TRUE(WaitForWorkToFinish(pool));

	EXPECT_EQ(pool.PumpCompletions(2u), 2u);
	EXPECT_EQ(pool.PumpCompletions(2u), 2u);
	EXPECT_EQ(pool.PumpCompletions(2u), 1u);
	EXPECT_EQ(pool.PumpCompletions(), 0u);
	const std::vector<uint32> expected = { 0u, 1u, 2u, 3u, 4u };
	EXPECT_EQ(ran, expected);
	EXPECT_TRUE(pool.IsIdle());
}

TEST(WorkerPool, APumpRunsOnlyTheCompletionsOfTasksSubmittedBeforeItBegan)
{
	// A chain: each completion submits the next step, whose work is done, and its completion waiting,
	// before the completion returns. A pump that took whatever had arrived would run the whole chain
	// in one call - one frame's pump turned into a loop for as long as the chain goes on.
	for (const ECompletionOrder order : { ECompletionOrder::Batch, ECompletionOrder::Strict })
	{
		const char* const orderName = order == ECompletionOrder::Strict ? "strict" : "batch";
		CWorkerPool pool({ .ThreadCount = 2u });
		pool.SetCompletionOrder(order);

		constexpr uint32 StepCount = 8u;
		uint32 ran = 0u;
		uint32 followUpsWaiting = 0u;
		std::function<void (uint32)> issue;
		issue = [&] (uint32 Step)
		{
			pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&, Step] ()
			{
				++ran;
				if (Step + 1u == StepCount)
				{
					return;
				}
				issue(Step + 1u);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				while (pool.GetStats().CompletionsWaiting == 0u && std::chrono::steady_clock::now() < deadline)
				{
					std::this_thread::yield();
				}
				followUpsWaiting += pool.GetStats().CompletionsWaiting == 1u ? 1u : 0u;
			});
		};
		issue(0u);
		ASSERT_TRUE(WaitForWorkToFinish(pool));

		for (uint32 step = 0u; step < StepCount; ++step)
		{
			ASSERT_EQ(pool.PumpCompletions(), 1u) << orderName << ", pump " << step;
			ASSERT_EQ(ran, step + 1u) << orderName;
		}
		EXPECT_EQ(followUpsWaiting, StepCount - 1u) << orderName;
		EXPECT_EQ(pool.PumpCompletions(), 0u) << orderName;
		EXPECT_TRUE(pool.IsIdle()) << orderName;
	}
}


// ---------------------------------------------------------------------------------
// Cancel groups
// ---------------------------------------------------------------------------------

TEST(WorkerPool, CancelDropsTheGroupsTasksThatHaveNotStarted)
{
	SGate gate;
	CWorkerPool pool({ .ThreadCount = 1u });
	const uint64 groupA = pool.CreateCancelGroup();
	const uint64 groupB = pool.CreateCancelGroup();
	EXPECT_NE(groupA, NoCancelGroup);
	EXPECT_NE(groupA, groupB);

	BlockTheWorker(pool, gate);
	std::atomic<uint32> workA = 0u;
	std::atomic<uint32> workOther = 0u;
	uint32 completionsA = 0u;
	uint32 completionsOther = 0u;
	for (uint32 i = 0u; i < 10u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, groupA, [&] () { workA.fetch_add(1u); }, [&] () { ++completionsA; });
		pool.Submit(EWorkPriority::Low, groupB, [&] () { workOther.fetch_add(1u); }, [&] () { ++completionsOther; });
		pool.Submit(EWorkPriority::High, NoCancelGroup, [&] () { workOther.fetch_add(1u); },
			[&] () { ++completionsOther; });
	}

	EXPECT_EQ(pool.Cancel(groupA), 10u);
	EXPECT_EQ(pool.Cancel(groupA), 0u);
	EXPECT_EQ(pool.Cancel(NoCancelGroup), 0u);
	gate.Open();
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));

	EXPECT_EQ(workA.load(), 0u);
	EXPECT_EQ(completionsA, 0u);
	EXPECT_EQ(workOther.load(), 20u);
	EXPECT_EQ(completionsOther, 20u);
	const SWorkerPoolStats stats = pool.GetStats();
	EXPECT_EQ(stats.Dropped, 10u);
	EXPECT_EQ(stats.Discarded, 0u);
	EXPECT_EQ(stats.CompletionsRun, 20u);
}

TEST(WorkerPool, RunningWorkSeesItsCancellationAndItsCompletionIsDiscarded)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	const uint64 group = pool.CreateCancelGroup();

	std::atomic<bool> bStarted = false;
	std::atomic<bool> bSawCancel = false;
	std::atomic<bool> bOtherSawCancel = true;
	bool bCompletionRan = false;
	pool.Submit(EWorkPriority::Normal, group, [&] ()
	{
		bStarted.store(true);
		bStarted.notify_all();
		RunUntilCancelled();
		bSawCancel.store(CWorkerPool::IsCancelled());
	}, [&] () { bCompletionRan = true; });
	bStarted.wait(false);
	EXPECT_FALSE(CWorkerPool::IsCancelled());

	// Nothing of the group was waiting to start: the running task is flagged, not dropped.
	EXPECT_EQ(pool.Cancel(group), 0u);
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] () { bOtherSawCancel.store(CWorkerPool::IsCancelled()); });
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));

	EXPECT_TRUE(bSawCancel.load());
	EXPECT_FALSE(bOtherSawCancel.load());
	EXPECT_FALSE(bCompletionRan);
	EXPECT_EQ(pool.GetStats().Discarded, 1u);
}

TEST(WorkerPool, CancelDiscardsTheGroupsCompletionsWaitingForThePump)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	const uint64 group = pool.CreateCancelGroup();
	uint32 grouped = 0u;
	uint32 ungrouped = 0u;
	for (uint32 i = 0u; i < 4u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, group, [] () {}, [&] () { ++grouped; });
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { ++ungrouped; });
	}
	ASSERT_TRUE(WaitForWorkToFinish(pool));
	EXPECT_EQ(pool.GetStats().CompletionsWaiting, 8u);

	EXPECT_EQ(pool.Cancel(group), 0u);
	EXPECT_EQ(pool.GetStats().CompletionsWaiting, 4u);
	EXPECT_EQ(pool.PumpCompletions(), 4u);
	EXPECT_EQ(grouped, 0u);
	EXPECT_EQ(ungrouped, 4u);
	EXPECT_EQ(pool.GetStats().Discarded, 4u);
}

TEST(WorkerPool, ACompletionCanCancelTheRestOfItsGroupInTheSamePump)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	const uint64 group = pool.CreateCancelGroup();
	std::vector<FString> ran;
	pool.Submit(EWorkPriority::Normal, group, [] () {}, [&] () { ran.emplace_back("G1"); pool.Cancel(group); });
	pool.Submit(EWorkPriority::Normal, group, [] () {}, [&] () { ran.emplace_back("G2"); });
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { ran.emplace_back("X"); });
	pool.Submit(EWorkPriority::Normal, group, [] () {}, [&] () { ran.emplace_back("G3"); });
	ASSERT_TRUE(WaitForWorkToFinish(pool));

	EXPECT_EQ(pool.PumpCompletions(), 2u);
	const std::vector<FString> expected = { "G1", "X" };
	EXPECT_EQ(ran, expected);
	EXPECT_TRUE(pool.IsIdle());
}

TEST(WorkerPool, CancelReachesOnlyTheTasksAlreadyInThePool)
{
	SGate gate;
	CWorkerPool pool({ .ThreadCount = 1u });
	const uint64 group = pool.CreateCancelGroup();
	BlockTheWorker(pool, gate);
	bool bEarlier = false;
	bool bLater = false;
	pool.Submit(EWorkPriority::Normal, group, [] () {}, [&] () { bEarlier = true; });
	EXPECT_EQ(pool.Cancel(group), 1u);
	pool.Submit(EWorkPriority::Normal, group, [] () {}, [&] () { bLater = true; });
	gate.Open();
	ASSERT_TRUE(pool.PumpUntil([&] () { return bLater; }, TestTimeoutSeconds));
	EXPECT_FALSE(bEarlier);
}

TEST(WorkerPool, ADiscardedCompletionDiesOnTheOwnerThread)
{
	// A completion's captures may hold TLSF memory (a TRefShared, a TArray), which only the owner
	// thread may free: a completion discarded on a worker, or by a Cancel from another thread, is
	// left for the owner's next pump to destroy, never destroyed where it was discarded.
	SDeathSites sites;
	SGate gate;
	std::atomic<bool> bStarted = false;
	CWorkerPool pool({ .ThreadCount = 1u });
	const auto cancelElsewhere = [&pool] (uint64 Group)
	{
		uint32 dropped = 0u;
		std::thread([&pool, &dropped, Group] () { dropped = pool.Cancel(Group); }).join();
		return dropped;
	};
	bool bRan = false;

	// Cancelled while it runs: the worker discards the completion.
	const uint64 running = pool.CreateCancelGroup();
	pool.Submit(EWorkPriority::Normal, running, [&bStarted] ()
	{
		bStarted.store(true);
		bStarted.notify_all();
		RunUntilCancelled();
	}, WitnessedCompletion(sites, &bRan));
	bStarted.wait(false);
	EXPECT_EQ(cancelElsewhere(running), 0u);
	ASSERT_TRUE(WaitForWorkToFinish(pool));

	// Dropped before they started.
	const uint64 queued = pool.CreateCancelGroup();
	BlockTheWorker(pool, gate);
	for (uint32 i = 0u; i < 3u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, queued, [] () {}, WitnessedCompletion(sites, &bRan));
	}
	EXPECT_EQ(cancelElsewhere(queued), 3u);
	gate.Open();
	ASSERT_TRUE(WaitForWorkToFinish(pool));

	// Finished, their completions waiting for the pump.
	const uint64 waiting = pool.CreateCancelGroup();
	for (uint32 i = 0u; i < 2u; ++i)
	{
		pool.Submit(EWorkPriority::Normal, waiting, [] () {}, WitnessedCompletion(sites, &bRan));
	}
	ASSERT_TRUE(WaitForWorkToFinish(pool));
	EXPECT_EQ(pool.GetStats().CompletionsWaiting, 2u);
	EXPECT_EQ(cancelElsewhere(waiting), 0u);
	EXPECT_EQ(pool.GetStats().CompletionsWaiting, 0u);

	// All six wait for the owner: the pool is not idle until it has destroyed them.
	EXPECT_EQ(sites.OnOwner.load(), 0u);
	EXPECT_EQ(sites.Elsewhere.load(), 0u);
	EXPECT_FALSE(pool.IsIdle());
	EXPECT_EQ(pool.PumpCompletions(), 0u);
	EXPECT_EQ(sites.OnOwner.load(), 6u);
	EXPECT_EQ(sites.Elsewhere.load(), 0u);
	EXPECT_TRUE(pool.IsIdle());
	EXPECT_FALSE(bRan);
	const SWorkerPoolStats stats = pool.GetStats();
	EXPECT_EQ(stats.Dropped, 3u);
	EXPECT_EQ(stats.Discarded, 3u);

	// A Cancel on the owner thread destroys what it discards at once.
	const uint64 local = pool.CreateCancelGroup();
	pool.Submit(EWorkPriority::Normal, local, [] () {}, WitnessedCompletion(sites, &bRan));
	ASSERT_TRUE(WaitForWorkToFinish(pool));
	pool.Cancel(local);
	EXPECT_EQ(sites.OnOwner.load(), 7u);
	EXPECT_EQ(sites.Elsewhere.load(), 0u);
	EXPECT_TRUE(pool.IsIdle());
}


// ---------------------------------------------------------------------------------
// Epochs
// ---------------------------------------------------------------------------------

TEST(WorkerPool, CompletionsOfAnOlderEpochAreDiscarded)
{
	SGate gate;
	CWorkerPool pool({ .ThreadCount = 2u });
	const uint64 first = pool.GetEpoch();
	bool bOld = false;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { bOld = true; });
	ASSERT_TRUE(WaitForWorkToFinish(pool));

	EXPECT_EQ(pool.BumpEpoch(), first + 1u);
	EXPECT_EQ(pool.GetEpoch(), first + 1u);
	EXPECT_EQ(pool.PumpCompletions(), 0u);
	EXPECT_FALSE(bOld);
	EXPECT_EQ(pool.GetStats().Discarded, 1u);

	// A task submitted before the bump and finishing after it is just as stale; one submitted after it
	// is not.
	bool bStraddling = false;
	bool bNew = false;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&gate] () { gate.Wait(); }, [&] () { bStraddling = true; });
	pool.BumpEpoch();
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { bNew = true; });
	gate.Open();
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));
	EXPECT_FALSE(bStraddling);
	EXPECT_TRUE(bNew);
}

TEST(WorkerPool, ACompletionThatBumpsTheEpochDiscardsTheOlderOnesAfterIt)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	std::vector<uint32> ran;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { ran.push_back(0u); pool.BumpEpoch(); });
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { ran.push_back(1u); });
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { ran.push_back(2u); });
	ASSERT_TRUE(WaitForWorkToFinish(pool));

	EXPECT_EQ(pool.PumpCompletions(), 1u);
	const std::vector<uint32> expected = { 0u };
	EXPECT_EQ(ran, expected);
	EXPECT_EQ(pool.GetStats().Discarded, 2u);
}


// ---------------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------------

TEST(WorkerPool, CancelAllAndDrainLeavesNothingBehindUnderStress)
{
	CWorkerPool pool({ .ThreadCount = 6u });
	std::minstd_rand random(20261024u);
	std::atomic<uint32> started = 0u;
	std::atomic<uint32> finished = 0u;
	std::atomic<uint32> running = 0u;
	uint64 completions = 0u;
	std::vector<uint64> groups;
	for (uint32 i = 0u; i < 4u; ++i)
	{
		groups.push_back(pool.CreateCancelGroup());
	}

	for (uint32 round = 0u; round < 6u; ++round)
	{
		const uint64 epochBefore = pool.GetEpoch();
		constexpr uint32 TaskCount = 3000u;
		for (uint32 i = 0u; i < TaskCount; ++i)
		{
			const auto priority = static_cast<EWorkPriority>(random() % WorkPriorityCount);
			const uint64 group = random() % 5u == 0u ? NoCancelGroup : groups[random() % groups.size()];
			const uint32 seed = static_cast<uint32>(random());
			pool.Submit(priority, group, [&, seed] ()
			{
				started.fetch_add(1u);
				running.fetch_add(1u);
				Busy(seed);
				running.fetch_sub(1u);
				finished.fetch_add(1u);
			}, [&] () { ++completions; });
			// Now and then: a pump, a cancel, a new epoch, while the rest is in flight.
			switch (random() % 500u)
			{
			case 0u: pool.PumpCompletions(); break;
			case 1u: pool.Cancel(groups[random() % groups.size()]); break;
			case 2u: pool.BumpEpoch(); break;
			default: break;
			}
		}
		if (round % 2u == 1u)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(random() % 5u));
		}

		pool.CancelAllAndDrain();

		// Nothing runs, waits or comes back afterwards; every work that started also finished.
		EXPECT_EQ(running.load(), 0u);
		EXPECT_EQ(started.load(), finished.load());
		const SWorkerPoolStats stats = pool.GetStats();
		EXPECT_EQ(stats.Queued, 0u);
		EXPECT_EQ(stats.Running, 0u);
		EXPECT_EQ(stats.CompletionsWaiting, 0u);
		EXPECT_TRUE(pool.IsIdle());
		EXPECT_GT(pool.GetEpoch(), epochBefore);
		const uint32 startedAfterDrain = started.load();
		const uint64 completionsAfterDrain = completions;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		EXPECT_EQ(pool.PumpCompletions(), 0u);
		EXPECT_EQ(started.load(), startedAfterDrain);
		EXPECT_EQ(completions, completionsAfterDrain);

		// Every task with a completion ended exactly one way.
		EXPECT_EQ(stats.Submitted, stats.CompletionsRun + stats.Discarded + stats.Dropped);
		EXPECT_EQ(stats.CompletionsRun, completions);
	}
}

TEST(WorkerPool, CancelAllAndDrainCancelsTheRunningWorkAndDropsWhatItSubmits)
{
	SDeathSites sites;
	std::atomic<bool> bStarted = false;
	std::atomic<uint64> submittedDuringDrain = 1u;
	CWorkerPool pool({ .ThreadCount = 2u });
	bool bCompletionRan = false;
	bool bDroppedRan = false;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] ()
	{
		bStarted.store(true);
		bStarted.notify_all();
		RunUntilCancelled();
		// The drain is under way: what the running work submits now is dropped.
		submittedDuringDrain.store(pool.Submit(EWorkPriority::High, NoCancelGroup, [] () {},
			WitnessedCompletion(sites, &bDroppedRan)));
	}, WitnessedCompletion(sites, &bCompletionRan));
	bStarted.wait(false);

	pool.CancelAllAndDrain();
	EXPECT_EQ(submittedDuringDrain.load(), 0u);
	EXPECT_FALSE(bCompletionRan);
	EXPECT_FALSE(bDroppedRan);
	// Both completions were discarded on the worker, and both died on the owner thread before the
	// drain returned.
	EXPECT_EQ(sites.OnOwner.load(), 2u);
	EXPECT_EQ(sites.Elsewhere.load(), 0u);
	EXPECT_EQ(pool.PumpCompletions(), 0u);

	// The pool takes work again.
	bool bAfter = false;
	EXPECT_NE(pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { bAfter = true; }), 0u);
	ASSERT_TRUE(pool.PumpUntil([&] () { return bAfter; }, TestTimeoutSeconds));
}

TEST(WorkerPool, DestroyingThePoolDropsQueuedWorkAndWaitsForTheRunningWork)
{
	std::atomic<uint32> queuedRan = 0u;
	std::atomic<bool> bRunningFinished = false;
	uint32 completions = 0u;
	{
		CWorkerPool pool({ .ThreadCount = 1u });
		std::atomic<bool> bStarted = false;
		pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] ()
		{
			bStarted.store(true);
			bStarted.notify_all();
			RunUntilCancelled();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			bRunningFinished.store(true);
		}, [&] () { ++completions; });
		bStarted.wait(false);
		for (uint32 i = 0u; i < 10u; ++i)
		{
			pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&] () { queuedRan.fetch_add(1u); },
				[&] () { ++completions; });
		}
	}
	EXPECT_TRUE(bRunningFinished.load());
	EXPECT_EQ(queuedRan.load(), 0u);
	EXPECT_EQ(completions, 0u);
}


// ---------------------------------------------------------------------------------
// Waiting: PumpUntil
// ---------------------------------------------------------------------------------

namespace
{
// A stand-in for the build scheduler (§7.8): each request is a chain of steps, and a step's
// completion, on the owner thread, issues the next step (a build that discovered an input and
// restarts) until the request is done. Nothing but completions moves a request forward, so a wait
// that blocked instead of pumping would never return.
class CFakeScheduler
{
public:
	CFakeScheduler (CWorkerPool& InPool, uint32 RequestCount)
		: Pool(InPool)
	{
		for (uint32 i = 0u; i < RequestCount; ++i)
		{
			StepsLeft.push_back(1u + (i * 5u) % 9u);
		}
	}

	void IssueAll ()
	{
		for (uint32 i = 0u; i < StepsLeft.size(); ++i)
		{
			Issue(i);
		}
	}

	[[nodiscard]] bool IsDone (uint32 Request) const { return StepsLeft[Request] == 0u; }
	[[nodiscard]] bool AllDone () const { return DoneCount == StepsLeft.size(); }

	// Waits for one request from inside a completion of another: a nested wait.
	void WaitInsideCompletion (uint32 Waiter, uint32 Awaited)
	{
		Pool.Submit(EWorkPriority::High, NoCancelGroup, [] () {}, [this, Waiter, Awaited] ()
		{
			bNestedWaitReturned = Pool.PumpUntil([this, Awaited] () { return IsDone(Awaited); }, TestTimeoutSeconds);
			Issue(Waiter);
		});
	}

	bool bNestedWaitReturned = false;

private:
	void Issue (uint32 Request)
	{
		const auto priority = static_cast<EWorkPriority>(Request % WorkPriorityCount);
		Pool.Submit(priority, NoCancelGroup, [Request] () { Busy(Request * 977u); }, [this, Request] ()
		{
			if (--StepsLeft[Request] == 0u)
			{
				++DoneCount;
			}
			else
			{
				Issue(Request);
			}
		});
	}

	CWorkerPool& Pool;
	std::vector<uint32> StepsLeft;
	uint32 DoneCount = 0u;
};
}

TEST(WorkerPool, AnOwnerThreadWaitingOnAFakeSchedulerNeverDeadlocks)
{
	for (const uint32 threadCount : { 1u, 4u })
	{
		for (const ECompletionOrder order : { ECompletionOrder::Batch, ECompletionOrder::Strict })
		{
			CWorkerPool pool({ .ThreadCount = threadCount });
			pool.SetCompletionOrder(order);
			CFakeScheduler scheduler(pool, 200u);
			scheduler.IssueAll();

			// One request first (an escalated blocking load), then all of them.
			ASSERT_TRUE(pool.PumpUntil([&] () { return scheduler.IsDone(17u); }, TestTimeoutSeconds))
				<< threadCount << " thread(s)";
			ASSERT_TRUE(pool.PumpUntil([&] () { return scheduler.AllDone(); }, TestTimeoutSeconds))
				<< threadCount << " thread(s)";
			EXPECT_TRUE(pool.IsIdle());
		}
	}
}

TEST(WorkerPool, APumpUntilNestedInACompletionReturns)
{
	CWorkerPool pool({ .ThreadCount = 1u });
	CFakeScheduler scheduler(pool, 20u);
	scheduler.IssueAll();
	scheduler.WaitInsideCompletion(0u, 19u);
	const auto done = [&] () { return scheduler.bNestedWaitReturned && scheduler.AllDone(); };
	ASSERT_TRUE(pool.PumpUntil(done, TestTimeoutSeconds));
}

TEST(WorkerPool, PumpUntilSeesWorkThatHasNoCompletion)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	std::atomic<bool> bDone = false;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [&bDone] ()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		bDone.store(true);
	});
	EXPECT_TRUE(pool.PumpUntil([&] () { return bDone.load(); }));
}

TEST(WorkerPool, PumpUntilWakesAsSoonAsThePoolGoesIdle)
{
	// Every way a task can end is an event for a waiting owner, the last worker going idle included:
	// a missed one shows as a wait that lasts until its timeout, and PumpUntil would still return
	// true after it, so each wait is timed. A cancelled task's completion is one more: the pool is
	// idle only once the owner's pump has destroyed it.
	CWorkerPool pool({ .ThreadCount = 2u });
	const uint64 group = pool.CreateCancelGroup();
	for (uint32 i = 0u; i < 90u; ++i)
	{
		std::atomic<bool> bStarted = false;
		switch (i % 3u)
		{
		case 0u:
			pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {});
			break;
		case 1u:
			pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [] () {});
			break;
		default:
			pool.Submit(EWorkPriority::Normal, group, [&bStarted] ()
			{
				bStarted.store(true);
				RunUntilCancelled();
			}, [] () {});
			while (!bStarted.load())
			{
				std::this_thread::yield();
			}
			pool.Cancel(group);
			break;
		}
		const auto start = std::chrono::steady_clock::now();
		ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, 10.0));
		ASSERT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5)) << "iteration " << i;
	}
	EXPECT_EQ(pool.GetStats().Discarded, 30u);
}

TEST(WorkerPool, PumpUntilReturnsAtOnceWhenNothingCouldMakeItTrue)
{
	CWorkerPool pool({ .ThreadCount = 2u });
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(pool.PumpUntil([] () { return false; }));
	EXPECT_TRUE(pool.PumpUntil([] () { return true; }));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST(WorkerPool, PumpUntilGivesUpAtItsTimeout)
{
	CWorkerPool pool({ .ThreadCount = 1u });
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, &RunUntilCancelled);
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(pool.PumpUntil([] () { return false; }, 0.05));
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	EXPECT_GE(elapsed, 0.045);
	EXPECT_LT(elapsed, 5.0);
	// The running work ends with the drain in the destructor.
}

TEST(WorkerPool, IsCancelledIsFalseOutsideWork)
{
	EXPECT_FALSE(CWorkerPool::IsCancelled());
	CWorkerPool pool({ .ThreadCount = 1u });
	bool bInCompletion = true;
	pool.Submit(EWorkPriority::Normal, NoCancelGroup, [] () {}, [&] () { bInCompletion = CWorkerPool::IsCancelled(); });
	ASSERT_TRUE(pool.PumpUntil([&] () { return pool.IsIdle(); }, TestTimeoutSeconds));
	EXPECT_FALSE(bInCompletion);
}
