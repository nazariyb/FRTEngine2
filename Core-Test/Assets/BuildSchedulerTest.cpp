// assets::CBuildScheduler, the build scheduler skeleton (design §7.8, §7.3, §7.9), with fake steps: steps
// run on the worker pool in (priority, sequence) order and Escalate moves a request ahead; two requests for
// one key share one run; a step that returns Need runs again once the steps it named are built, and sees
// their outputs; a fourth Need, or steps that need each other, fail with a cycle error instead of waiting
// forever; a failed or missing input fails its step without another run; cancelling a request cancels its
// step only when nothing else needs it, a running step sees IsCancelled and its inputs nobody else needs go
// too; every request's OnComplete runs exactly once, on the owner thread in a Tick, in request order with
// EBuildCommitOrder::Sequence whatever finishes first; a step whose run the pool discards (a new epoch,
// the module barrier) runs again, never beside its first run, and its request completes; and a random
// graph of steps under random delays
// always completes with the outputs a sequential build gives - an owner that waits by pumping never
// deadlocks.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "TestProcess.h"

#include "Assets/BuildScheduler.h"
#include "Jobs/WorkerPool.h"

using namespace frt;
using namespace frt::assets;


namespace
{
constexpr double TestTimeoutSeconds = 60.0;
// A condition only a worker changes (a step reached its gate) is polled for, not pumped for: the pool
// wakes a pump for completions, not for what a running step does.
constexpr std::chrono::milliseconds WorkerTimeout = std::chrono::seconds(60);

using FFakeStep = std::function<SBuildStepResult (CBuildStepContext&)>;

SBuildStep MakeStep (FStringView Key, FFakeStep Step)
{
	return SBuildStep{ FString(Key), [Step = std::move(Step)] (CBuildStepContext& Context) { return Step(Context); } };
}

// Fake steps by key, for the resolver; filled before the scheduler runs, read on the owner thread only.
class CFakeSteps
{
public:
	void Add (FStringView Key, FFakeStep Step)
	{
		Steps[FString(Key)] = std::move(Step);
	}

	FBuildStepResolver MakeResolver ()
	{
		return [this] (FStringView Key) -> std::optional<SBuildStep>
		{
			const auto it = Steps.find(FString(Key));
			if (it == Steps.end())
			{
				return std::nullopt;
			}
			return MakeStep(Key, it->second);
		};
	}

private:
	std::map<FString, FFakeStep> Steps;
};

// What ran, in the order it started, from any thread.
class CRunLog
{
public:
	void Add (FStringView Key)
	{
		const std::lock_guard lock(Mutex);
		Runs.emplace_back(Key);
	}

	[[nodiscard]] std::vector<FString> Get () const
	{
		const std::lock_guard lock(Mutex);
		return Runs;
	}

	[[nodiscard]] size_t Count (FStringView Key) const
	{
		const std::lock_guard lock(Mutex);
		return static_cast<size_t>(std::count(Runs.begin(), Runs.end(), Key));
	}

private:
	mutable std::mutex Mutex;
	std::vector<FString> Runs;
};

// Opened once by the test; a step waits on it. Declared before the pool, so it outlives the work.
struct SGate
{
	std::atomic<bool> bOpen = false;
	std::atomic<bool> bReached = false;

	void Open ()
	{
		bOpen.store(true);
	}

	// Returns once the gate opens or the step is cancelled (or a minute passes: a broken test ends).
	void Wait ()
	{
		bReached.store(true);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!bOpen.load() && !CBuildStepContext::IsCancelled() && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	}
};

// A step that waits for Gate, then succeeds with its key's bytes.
FFakeStep GatedStep (SGate& Gate, CRunLog& Log)
{
	return [&Gate, &Log] (CBuildStepContext& Context)
	{
		Log.Add(Context.GetKey());
		Gate.Wait();
		return SBuildStepResult::Success(std::vector<uint8>(Context.GetKey().begin(), Context.GetKey().end()));
	};
}

// A step that logs and succeeds with its key's bytes.
FFakeStep QuickStep (CRunLog& Log)
{
	return [&Log] (CBuildStepContext& Context)
	{
		Log.Add(Context.GetKey());
		return SBuildStepResult::Success(std::vector<uint8>(Context.GetKey().begin(), Context.GetKey().end()));
	};
}

std::vector<uint8> Bytes (FStringView Text)
{
	return std::vector<uint8>(Text.begin(), Text.end());
}

bool AnyContains (const std::vector<FString>& Lines, FStringView Text)
{
	return std::any_of(Lines.begin(), Lines.end(), [&] (const FString& Line) { return Line.Contains(Text); });
}

FString Join (const std::vector<FString>& Lines)
{
	FString text;
	for (const FString& line : Lines)
	{
		text.AppendFormat("{}{}", text.IsEmpty() ? "" : " | ", line);
	}
	return text;
}

// Every outcome a test's requests got, in the order their OnComplete ran.
struct SOutcomes
{
	std::vector<SBuildOutcome> List;

	FBuildCompletion Collect ()
	{
		return [this] (const SBuildOutcome& Outcome) { List.push_back(Outcome); };
	}

	[[nodiscard]] const SBuildOutcome* Find (uint64 Request) const
	{
		for (const SBuildOutcome& outcome : List)
		{
			if (outcome.Request == Request)
			{
				return &outcome;
			}
		}
		return nullptr;
	}
};

jobs::SWorkerPoolDesc Threads (uint32 Count)
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = Count;
	return desc;
}
}


TEST(BuildSchedulerTest, ARequestRunsItsStepOnAWorkerAndCommitsOnTheOwnerThread)
{
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool);
	std::atomic<bool> bOnWorker = false;
	const std::thread::id owner = std::this_thread::get_id();
	std::optional<SBuildOutcome> result;
	std::thread::id callbackThread;

	const uint64 request = scheduler.Request(MakeStep("A", [&] (CBuildStepContext& Context)
	{
		bOnWorker.store(jobs::CWorkerPool::IsWorkerThread());
		EXPECT_EQ(Context.GetRestartCount(), 0u);
		return SBuildStepResult::Success(Bytes("output"));
	}), EBuildPriority::Visible, [&] (const SBuildOutcome& Outcome)
	{
		result = Outcome;
		callbackThread = std::this_thread::get_id();
	});
	EXPECT_EQ(request, 1u);
	EXPECT_FALSE(result.has_value());   // never inside Request

	ASSERT_TRUE(scheduler.PumpUntil([&] { return result.has_value(); }, TestTimeoutSeconds));
	EXPECT_TRUE(bOnWorker.load());
	EXPECT_EQ(callbackThread, owner);
	EXPECT_EQ(result->Request, request);
	EXPECT_EQ(result->Key, "A");
	EXPECT_EQ(result->Outcome, EBuildOutcome::Succeeded);
	ASSERT_NE(result->Output, nullptr);
	EXPECT_EQ(*result->Output, Bytes("output"));
	EXPECT_EQ(result->Restarts, 0u);
	EXPECT_TRUE(scheduler.IsIdle());
	const SBuildSchedulerStats stats = scheduler.GetStats();
	EXPECT_EQ(stats.Requests, 1u);
	EXPECT_EQ(stats.Runs, 1u);
	EXPECT_EQ(stats.Succeeded, 1u);
	EXPECT_EQ(stats.Committed, 1u);
}

TEST(BuildSchedulerTest, StepsStartInPriorityThenSequenceOrder)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(1));
	SBuildSchedulerDesc desc;
	desc.MaxInFlight = 1u;
	CBuildScheduler scheduler(pool, nullptr, desc);
	SOutcomes outcomes;

	scheduler.Request(MakeStep("Block", GatedStep(gate, log)), EBuildPriority::Escalated, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return gate.bReached.load(); }, WorkerTimeout));
	scheduler.Request(MakeStep("c1", QuickStep(log)), EBuildPriority::Cook, outcomes.Collect());
	scheduler.Request(MakeStep("b1", QuickStep(log)), EBuildPriority::Background, outcomes.Collect());
	scheduler.Request(MakeStep("v1", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	scheduler.Request(MakeStep("e1", QuickStep(log)), EBuildPriority::Escalated, outcomes.Collect());
	scheduler.Request(MakeStep("b2", QuickStep(log)), EBuildPriority::Background, outcomes.Collect());
	scheduler.Request(MakeStep("v2", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	EXPECT_EQ(scheduler.GetStats().Queued, 6u);

	gate.Open();
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 7u; }, TestTimeoutSeconds));
	EXPECT_EQ(log.Get(), (std::vector<FString>{ "Block", "e1", "v1", "v2", "b1", "b2", "c1" }));
}

TEST(BuildSchedulerTest, EscalateMovesAQueuedRequestAheadWithWhatItNeeds)
{
	SGate gate;
	CRunLog log;
	CFakeSteps steps;
	steps.Add("x.input", QuickStep(log));
	jobs::CWorkerPool pool(Threads(1));
	SBuildSchedulerDesc desc;
	desc.MaxInFlight = 1u;
	CBuildScheduler scheduler(pool, steps.MakeResolver(), desc);
	SOutcomes outcomes;

	scheduler.Request(MakeStep("Block", GatedStep(gate, log)), EBuildPriority::Escalated, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return gate.bReached.load(); }, WorkerTimeout));
	const uint64 x = scheduler.Request(MakeStep("x", [&log] (CBuildStepContext& Context)
	{
		log.Add(Context.GetKey());
		return Context.FindInput("x.input") ? SBuildStepResult::Success() : SBuildStepResult::Need({ "x.input" });
	}), EBuildPriority::Background, outcomes.Collect());
	scheduler.Request(MakeStep("y", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	scheduler.Request(MakeStep("z", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	EXPECT_TRUE(scheduler.Escalate(x, EBuildPriority::Escalated));
	EXPECT_FALSE(scheduler.Escalate(12345u, EBuildPriority::Escalated));

	gate.Open();
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 4u; }, TestTimeoutSeconds));
	// x first; its input inherits its urgency and goes before y and z too; then x again.
	EXPECT_EQ(log.Get(), (std::vector<FString>{ "Block", "x", "x.input", "x", "y", "z" }));
}

TEST(BuildSchedulerTest, TwoRequestsForOneKeyShareOneRun)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool);
	SOutcomes outcomes;

	const uint64 first = scheduler.Request(MakeStep("Shared", GatedStep(gate, log)), EBuildPriority::Background, outcomes.Collect());
	const uint64 second = scheduler.Request(MakeStep("Shared", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	gate.Open();
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));

	EXPECT_EQ(log.Count("Shared"), 1u);
	const SBuildOutcome* a = outcomes.Find(first);
	const SBuildOutcome* b = outcomes.Find(second);
	ASSERT_TRUE(a && b);
	EXPECT_EQ(a->Outcome, EBuildOutcome::Succeeded);
	EXPECT_EQ(b->Outcome, EBuildOutcome::Succeeded);
	EXPECT_EQ(a->Output, b->Output);   // the same output, shared
	EXPECT_EQ(scheduler.GetStats().StepsCreated, 1u);

	// Once nothing holds it, the step is gone: a new request runs it anew.
	EXPECT_FALSE(scheduler.FindStepState("Shared").has_value());
	scheduler.Request(MakeStep("Shared", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 3u; }, TestTimeoutSeconds));
	EXPECT_EQ(log.Count("Shared"), 2u);
}

TEST(BuildSchedulerTest, AStepThatNeedsInputsRunsAgainWithTheirOutputs)
{
	CRunLog log;
	CFakeSteps steps;
	// A needs B and C; B needs D. Each succeeds with its inputs' outputs and its own key appended.
	const auto composite = [&log] (std::vector<FString> Needs)
	{
		return [&log, Needs] (CBuildStepContext& Context)
		{
			log.Add(Context.GetKey());
			std::vector<uint8> output;
			std::vector<FString> missing;
			for (const FString& need : Needs)
			{
				if (const std::vector<uint8>* input = Context.FindInput(need))
				{
					output.insert(output.end(), input->begin(), input->end());
				}
				else
				{
					missing.push_back(need);
				}
			}
			if (!missing.empty())
			{
				return SBuildStepResult::Need(missing);
			}
			output.insert(output.end(), Context.GetKey().begin(), Context.GetKey().end());
			return SBuildStepResult::Success(output);
		};
	};
	steps.Add("A", composite({ "B", "C" }));
	steps.Add("B", composite({ "D" }));
	steps.Add("C", composite({}));
	steps.Add("D", composite({}));
	jobs::CWorkerPool pool(Threads(3));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;

	const uint64 request = scheduler.Request("A", EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 1u; }, TestTimeoutSeconds));
	const SBuildOutcome& outcome = outcomes.List[0];
	EXPECT_EQ(outcome.Request, request);
	ASSERT_EQ(outcome.Outcome, EBuildOutcome::Succeeded) << Join(outcome.Diagnostics).CStr();
	EXPECT_EQ(*outcome.Output, Bytes("DBCA"));
	EXPECT_EQ(outcome.Restarts, 1u);
	EXPECT_EQ(log.Count("A"), 2u);
	EXPECT_EQ(log.Count("B"), 2u);
	EXPECT_EQ(log.Count("C"), 1u);
	EXPECT_EQ(log.Count("D"), 1u);
	EXPECT_EQ(scheduler.GetStats().Restarts, 2u);
	// The inputs went with the step that needed them.
	EXPECT_TRUE(scheduler.IsIdle());
}

TEST(BuildSchedulerTest, AFourthNeedFailsTheStepAsACycle)
{
	CRunLog log;
	CFakeSteps steps;
	for (uint32 i = 0u; i < 10u; ++i)
	{
		steps.Add(FString::Format("G{}", i), QuickStep(log));
	}
	// Discovers a new input on every run, forever.
	steps.Add("Greedy", [&log] (CBuildStepContext& Context)
	{
		log.Add(Context.GetKey());
		return SBuildStepResult::Need({ FString::Format("G{}", Context.GetRestartCount()) });
	});
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;

	scheduler.Request("Greedy", EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 1u; }, TestTimeoutSeconds));
	const SBuildOutcome& outcome = outcomes.List[0];
	EXPECT_EQ(outcome.Outcome, EBuildOutcome::Failed);
	EXPECT_TRUE(AnyContains(outcome.Diagnostics, "cycle")) << Join(outcome.Diagnostics).CStr();
	EXPECT_TRUE(AnyContains(outcome.Diagnostics, "after 3 restarts")) << Join(outcome.Diagnostics).CStr();
	EXPECT_EQ(outcome.Restarts, MaxBuildStepRestarts);
	EXPECT_EQ(log.Count("Greedy"), MaxBuildStepRestarts + 1u);
	EXPECT_EQ(log.Count("G3"), 0u);
	EXPECT_TRUE(scheduler.IsIdle());
}

TEST(BuildSchedulerTest, StepsThatNeedEachOtherFailWithACycleInsteadOfWaiting)
{
	CRunLog log;
	CFakeSteps steps;
	const auto needs = [&log] (FStringView Need)
	{
		return [&log, need = FString(Need)] (CBuildStepContext& Context)
		{
			log.Add(Context.GetKey());
			return Context.FindInput(need) ? SBuildStepResult::Success() : SBuildStepResult::Need({ need });
		};
	};
	steps.Add("A", needs("B"));
	steps.Add("B", needs("C"));
	steps.Add("C", needs("A"));
	steps.Add("Self", needs("Self"));
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;

	const uint64 loop = scheduler.Request("A", EBuildPriority::Visible, outcomes.Collect());
	const uint64 self = scheduler.Request("Self", EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));

	const SBuildOutcome* a = outcomes.Find(loop);
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(a->Outcome, EBuildOutcome::Failed);
	// C closes the loop and fails with it; B and A fail because their input did.
	EXPECT_TRUE(AnyContains(a->Diagnostics, "its input 'B' failed")) << Join(a->Diagnostics).CStr();
	const SBuildOutcome* s = outcomes.Find(self);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->Outcome, EBuildOutcome::Failed);
	EXPECT_TRUE(AnyContains(s->Diagnostics, "cycle: Self -> Self")) << Join(s->Diagnostics).CStr();
	EXPECT_TRUE(scheduler.IsIdle());

	// Asked for directly, the step that closes the loop names it.
	SOutcomes direct;
	scheduler.Request("C", EBuildPriority::Visible, direct.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return direct.List.size() == 1u; }, TestTimeoutSeconds));
	EXPECT_TRUE(AnyContains(direct.List[0].Diagnostics, "its input 'A' failed")) << Join(direct.List[0].Diagnostics).CStr();
	EXPECT_EQ(log.Count("A"), 2u);   // once for each request of the loop
}

TEST(BuildSchedulerTest, ACycleErrorNamesTheLoop)
{
	CFakeSteps steps;
	steps.Add("P", [] (CBuildStepContext& Context) { return Context.FindInput("Q") ? SBuildStepResult::Success() : SBuildStepResult::Need({ "Q" }); });
	steps.Add("Q", [] (CBuildStepContext& Context) { return Context.FindInput("P") ? SBuildStepResult::Success() : SBuildStepResult::Need({ "P" }); });
	jobs::CWorkerPool pool(Threads(1));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;
	// Q's own request sees the cycle error Q got when it named P, which waits for Q.
	scheduler.Request("P", EBuildPriority::Visible, outcomes.Collect());
	const uint64 q = scheduler.Request("Q", EBuildPriority::Background, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));
	const SBuildOutcome* outcome = outcomes.Find(q);
	ASSERT_NE(outcome, nullptr);
	EXPECT_EQ(outcome->Outcome, EBuildOutcome::Failed);
	EXPECT_TRUE(AnyContains(outcome->Diagnostics, "cycle: Q -> P -> Q")) << Join(outcome->Diagnostics).CStr();
}

TEST(BuildSchedulerTest, AFailedOrMissingInputFailsTheStepWithoutAnotherRun)
{
	CRunLog log;
	CFakeSteps steps;
	steps.Add("Bad", [&log] (CBuildStepContext& Context)
	{
		log.Add(Context.GetKey());
		return SBuildStepResult::Failure("broken source");
	});
	steps.Add("NeedsBad", [&log] (CBuildStepContext& Context)
	{
		log.Add(Context.GetKey());
		return SBuildStepResult::Need({ "Bad" });
	});
	steps.Add("NeedsMissing", [&log] (CBuildStepContext& Context)
	{
		log.Add(Context.GetKey());
		return SBuildStepResult::Need({ "Missing" });
	});
	steps.Add("NeedsNothing", [] (CBuildStepContext&) { return SBuildStepResult::Need({}); });
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;

	const uint64 bad = scheduler.Request("NeedsBad", EBuildPriority::Visible, outcomes.Collect());
	const uint64 missing = scheduler.Request("NeedsMissing", EBuildPriority::Visible, outcomes.Collect());
	const uint64 nothing = scheduler.Request("NeedsNothing", EBuildPriority::Visible, outcomes.Collect());
	const uint64 unknown = scheduler.Request("Unknown", EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 4u; }, TestTimeoutSeconds));

	EXPECT_EQ(outcomes.Find(bad)->Outcome, EBuildOutcome::Failed);
	EXPECT_TRUE(AnyContains(outcomes.Find(bad)->Diagnostics, "its input 'Bad' failed")) << Join(outcomes.Find(bad)->Diagnostics).CStr();
	EXPECT_EQ(log.Count("NeedsBad"), 1u);
	EXPECT_EQ(log.Count("Bad"), 1u);
	EXPECT_EQ(outcomes.Find(missing)->Outcome, EBuildOutcome::Failed);
	EXPECT_TRUE(AnyContains(outcomes.Find(missing)->Diagnostics, "needs 'Missing', and there is no build step for it"));
	EXPECT_EQ(outcomes.Find(nothing)->Outcome, EBuildOutcome::Failed);
	EXPECT_TRUE(AnyContains(outcomes.Find(nothing)->Diagnostics, "named none"));
	EXPECT_EQ(outcomes.Find(unknown)->Outcome, EBuildOutcome::Failed);
	EXPECT_EQ(outcomes.Find(unknown)->Key, "Unknown");
	EXPECT_TRUE(AnyContains(outcomes.Find(unknown)->Diagnostics, "no build step for 'Unknown'"));

	// A failed step's own diagnostics reach its request.
	SOutcomes direct;
	scheduler.Request("Bad", EBuildPriority::Visible, direct.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return direct.List.size() == 1u; }, TestTimeoutSeconds));
	EXPECT_EQ(direct.List[0].Diagnostics, std::vector<FString>{ "broken source" });
}

TEST(BuildSchedulerTest, CancellingAQueuedRequestDropsItsStep)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(1));
	SBuildSchedulerDesc desc;
	desc.MaxInFlight = 1u;
	CBuildScheduler scheduler(pool, nullptr, desc);
	SOutcomes outcomes;

	const uint64 block = scheduler.Request(MakeStep("Block", GatedStep(gate, log)), EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return gate.bReached.load(); }, WorkerTimeout));
	const uint64 queued = scheduler.Request(MakeStep("Queued", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	EXPECT_EQ(scheduler.FindStepState("Queued"), EBuildStepState::Queued);

	EXPECT_TRUE(scheduler.Cancel(queued));
	EXPECT_FALSE(scheduler.Cancel(queued));
	EXPECT_FALSE(scheduler.FindStepState("Queued").has_value());
	EXPECT_TRUE(outcomes.List.empty());   // committed in a Tick, not inside Cancel
	scheduler.Tick();
	ASSERT_EQ(outcomes.List.size(), 1u);
	EXPECT_EQ(outcomes.List[0].Request, queued);
	EXPECT_EQ(outcomes.List[0].Key, "Queued");
	EXPECT_EQ(outcomes.List[0].Outcome, EBuildOutcome::Cancelled);

	gate.Open();
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.Find(block) != nullptr; }, TestTimeoutSeconds));
	EXPECT_EQ(log.Count("Queued"), 0u);
	EXPECT_EQ(scheduler.GetStats().Cancelled, 1u);
}

TEST(BuildSchedulerTest, CancellingARunningRequestCancelsItsStepAndARequestAfterRunsAnew)
{
	std::atomic<bool> bStarted = false;
	std::atomic<bool> bSawCancel = false;
	std::atomic<uint32> runs = 0u;
	CFakeSteps steps;
	steps.Add("Long", [&] (CBuildStepContext&)
	{
		if (runs.fetch_add(1u) > 0u)
		{
			return SBuildStepResult::Success(Bytes("second"));
		}
		bStarted.store(true);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!CBuildStepContext::IsCancelled() && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
		}
		bSawCancel.store(CBuildStepContext::IsCancelled());
		return SBuildStepResult::Success(Bytes("first"));
	});
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;

	const uint64 first = scheduler.Request("Long", EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return bStarted.load(); }, WorkerTimeout));
	EXPECT_EQ(scheduler.FindStepState("Long"), EBuildStepState::Running);
	EXPECT_TRUE(scheduler.Cancel(first));
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 1u; }, TestTimeoutSeconds));
	EXPECT_EQ(outcomes.List[0].Outcome, EBuildOutcome::Cancelled);
	EXPECT_EQ(outcomes.List[0].Output, nullptr);
	EXPECT_TRUE(test::WaitUntil([&] { return bSawCancel.load(); }));

	const uint64 second = scheduler.Request("Long", EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));
	EXPECT_EQ(outcomes.Find(second)->Outcome, EBuildOutcome::Succeeded);
	EXPECT_EQ(*outcomes.Find(second)->Output, Bytes("second"));
}

// §16.2 M6: "cancelling one of two requests for a shared asset leaves it loading and never Failed".
TEST(BuildSchedulerTest, CancellingOneOfTwoRequestsForAStepLeavesItRunning)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool);
	SOutcomes outcomes;

	const uint64 first = scheduler.Request(MakeStep("Shared", GatedStep(gate, log)), EBuildPriority::Visible, outcomes.Collect());
	const uint64 second = scheduler.Request(MakeStep("Shared", GatedStep(gate, log)), EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return gate.bReached.load(); }, WorkerTimeout));
	EXPECT_TRUE(scheduler.Cancel(first));
	EXPECT_EQ(scheduler.FindStepState("Shared"), EBuildStepState::Running);
	gate.Open();
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));
	EXPECT_EQ(outcomes.Find(first)->Outcome, EBuildOutcome::Cancelled);
	EXPECT_EQ(outcomes.Find(second)->Outcome, EBuildOutcome::Succeeded);
	EXPECT_EQ(log.Count("Shared"), 1u);
	EXPECT_EQ(scheduler.GetStats().Cancelled, 0u);
}

TEST(BuildSchedulerTest, CancellingARequestReleasesTheInputsNothingElseNeeds)
{
	SGate gate;
	CRunLog log;
	CFakeSteps steps;
	steps.Add("Own", GatedStep(gate, log));
	steps.Add("Common", GatedStep(gate, log));
	steps.Add("Top", [&log] (CBuildStepContext& Context)
	{
		log.Add(Context.GetKey());
		return Context.FindInput("Own") ? SBuildStepResult::Success() : SBuildStepResult::Need({ "Own", "Common" });
	});
	jobs::CWorkerPool pool(Threads(3));
	CBuildScheduler scheduler(pool, steps.MakeResolver());
	SOutcomes outcomes;

	const uint64 top = scheduler.Request("Top", EBuildPriority::Visible, outcomes.Collect());
	const uint64 common = scheduler.Request("Common", EBuildPriority::Background, outcomes.Collect());
	ASSERT_TRUE(scheduler.PumpUntil([&] { return scheduler.FindStepState("Top") == EBuildStepState::WaitingForInputs; }, TestTimeoutSeconds));
	EXPECT_TRUE(scheduler.Cancel(top));
	EXPECT_FALSE(scheduler.FindStepState("Top").has_value());
	EXPECT_FALSE(scheduler.FindStepState("Own").has_value());       // needed by Top alone: cancelled
	EXPECT_TRUE(scheduler.FindStepState("Common").has_value());     // its own request still wants it

	gate.Open();
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));
	EXPECT_EQ(outcomes.Find(top)->Outcome, EBuildOutcome::Cancelled);
	EXPECT_EQ(outcomes.Find(common)->Outcome, EBuildOutcome::Succeeded);
	EXPECT_EQ(log.Count("Top"), 1u);
	EXPECT_TRUE(scheduler.IsIdle());
}

TEST(BuildSchedulerTest, SequenceOrderCommitsInRequestOrderWhateverFinishesFirst)
{
	for (const EBuildCommitOrder order : { EBuildCommitOrder::Sequence, EBuildCommitOrder::Completion })
	{
		SCOPED_TRACE(order == EBuildCommitOrder::Sequence ? "Sequence" : "Completion");
		SGate gates[3];
		CRunLog log;
		jobs::CWorkerPool pool(Threads(3));
		SBuildSchedulerDesc desc;
		desc.CommitOrder = order;
		CBuildScheduler scheduler(pool, nullptr, desc);
		std::vector<FString> committed;
		const auto collect = [&committed] (const SBuildOutcome& Outcome) { committed.push_back(Outcome.Key); };

		for (uint32 i = 0u; i < 3u; ++i)
		{
			scheduler.Request(MakeStep(FString::Format("{}", i + 1u), GatedStep(gates[i], log)), EBuildPriority::Visible, collect);
		}
		ASSERT_TRUE(test::WaitUntil([&] { return gates[0].bReached && gates[1].bReached && gates[2].bReached; }, WorkerTimeout));
		// They finish last to first, each committed (or not) before the next finishes.
		for (int32 i = 2; i >= 0; --i)
		{
			gates[i].Open();
			ASSERT_TRUE(scheduler.PumpUntil([&] { return scheduler.FindStepState(FString::Format("{}", i + 1)) == EBuildStepState::Succeeded
				|| !scheduler.FindStepState(FString::Format("{}", i + 1)).has_value(); }, TestTimeoutSeconds));
			scheduler.Tick();
		}
		ASSERT_TRUE(scheduler.PumpUntil([&] { return committed.size() == 3u; }, TestTimeoutSeconds));
		if (order == EBuildCommitOrder::Sequence)
		{
			EXPECT_EQ(committed, (std::vector<FString>{ "1", "2", "3" }));
		}
		else
		{
			EXPECT_EQ(committed, (std::vector<FString>{ "3", "2", "1" }));
		}
	}
}

TEST(BuildSchedulerTest, ACallbackMayMakeAndCancelRequests)
{
	CRunLog log;
	jobs::CWorkerPool pool(Threads(2));
	CBuildScheduler scheduler(pool);
	SOutcomes outcomes;
	uint64 next = 0u;

	scheduler.Request(MakeStep("First", QuickStep(log)), EBuildPriority::Visible, [&] (const SBuildOutcome& Outcome)
	{
		outcomes.List.push_back(Outcome);
		next = scheduler.Request(MakeStep("Next", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
		const uint64 dropped = scheduler.Request(MakeStep("Dropped", QuickStep(log)), EBuildPriority::Background, outcomes.Collect());
		EXPECT_TRUE(scheduler.Cancel(dropped));
	});
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 3u; }, TestTimeoutSeconds));
	ASSERT_NE(outcomes.Find(next), nullptr);
	EXPECT_EQ(outcomes.Find(next)->Outcome, EBuildOutcome::Succeeded);
	EXPECT_TRUE(scheduler.IsIdle());
}

TEST(BuildSchedulerTest, PumpUntilEndsWhenNothingIsOpenOrTheTimeRunsOut)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(1));
	CBuildScheduler scheduler(pool);
	EXPECT_FALSE(scheduler.PumpUntil([] { return false; }));   // nothing open: it would never end

	SOutcomes outcomes;
	const uint64 request = scheduler.Request(MakeStep("Stuck", GatedStep(gate, log)), EBuildPriority::Visible, outcomes.Collect());
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(scheduler.PumpUntil([] { return false; }, 0.05));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
	EXPECT_TRUE(scheduler.Cancel(request));
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 1u; }, TestTimeoutSeconds));
	EXPECT_EQ(outcomes.List[0].Outcome, EBuildOutcome::Cancelled);
}

TEST(BuildSchedulerTest, DestroyingTheSchedulerCancelsItsWorkWithoutCallingBack)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(1));
	bool bCalled = false;
	{
		CBuildScheduler scheduler(pool);
		scheduler.Request(MakeStep("Running", GatedStep(gate, log)), EBuildPriority::Visible, [&] (const SBuildOutcome&) { bCalled = true; });
		scheduler.Request(MakeStep("Queued", QuickStep(log)), EBuildPriority::Visible, [&] (const SBuildOutcome&) { bCalled = true; });
		ASSERT_TRUE(test::WaitUntil([&] { return gate.bReached.load(); }, WorkerTimeout));
	}
	pool.CancelAllAndDrain();
	pool.PumpCompletions();
	EXPECT_FALSE(bCalled);
	EXPECT_TRUE(pool.IsIdle());
	EXPECT_EQ(log.Count("Queued"), 0u);
}

// The pool discards, unrun, the completion of a task from an older epoch (a module reload, §6.7). A step
// that was running when the epoch moved on runs again once its first run has returned - never beside it,
// even when that run ignores the cancel - and one the pool had queued but not started is dropped and runs
// once; both requests complete, with the outputs of the runs after the new epoch.
TEST(BuildSchedulerTest, StepsCaughtByANewEpochRunAgainAndTheirRequestsComplete)
{
	std::atomic<uint32> longRuns = 0u;
	std::atomic<uint32> longActive = 0u;
	std::atomic<uint32> longMostActive = 0u;
	std::atomic<bool> bRelease = false;
	std::atomic<bool> bFirstSawCancel = false;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(1));
	SBuildSchedulerDesc desc;
	desc.MaxInFlight = 2u;   // "Next" waits in the pool's queue behind "Long"
	CBuildScheduler scheduler(pool, nullptr, desc);
	SOutcomes outcomes;

	const uint64 longRequest = scheduler.Request(MakeStep("Long", [&] (CBuildStepContext&)
	{
		const uint32 active = longActive.fetch_add(1u) + 1u;
		uint32 most = longMostActive.load();
		while (most < active && !longMostActive.compare_exchange_weak(most, active))
		{
		}
		const uint32 run = longRuns.fetch_add(1u);
		if (run == 0u)
		{
			// A long first run that never polls for cancellation: it ends when the test lets it.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (!bRelease.load() && std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}
			bFirstSawCancel.store(CBuildStepContext::IsCancelled());
		}
		longActive.fetch_sub(1u);
		return SBuildStepResult::Success(Bytes(run == 0u ? "first" : "rerun"));
	}), EBuildPriority::Visible, outcomes.Collect());
	const uint64 nextRequest = scheduler.Request(MakeStep("Next", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return longRuns.load() == 1u; }, WorkerTimeout));
	EXPECT_EQ(scheduler.GetStats().Running, 2u);

	pool.BumpEpoch();
	// The first run of "Long" still holds the step: nothing runs beside it, and nothing can commit.
	EXPECT_FALSE(scheduler.PumpUntil([&] { return !outcomes.List.empty(); }, 0.2));
	EXPECT_EQ(longRuns.load(), 1u);
	EXPECT_EQ(scheduler.FindStepState("Long"), EBuildStepState::Running);
	EXPECT_EQ(log.Count("Next"), 0u);
	EXPECT_EQ(scheduler.GetStats().Reruns, 1u);   // "Next", dropped unstarted and submitted again

	bRelease.store(true);
	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));
	EXPECT_TRUE(bFirstSawCancel.load());
	EXPECT_EQ(longRuns.load(), 2u);
	EXPECT_EQ(longMostActive.load(), 1u);
	EXPECT_EQ(log.Count("Next"), 1u);
	const SBuildOutcome* longOutcome = outcomes.Find(longRequest);
	ASSERT_NE(longOutcome, nullptr);
	EXPECT_EQ(longOutcome->Outcome, EBuildOutcome::Succeeded);
	ASSERT_NE(longOutcome->Output, nullptr);
	EXPECT_EQ(*longOutcome->Output, Bytes("rerun"));
	EXPECT_EQ(longOutcome->Restarts, 0u);
	ASSERT_NE(outcomes.Find(nextRequest), nullptr);
	EXPECT_EQ(outcomes.Find(nextRequest)->Outcome, EBuildOutcome::Succeeded);
	const SBuildSchedulerStats stats = scheduler.GetStats();
	EXPECT_EQ(stats.Runs, 4u);
	EXPECT_EQ(stats.Reruns, 2u);
	EXPECT_EQ(stats.Succeeded, 2u);
	EXPECT_TRUE(scheduler.IsIdle());
}

// CancelAllAndDrain - the module barrier - cancels the running step, drops the queued one and starts a new
// epoch, discarding both completions: the next Tick runs both again and their requests complete.
TEST(BuildSchedulerTest, StepsTheModuleBarrierDrainedRunAgainAndTheirRequestsComplete)
{
	SGate gate;
	CRunLog log;
	jobs::CWorkerPool pool(Threads(1));
	SBuildSchedulerDesc desc;
	desc.MaxInFlight = 2u;
	CBuildScheduler scheduler(pool, nullptr, desc);
	SOutcomes outcomes;

	scheduler.Request(MakeStep("Running", GatedStep(gate, log)), EBuildPriority::Visible, outcomes.Collect());
	scheduler.Request(MakeStep("Queued", QuickStep(log)), EBuildPriority::Visible, outcomes.Collect());
	ASSERT_TRUE(test::WaitUntil([&] { return gate.bReached.load(); }, WorkerTimeout));
	pool.CancelAllAndDrain();   // the gated step sees the cancel and returns; the queued one never starts
	EXPECT_EQ(log.Count("Queued"), 0u);
	gate.Open();

	ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == 2u; }, TestTimeoutSeconds));
	for (const SBuildOutcome& outcome : outcomes.List)
	{
		EXPECT_EQ(outcome.Outcome, EBuildOutcome::Succeeded) << outcome.Key.CStr() << ": " << Join(outcome.Diagnostics).CStr();
	}
	EXPECT_EQ(log.Count("Running"), 2u);
	EXPECT_EQ(log.Count("Queued"), 1u);
	EXPECT_EQ(scheduler.GetStats().Reruns, 2u);
	EXPECT_TRUE(scheduler.IsIdle());
}

// A random graph of steps - each needs a few earlier ones, discovers them all on its first run, sleeps a
// random while - requested at random priorities: every request completes, every step the scheduler made
// ran at most twice (its first run and the restart once its needs are built; a step nothing holds any more
// goes, and one needed again later is made anew), and every output equals what a sequential build
// computes. With EBuildCommitOrder::Sequence the commits come in request order every time.
TEST(BuildSchedulerTest, ARandomGraphOfStepsAlwaysCompletesWithTheSequentialOutputs)
{
	constexpr uint32 StepCount = 120u;
	constexpr uint32 RequestCount = 40u;
	std::mt19937 random(20261025u);
	std::vector<std::vector<uint32>> needs(StepCount);
	for (uint32 i = 1u; i < StepCount; ++i)
	{
		const uint32 count = random() % 4u;
		for (uint32 n = 0u; n < count; ++n)
		{
			const uint32 need = random() % i;
			if (std::find(needs[i].begin(), needs[i].end(), need) == needs[i].end())
			{
				needs[i].push_back(need);
			}
		}
	}
	const auto keyOf = [] (uint32 I) { return FString::Format("S{}", I); };
	// The output of step i: its index, then each need's output's first byte and size, in order.
	std::vector<std::vector<uint8>> expected(StepCount);
	for (uint32 i = 0u; i < StepCount; ++i)
	{
		expected[i].push_back(static_cast<uint8>(i));
		for (const uint32 need : needs[i])
		{
			expected[i].push_back(expected[need][0]);
			expected[i].push_back(static_cast<uint8>(expected[need].size()));
		}
	}
	std::vector<uint32> requested;
	std::vector<EBuildPriority> priorities;
	for (uint32 r = 0u; r < RequestCount; ++r)
	{
		requested.push_back(random() % StepCount);
		priorities.push_back(static_cast<EBuildPriority>(random() % BuildPriorityCount));
	}

	for (uint32 round = 0u; round < 2u; ++round)
	{
		SCOPED_TRACE(round);
		CRunLog log;
		CFakeSteps steps;
		for (uint32 i = 0u; i < StepCount; ++i)
		{
			steps.Add(keyOf(i), [&log, &keyOf, i, need = needs[i], seed = 7u * i + round] (CBuildStepContext& Context)
			{
				log.Add(Context.GetKey());
				std::this_thread::sleep_for(std::chrono::microseconds((seed * 2654435761u) % 400u));
				std::vector<uint8> output = { static_cast<uint8>(i) };
				std::vector<FString> missing;
				for (const uint32 n : need)
				{
					const std::vector<uint8>* input = Context.FindInput(keyOf(n));
					if (input == nullptr)
					{
						missing.push_back(keyOf(n));
						continue;
					}
					output.push_back((*input)[0]);
					output.push_back(static_cast<uint8>(input->size()));
				}
				return missing.empty() ? SBuildStepResult::Success(output) : SBuildStepResult::Need(missing);
			});
		}
		jobs::CWorkerPool pool(Threads(4));
		SBuildSchedulerDesc desc;
		desc.CommitOrder = EBuildCommitOrder::Sequence;
		CBuildScheduler scheduler(pool, steps.MakeResolver(), desc);
		SOutcomes outcomes;
		std::vector<uint64> requests;
		for (uint32 r = 0u; r < RequestCount; ++r)
		{
			requests.push_back(scheduler.Request(keyOf(requested[r]), priorities[r], outcomes.Collect()));
		}
		ASSERT_TRUE(scheduler.PumpUntil([&] { return outcomes.List.size() == RequestCount; }, TestTimeoutSeconds));

		for (uint32 r = 0u; r < RequestCount; ++r)
		{
			const SBuildOutcome& outcome = outcomes.List[r];
			EXPECT_EQ(outcome.Request, requests[r]);   // request order
			ASSERT_EQ(outcome.Outcome, EBuildOutcome::Succeeded) << Join(outcome.Diagnostics).CStr();
			EXPECT_EQ(*outcome.Output, expected[requested[r]]) << outcome.Key.CStr();
		}
		const SBuildSchedulerStats stats = scheduler.GetStats();
		EXPECT_LE(stats.Runs, 2u * stats.StepsCreated);
		EXPECT_LE(stats.Restarts, stats.StepsCreated);
		EXPECT_EQ(stats.Runs, log.Get().size());
		EXPECT_EQ(stats.Failed, 0u);
		EXPECT_TRUE(scheduler.IsIdle());
	}
}
