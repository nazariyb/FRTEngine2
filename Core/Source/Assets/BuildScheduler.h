#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Functional/UniqueFunction.h"
#include "Jobs/WorkerPool.h"


/**
 * The build scheduler (design §7.8, §7.3, §7.9): decides which build step runs next, runs it on the
 * worker pool, and hands each request its outcome on the owner thread. A skeleton: a step is a callable
 * with a key, nothing more - no DDC lookups, BuildState, registry or diagnostics records yet (M2 on).
 *
 * Owner thread. The scheduler lives on the thread that owns the worker pool (the main thread in the
 * engine, the AssetTool loop) and is ticked by its pump: Tick() admits steps and commits outcomes, the
 * pool's PumpCompletions() delivers finished steps to it. Every wait is a pump loop (PumpUntil), never a
 * block on a step, so a step that needs another step is built by the same loop that waits (§7.3: nothing
 * ever waits on the worker pool alone).
 *
 * Requests carry (priority, sequence): EBuildPriority, then the order they were made in. Steps run in
 * that order, as many at once as MaxInFlight allows (the pool's thread count by default), and the steps
 * a step needs inherit the best (priority, sequence) of whatever waits for them. Escalate raises a
 * waiting request - a load that has become blocking - and everything it needs with it.
 *
 * Steps. A step's key names it: two requests for the same key share one run (the second joins the first,
 * §7.6 "build once"), and its run callable gets a CBuildStepContext. A step never spawns tasks and never
 * blocks on another step (§7.3, HH 166): when it finds that it needs other steps' outputs, it returns
 * Need(keys). The scheduler builds those first - asking the resolver for any key it has no step for yet -
 * and then runs the step again, whose context now offers their outputs (FindInput). A step restarts at
 * most MaxBuildStepRestarts times; needing inputs once more after that, or needing a step that (through
 * its own needs) waits for this one, fails it with a cycle error instead of waiting forever. A step whose
 * input failed or was cancelled fails without running again.
 *
 * Cancellation. Cancel(request) ends the request (its outcome is Cancelled) and releases its step, which
 * is cancelled only when nothing else needs it - no other request, no step waiting for its output (§7.9:
 * "cancelled only when no other request pins its output"). A cancelled step's queued run is dropped, a
 * running one sees IsCancelled() and its result is discarded, and the steps it needed are released in
 * turn.
 *
 * Epochs. The pool discards, unrun, the completion of a task submitted before its epoch moved on
 * (CWorkerPool::BumpEpoch, CancelAllAndDrain: a game-module reload, §6.7), so a step running across a new
 * epoch never reports back. Each Tick looks for such steps: it cancels the run (it may stop early; its
 * result goes unused) and, once the run has let go of the step's callable - returned, or been dropped
 * unstarted - queues the step again, so its requests still complete ("their work re-queued", §5.9) and the
 * callable still never runs twice at once.
 *
 * Commit order. Every request's OnComplete runs exactly once, in a Tick on the owner thread, with its
 * outcome - Succeeded, Failed or Cancelled - unless the scheduler is destroyed first. Interactively
 * (jobs::ECommitOrder::Completion) each Tick commits every request that has finished, in sequence order;
 * deterministic runs and reproducible tools (jobs::ECommitOrder::Sequence) commit strictly in request
 * order, a finished request waiting for every earlier one (§7.8 "Commit order", §8.10). The order is the
 * pool's (CWorkerPool::GetCommitOrder) unless SBuildSchedulerDesc names one, so a deterministic host sets
 * it once, on the pool, for builds and for the work it submits to the pool directly. OnComplete may
 * make, cancel or escalate requests.
 *
 * Memory and threads: std containers and the CRT heap only; every function on the owner thread. A step's
 * callable runs on a worker and must follow the worker rules (Jobs/WorkerPool.h: no TLSF, TArray,
 * TRefShared or world); its result is moved back to the owner thread, and the outputs it reads through
 * FindInput are immutable, shared with the scheduler. Before the code of a step's callable is unloaded
 * (a module barrier), CancelAll() and then the pool's CancelAllAndDrain().
 */
namespace frt::assets
{
// Ordered most urgent first: Escalated < Visible < Background < Cook.
enum class EBuildPriority : uint8
{
	Escalated,    // a blocking load or a scripted frame waits for it
	Visible,      // a load or a visible thumbnail
	Background,   // the warm pass
	Cook,         // the cooker, behind everything interactive
};

inline constexpr uint32 BuildPriorityCount = 4u;

// How many times a step may be run again after returning Need, before it fails as a cycle (§7.3).
inline constexpr uint32 MaxBuildStepRestarts = 3u;

// What a step's run returned.
enum class EBuildStepStatus : uint8
{
	Succeeded,
	Failed,
	NeedInputs,   // NeededSteps must be built first; then the step runs again
};

struct SBuildStepResult
{
	EBuildStepStatus Status = EBuildStepStatus::Succeeded;
	std::vector<uint8> Output;            // Succeeded: the step's output (opaque here; values and records later)
	std::vector<FString> Diagnostics;     // problems, as text lines until structured diagnostics are stored per step
	std::vector<FString> NeededSteps;     // NeedInputs: the keys of the steps to build first

	[[nodiscard]] FRT_CORE_API static SBuildStepResult Success (std::vector<uint8> Output = {});
	[[nodiscard]] FRT_CORE_API static SBuildStepResult Failure (FString Message);
	[[nodiscard]] FRT_CORE_API static SBuildStepResult Need (std::vector<FString> Steps);
};

/**
 * What a step's run gets, on its worker. Read-only, owned by the run: the outputs it offers are
 * immutable and stay alive while the run holds the context.
 */
class FRT_CORE_API CBuildStepContext
{
public:
	[[nodiscard]] FStringView GetKey () const noexcept { return Key; }
	// 0 on the first run; n after the n-th restart.
	[[nodiscard]] uint32 GetRestartCount () const noexcept { return RestartCount; }
	// The step was cancelled (CWorkerPool::IsCancelled): long work polls it and returns early.
	[[nodiscard]] static bool IsCancelled () noexcept;
	// The output of a step this one named in an earlier Need, which succeeded; nullptr for any other key.
	[[nodiscard]] const std::vector<uint8>* FindInput (FStringView StepKey) const noexcept;

private:
	friend class CBuildScheduler;

	FString Key;
	uint32 RestartCount = 0u;
	std::vector<std::pair<FString, std::shared_ptr<const std::vector<uint8>>>> Inputs;
};

using FBuildStepFunction = TUniqueFunction<SBuildStepResult (CBuildStepContext& Context)>;

// A step: its key and what runs it. The callable may run more than once (restarts), never twice at once.
struct SBuildStep
{
	FString Key;
	FBuildStepFunction Run;
};

// The step for a key that a running step needs, or nullopt when there is none (the needing step fails).
using FBuildStepResolver = TUniqueFunction<std::optional<SBuildStep> (FStringView Key)>;

enum class EBuildOutcome : uint8
{
	Succeeded,
	Failed,
	Cancelled,
};

// What a request's OnComplete gets.
struct SBuildOutcome
{
	uint64 Request = 0u;
	FString Key;
	EBuildOutcome Outcome = EBuildOutcome::Cancelled;
	std::shared_ptr<const std::vector<uint8>> Output;   // Succeeded: the step's output, shared with other requests for it
	std::vector<FString> Diagnostics;
	uint32 Restarts = 0u;                               // how often the step ran again after needing inputs
};

using FBuildCompletion = TUniqueFunction<void (const SBuildOutcome& Outcome)>;

// Where a step is (tests, panels).
enum class EBuildStepState : uint8
{
	Queued,             // waiting for a worker
	Running,
	WaitingForInputs,   // returned Need; restarts once its inputs have finished
	Succeeded,
	Failed,
	Cancelled,
};

struct SBuildSchedulerDesc
{
	// Steps on workers at once; 0: the pool's thread count.
	uint32 MaxInFlight = 0u;
	// Unset: the pool's, when the scheduler is made (see the class comment).
	std::optional<jobs::ECommitOrder> CommitOrder;
};

// Counts since construction, and the state now.
struct SBuildSchedulerStats
{
	uint64 Requests = 0u;
	uint64 StepsCreated = 0u;
	uint64 Runs = 0u;          // step runs started, restarts and reruns included
	uint64 Restarts = 0u;
	uint64 Reruns = 0u;        // runs queued again: the pool's epoch moved on while they ran, discarding their results
	uint64 Succeeded = 0u;     // steps
	uint64 Failed = 0u;        // steps
	uint64 Cancelled = 0u;     // steps
	uint64 Committed = 0u;     // requests whose OnComplete ran
	uint32 Queued = 0u;
	uint32 Running = 0u;
	uint32 Waiting = 0u;
	uint32 OpenRequests = 0u;
};

class FRT_CORE_API CBuildScheduler
{
public:
	// On the pool's owner thread. Resolver may be empty: then only steps given to Request exist.
	CBuildScheduler (jobs::CWorkerPool& Pool, FBuildStepResolver Resolver = nullptr, const SBuildSchedulerDesc& Desc = {});
	// Cancels everything; OnComplete callbacks still waiting are destroyed without running.
	~CBuildScheduler ();

	CBuildScheduler (const CBuildScheduler&) = delete;
	CBuildScheduler& operator= (const CBuildScheduler&) = delete;

	/**
	 * Requests a step's output and returns the request (its sequence number, from 1). The step joins
	 * an existing one with the same key (Step.Run is then dropped unused); otherwise it is new. The
	 * second form asks the resolver for the step (OnComplete gets Failed when there is none).
	 */
	uint64 Request (SBuildStep Step, EBuildPriority Priority, FBuildCompletion OnComplete = nullptr);
	uint64 Request (FStringView Key, EBuildPriority Priority, FBuildCompletion OnComplete = nullptr);

	// Ends the request with Cancelled (committed in the next Tick) and releases its step. False when
	// it already finished or does not exist.
	bool Cancel (uint64 Request);
	// Raises a request (and what it needs) to Priority, when that is more urgent. False when it
	// already finished or does not exist.
	bool Escalate (uint64 Request, EBuildPriority Priority);
	// Cancels every open request.
	void CancelAll ();

	// Admits queued steps to the pool and commits finished requests (see the class comment). Returns how
	// many requests it committed.
	uint32 Tick ();
	// The wait: pumps the pool's completions and ticks until Done() holds (true), TimeoutSeconds runs out
	// (negative: no limit) or nothing could change any more - no open request (false). It sleeps between
	// rounds until the pool has news; a Done() that something else turns true is seen within a second.
	bool PumpUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds = -1.0);

	void SetCommitOrder (jobs::ECommitOrder Order) noexcept { CommitOrder = Order; }
	[[nodiscard]] jobs::ECommitOrder GetCommitOrder () const noexcept { return CommitOrder; }

	// No open request and no step.
	[[nodiscard]] bool IsIdle () const noexcept { return Requests.empty() && Nodes.empty(); }
	[[nodiscard]] SBuildSchedulerStats GetStats () const;
	// The state of the step with Key, while anything holds it; nullopt otherwise.
	[[nodiscard]] std::optional<EBuildStepState> FindStepState (FStringView Key) const;

private:
	struct SStepDefinition
	{
		FBuildStepFunction Run;
	};

	// One run of a step, shared by its work and its completion (BuildScheduler.cpp).
	struct SRunState;

	struct SNode
	{
		uint64 Id = 0u;
		FString Key;
		std::shared_ptr<const SStepDefinition> Definition;
		EBuildStepState State = EBuildStepState::Queued;
		EBuildPriority Priority = EBuildPriority::Cook;
		uint64 Sequence = 0u;
		uint32 Restarts = 0u;
		uint32 Pins = 0u;                  // open requests and steps that need it
		std::vector<uint64> Needs;         // the steps it named in Need, in order, each once (pinned)
		uint32 PendingNeeds = 0u;          // of those, how many have not finished
		std::vector<uint64> Waiters;       // steps waiting for this one to finish
		uint64 CancelGroup = 0u;           // of its run on the pool, while Running
		uint64 RunEpoch = 0u;              // the pool's epoch its run was submitted in, while Running
		std::shared_ptr<SRunState> Run;    // its run, while Running
		bool bRunStale = false;            // the epoch moved on while it ran: cancelled, a rerun waits for it
		std::shared_ptr<const std::vector<uint8>> Output;
		std::vector<FString> Diagnostics;
	};

	struct SRequestRecord
	{
		FString Key;
		uint64 Node = 0u;                  // 0 once released (cancelled)
		EBuildPriority Priority = EBuildPriority::Cook;
		FBuildCompletion OnComplete;
		bool bCancelled = false;
		std::optional<SBuildOutcome> Failure;   // ended before it had a step (no step for its key)
	};

	// (priority, sequence, node id): the order steps start in.
	using SQueueEntry = std::tuple<uint8, uint64, uint64>;

	void CheckOwnerThread () const;
	SNode* FindNode (uint64 Id);
	const SNode* FindNode (uint64 Id) const;
	// The node for Step's key, created (queued) when there is none.
	uint64 GetOrCreateNode (SBuildStep&& Step, EBuildPriority Priority, uint64 Sequence);
	void Pin (SNode& Node);
	void Unpin (uint64 Id);
	// Raises Node and what it needs to (Priority, Sequence) where that is more urgent.
	void Raise (uint64 Id, EBuildPriority Priority, uint64 Sequence);
	void Enqueue (SNode& Node);
	void Dequeue (SNode& Node);
	void Admit ();
	// Queues again every running step whose run the pool will discard (an older epoch), once that run has
	// let go of the step's callable.
	void RequeueStaleRuns ();
	void OnRunFinished (uint64 Id, uint64 CancelGroup, SBuildStepResult&& Result);
	void HandleNeed (SNode& Node, std::vector<FString>&& Keys);
	// Final states: records it, wakes or fails what waits for it, releases what it needed.
	void Finish (SNode& Node, EBuildStepState State);
	void Fail (SNode& Node, FString Message);
	// Node Waiter waits for Need: whether Need (through what it waits for) waits for Waiter. Path gets
	// the keys from Need to Waiter when it does.
	bool WaitsFor (uint64 Need, uint64 Waiter, std::vector<uint64>& Path) const;
	void MarkRequestsReady (uint64 NodeId);
	SBuildOutcome MakeOutcome (uint64 RequestId, const SRequestRecord& Record) const;

	jobs::CWorkerPool& Pool;
	FBuildStepResolver Resolver;
	uint32 MaxInFlight = 1u;
	jobs::ECommitOrder CommitOrder = jobs::ECommitOrder::Completion;

	std::map<uint64, SNode> Nodes;               // by id: creation order
	std::map<FString, uint64> NodesByKey;
	std::map<uint64, SRequestRecord> Requests;   // open requests, by sequence
	std::set<uint64> ReadyToCommit;              // requests whose outcome is known
	std::set<SQueueEntry> Queue;
	uint32 InFlight = 0u;
	uint64 CheckedEpoch = 0u;                    // the pool's epoch when RequeueStaleRuns last looked
	uint32 StaleRuns = 0u;                       // stale runs it found still holding their step's callable
	uint64 NextNodeId = 1u;
	uint64 NextRequest = 1u;
	SBuildSchedulerStats Totals;
};
}
