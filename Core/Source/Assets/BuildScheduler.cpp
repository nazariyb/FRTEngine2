#include "Assets/BuildScheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "Jobs/WorkerPool.h"


namespace frt::assets
{
namespace
{
// The pool has three queues; the scheduler orders its four priorities itself and hands each run over
// with the nearest one, so that a build step and other work on the pool share it sensibly.
jobs::EWorkPriority ToWorkPriority (EBuildPriority Priority) noexcept
{
	switch (Priority)
	{
		case EBuildPriority::Escalated:
			return jobs::EWorkPriority::High;
		case EBuildPriority::Visible:
			return jobs::EWorkPriority::Normal;
		case EBuildPriority::Background:
		case EBuildPriority::Cook:
			break;
	}
	return jobs::EWorkPriority::Low;
}

bool IsFinal (EBuildStepState State) noexcept
{
	return State == EBuildStepState::Succeeded || State == EBuildStepState::Failed || State == EBuildStepState::Cancelled;
}

// (Priority, Sequence) is more urgent than (OtherPriority, OtherSequence).
bool IsMoreUrgent (EBuildPriority Priority, uint64 Sequence, EBuildPriority OtherPriority, uint64 OtherSequence) noexcept
{
	return Priority != OtherPriority ? Priority < OtherPriority : Sequence < OtherSequence;
}

// Raises Flag when destroyed (not when moved from). A run's work callable holds one: the pool destroys
// that callable once it has returned, or unrun when it drops the task, so the flag says the run no longer
// uses the step's callable, whichever way the pool ended it - also when its completion is discarded.
class CReleaseFlag
{
public:
	explicit CReleaseFlag (std::shared_ptr<std::atomic<bool>> InFlag) noexcept : Flag(std::move(InFlag)) {}
	CReleaseFlag (CReleaseFlag&&) noexcept = default;
	CReleaseFlag& operator= (CReleaseFlag&&) = delete;
	CReleaseFlag (const CReleaseFlag&) = delete;
	CReleaseFlag& operator= (const CReleaseFlag&) = delete;

	~CReleaseFlag ()
	{
		if (Flag)
		{
			Flag->store(true, std::memory_order_release);
		}
	}

private:
	std::shared_ptr<std::atomic<bool>> Flag;
};
}


// A run's result, filled on the worker and read by the completion on the owner thread, and whether the
// run has let go of the step's callable (CReleaseFlag).
struct CBuildScheduler::SRunState
{
	SBuildStepResult Result;
	std::shared_ptr<std::atomic<bool>> ReleasedFlag = std::make_shared<std::atomic<bool>>(false);
};


SBuildStepResult SBuildStepResult::Success (std::vector<uint8> Output)
{
	SBuildStepResult result;
	result.Status = EBuildStepStatus::Succeeded;
	result.Output = std::move(Output);
	return result;
}

SBuildStepResult SBuildStepResult::Failure (FString Message)
{
	SBuildStepResult result;
	result.Status = EBuildStepStatus::Failed;
	result.Diagnostics.push_back(std::move(Message));
	return result;
}

SBuildStepResult SBuildStepResult::Need (std::vector<FString> Steps)
{
	SBuildStepResult result;
	result.Status = EBuildStepStatus::NeedInputs;
	result.NeededSteps = std::move(Steps);
	return result;
}


bool CBuildStepContext::IsCancelled () noexcept
{
	return jobs::CWorkerPool::IsCancelled();
}

const std::vector<uint8>* CBuildStepContext::FindInput (FStringView StepKey) const noexcept
{
	for (const auto& [key, output] : Inputs)
	{
		if (key == StepKey)
		{
			return output.get();
		}
	}
	return nullptr;
}


CBuildScheduler::CBuildScheduler (jobs::CWorkerPool& InPool, FBuildStepResolver InResolver, const SBuildSchedulerDesc& Desc)
	: Pool(InPool)
	, Resolver(std::move(InResolver))
	, MaxInFlight(Desc.MaxInFlight != 0u ? Desc.MaxInFlight : std::max(InPool.GetThreadCount(), 1u))
	, CommitOrder(Desc.CommitOrder)
{
	CheckOwnerThread();
}

CBuildScheduler::~CBuildScheduler ()
{
	CheckOwnerThread();
	CancelAll();
	// What was cancelled would commit in the next Tick: there is none. The pool discards the cancelled
	// runs' completions (they hold `this`) unrun; a run still on a worker holds only its own context.
	Requests.clear();
	ReadyToCommit.clear();
}

void CBuildScheduler::CheckOwnerThread () const
{
	frt_assert(Pool.IsOwnerThread())
}

CBuildScheduler::SNode* CBuildScheduler::FindNode (uint64 Id)
{
	const auto it = Nodes.find(Id);
	return it != Nodes.end() ? &it->second : nullptr;
}

const CBuildScheduler::SNode* CBuildScheduler::FindNode (uint64 Id) const
{
	const auto it = Nodes.find(Id);
	return it != Nodes.end() ? &it->second : nullptr;
}


// ---------------- Requests ----------------

uint64 CBuildScheduler::Request (SBuildStep Step, EBuildPriority Priority, FBuildCompletion OnComplete)
{
	CheckOwnerThread();
	const uint64 sequence = NextRequest++;
	++Totals.Requests;
	const uint64 id = GetOrCreateNode(std::move(Step), Priority, sequence);
	SNode& node = Nodes.at(id);
	Pin(node);
	Raise(id, Priority, sequence);

	SRequestRecord& record = Requests[sequence];
	record.Key = node.Key;
	record.Node = id;
	record.Priority = Priority;
	record.OnComplete = std::move(OnComplete);
	if (IsFinal(node.State))
	{
		ReadyToCommit.insert(sequence);
	}
	Admit();
	return sequence;
}

uint64 CBuildScheduler::Request (FStringView Key, EBuildPriority Priority, FBuildCompletion OnComplete)
{
	CheckOwnerThread();
	if (const auto existing = NodesByKey.find(FString(Key)); existing != NodesByKey.end())
	{
		return Request(SBuildStep{ FString(Key), nullptr }, Priority, std::move(OnComplete));
	}
	std::optional<SBuildStep> step = Resolver ? Resolver(Key) : std::nullopt;
	if (step && step->Run)
	{
		step->Key = Key;
		return Request(std::move(*step), Priority, std::move(OnComplete));
	}

	// No step for the key: the request ends Failed at the next commit, with no step behind it.
	const uint64 sequence = NextRequest++;
	++Totals.Requests;
	SRequestRecord& record = Requests[sequence];
	record.Key = Key;
	record.Priority = Priority;
	record.OnComplete = std::move(OnComplete);
	SBuildOutcome& failure = record.Failure.emplace();
	failure.Key = Key;
	failure.Outcome = EBuildOutcome::Failed;
	failure.Diagnostics.push_back(FString::Format("no build step for '{}'", Key));
	ReadyToCommit.insert(sequence);
	return sequence;
}

bool CBuildScheduler::Cancel (uint64 RequestId)
{
	CheckOwnerThread();
	const auto it = Requests.find(RequestId);
	if (it == Requests.end() || it->second.bCancelled || ReadyToCommit.contains(RequestId))
	{
		return false;
	}
	it->second.bCancelled = true;
	ReadyToCommit.insert(RequestId);
	const uint64 node = std::exchange(it->second.Node, 0u);
	if (node != 0u)
	{
		Unpin(node);
	}
	Admit();
	return true;
}

bool CBuildScheduler::Escalate (uint64 RequestId, EBuildPriority Priority)
{
	CheckOwnerThread();
	const auto it = Requests.find(RequestId);
	if (it == Requests.end() || it->second.bCancelled || ReadyToCommit.contains(RequestId) || it->second.Node == 0u)
	{
		return false;
	}
	if (Priority < it->second.Priority)
	{
		it->second.Priority = Priority;
	}
	Raise(it->second.Node, it->second.Priority, RequestId);
	return true;
}

void CBuildScheduler::CancelAll ()
{
	CheckOwnerThread();
	std::vector<uint64> open;
	for (const auto& [id, record] : Requests)
	{
		open.push_back(id);
	}
	for (const uint64 id : open)
	{
		Cancel(id);
	}
}


// ---------------- Steps ----------------

uint64 CBuildScheduler::GetOrCreateNode (SBuildStep&& Step, EBuildPriority Priority, uint64 Sequence)
{
	if (const auto existing = NodesByKey.find(Step.Key); existing != NodesByKey.end())
	{
		return existing->second;
	}
	const uint64 id = NextNodeId++;
	++Totals.StepsCreated;
	SNode& node = Nodes[id];
	node.Id = id;
	node.Key = Step.Key;
	node.Priority = Priority;
	node.Sequence = Sequence;
	if (Step.Run)
	{
		auto definition = std::make_shared<SStepDefinition>();
		definition->Run = std::move(Step.Run);
		node.Definition = std::move(definition);
		Enqueue(node);
	}
	else
	{
		node.State = EBuildStepState::Failed;
		node.Diagnostics.push_back(FString::Format("no build step for '{}'", node.Key));
		++Totals.Failed;
	}
	NodesByKey.emplace(node.Key, id);
	return id;
}

void CBuildScheduler::Pin (SNode& Node)
{
	++Node.Pins;
}

void CBuildScheduler::Unpin (uint64 Id)
{
	SNode* node = FindNode(Id);
	if (node == nullptr || node->Pins == 0u || --node->Pins > 0u)
	{
		return;
	}
	// Nothing needs it any more: a step still to finish is cancelled (§7.9), and it goes.
	if (!IsFinal(node->State))
	{
		if (node->State == EBuildStepState::Queued)
		{
			Dequeue(*node);
		}
		else if (node->State == EBuildStepState::Running)
		{
			Pool.Cancel(node->CancelGroup);
			--InFlight;
		}
		node->State = EBuildStepState::Cancelled;
		++Totals.Cancelled;
	}
	const std::vector<uint64> needs = std::move(node->Needs);
	NodesByKey.erase(node->Key);
	Nodes.erase(Id);
	for (const uint64 need : needs)
	{
		// It no longer waits for them.
		if (SNode* needed = FindNode(need))
		{
			std::erase(needed->Waiters, Id);
		}
		Unpin(need);
	}
}

void CBuildScheduler::Raise (uint64 Id, EBuildPriority Priority, uint64 Sequence)
{
	SNode* node = FindNode(Id);
	if (node == nullptr || IsFinal(node->State) || !IsMoreUrgent(Priority, Sequence, node->Priority, node->Sequence))
	{
		return;
	}
	const bool bQueued = node->State == EBuildStepState::Queued;
	if (bQueued)
	{
		Dequeue(*node);
	}
	// The urgency of whatever waits for it most urgently: a better priority brings its sequence along.
	node->Priority = Priority;
	node->Sequence = Sequence;
	if (bQueued)
	{
		Enqueue(*node);
	}
	// Each step's urgency only ever improves, and cycles are refused: this ends.
	const std::vector<uint64> needs = node->Needs;
	for (const uint64 need : needs)
	{
		Raise(need, Priority, Sequence);
	}
}

void CBuildScheduler::Enqueue (SNode& Node)
{
	Node.State = EBuildStepState::Queued;
	Queue.emplace(static_cast<uint8>(Node.Priority), Node.Sequence, Node.Id);
}

void CBuildScheduler::Dequeue (SNode& Node)
{
	Queue.erase(SQueueEntry(static_cast<uint8>(Node.Priority), Node.Sequence, Node.Id));
}

void CBuildScheduler::Admit ()
{
	while (InFlight < MaxInFlight && !Queue.empty())
	{
		const uint64 id = std::get<2>(*Queue.begin());
		Queue.erase(Queue.begin());
		SNode& node = Nodes.at(id);
		node.State = EBuildStepState::Running;
		node.CancelGroup = Pool.CreateCancelGroup();
		++InFlight;
		++Totals.Runs;

		CBuildStepContext context;
		context.Key = node.Key;
		context.RestartCount = node.Restarts;
		for (const uint64 need : node.Needs)
		{
			const SNode* needed = FindNode(need);
			if (needed != nullptr && needed->State == EBuildStepState::Succeeded)
			{
				context.Inputs.emplace_back(needed->Key, needed->Output);
			}
		}

		// The work holds only what it reads (its definition and context), the state it writes and its
		// release flag: never the scheduler, which a cancelled run may outlive.
		auto run = std::make_shared<SRunState>();
		node.Run = run;
		node.RunEpoch = Pool.GetEpoch();   // the epoch Submit tags the task with: both on this thread
		node.bRunStale = false;
		CReleaseFlag release(run->ReleasedFlag);
		Pool.Submit(ToWorkPriority(node.Priority), node.CancelGroup,
			[definition = node.Definition, context = std::move(context), run, release = std::move(release)] () mutable
			{
				run->Result = definition->Run(context);
			},
			[this, id, group = node.CancelGroup, run] ()
			{
				OnRunFinished(id, group, std::move(run->Result));
			});
	}
}

void CBuildScheduler::RequeueStaleRuns ()
{
	// Epochs only grow, and only on this thread: a run submitted in an older one has a completion the
	// pool will discard (or has discarded), so OnRunFinished never comes for it.
	const uint64 epoch = Pool.GetEpoch();
	if (epoch == CheckedEpoch && StaleRuns == 0u)
	{
		return;
	}
	CheckedEpoch = epoch;
	StaleRuns = 0u;
	for (auto& [id, node] : Nodes)
	{
		if (node.State != EBuildStepState::Running || node.RunEpoch >= epoch)
		{
			continue;
		}
		if (!node.bRunStale)
		{
			// Its result goes unused: let it stop early, or drop it if it has not started.
			node.bRunStale = true;
			Pool.Cancel(node.CancelGroup);
		}
		if (!node.Run->ReleasedFlag->load(std::memory_order_acquire))
		{
			++StaleRuns;   // still on a worker: the rerun waits, so the step's callable never runs twice at once
			continue;
		}
		--InFlight;
		node.CancelGroup = 0u;
		node.Run.reset();
		node.bRunStale = false;
		++Totals.Reruns;
		Enqueue(node);
	}
}

void CBuildScheduler::OnRunFinished (uint64 Id, uint64 CancelGroup, SBuildStepResult&& Result)
{
	SNode* node = FindNode(Id);
	if (node == nullptr || node->State != EBuildStepState::Running || node->CancelGroup != CancelGroup)
	{
		return;   // cancelled meanwhile (the pool discards those; this is the belt to its braces)
	}
	--InFlight;
	node->CancelGroup = 0u;
	node->Run.reset();
	switch (Result.Status)
	{
		case EBuildStepStatus::Succeeded:
			node->Output = std::make_shared<const std::vector<uint8>>(std::move(Result.Output));
			node->Diagnostics = std::move(Result.Diagnostics);
			Finish(*node, EBuildStepState::Succeeded);
			break;
		case EBuildStepStatus::Failed:
			node->Diagnostics = std::move(Result.Diagnostics);
			if (node->Diagnostics.empty())
			{
				node->Diagnostics.push_back("the step failed");
			}
			Finish(*node, EBuildStepState::Failed);
			break;
		case EBuildStepStatus::NeedInputs:
			node->Diagnostics = std::move(Result.Diagnostics);
			HandleNeed(*node, std::move(Result.NeededSteps));
			break;
	}
	Admit();
}

void CBuildScheduler::HandleNeed (SNode& Node, std::vector<FString>&& Keys)
{
	if (Keys.empty())
	{
		Fail(Node, "the step needs inputs but named none");
		return;
	}
	if (Node.Restarts >= MaxBuildStepRestarts)
	{
		FString names;
		for (const FString& key : Keys)
		{
			names.AppendFormat("{}'{}'", names.IsEmpty() ? "" : ", ", key);
		}
		Fail(Node, FString::Format("cycle: the step still needs inputs ({}) after {} restarts", names, MaxBuildStepRestarts));
		return;
	}

	const uint64 id = Node.Id;
	for (const FString& key : Keys)
	{
		// The step may have failed (and been released) through an earlier key of this list.
		SNode* waiter = FindNode(id);
		if (waiter == nullptr || IsFinal(waiter->State))
		{
			return;
		}
		uint64 need = 0u;
		if (const auto existing = NodesByKey.find(key); existing != NodesByKey.end())
		{
			need = existing->second;
		}
		else
		{
			std::optional<SBuildStep> step = Resolver ? Resolver(key) : std::nullopt;
			if (!step || !step->Run)
			{
				Fail(*waiter, FString::Format("needs '{}', and there is no build step for it", key));
				return;
			}
			step->Key = key;
			need = GetOrCreateNode(std::move(*step), waiter->Priority, waiter->Sequence);
			waiter = FindNode(id);   // the map may have rebalanced; the node did not move, but be plain about it
		}
		if (std::find(waiter->Needs.begin(), waiter->Needs.end(), need) != waiter->Needs.end())
		{
			continue;   // named before: pinned already, and finished or waited for
		}

		std::vector<uint64> path;
		if (need == id || WaitsFor(need, id, path))
		{
			FString chain = waiter->Key;
			chain.AppendFormat(" -> {}", Nodes.at(need).Key);
			for (const uint64 step : path)
			{
				chain.AppendFormat(" -> {}", Nodes.at(step).Key);
			}
			if (need != id)
			{
				chain.AppendFormat(" -> {}", waiter->Key);
			}
			Fail(*waiter, "cycle: " + chain);
			return;
		}

		SNode& needed = Nodes.at(need);
		waiter->Needs.push_back(need);
		Pin(needed);
		if (needed.State == EBuildStepState::Failed || needed.State == EBuildStepState::Cancelled)
		{
			Fail(*waiter, FString::Format("its input '{}' {}", needed.Key, needed.State == EBuildStepState::Failed ? "failed" : "was cancelled"));
			return;
		}
		if (needed.State != EBuildStepState::Succeeded)
		{
			needed.Waiters.push_back(id);
			++waiter->PendingNeeds;
			Raise(need, waiter->Priority, waiter->Sequence);
		}
	}

	SNode& waiter = Nodes.at(id);
	++waiter.Restarts;
	++Totals.Restarts;
	if (waiter.PendingNeeds == 0u)
	{
		Enqueue(waiter);
	}
	else
	{
		waiter.State = EBuildStepState::WaitingForInputs;
	}
}

bool CBuildScheduler::WaitsFor (uint64 Need, uint64 Waiter, std::vector<uint64>& Path) const
{
	// Depth first over the steps Need waits for (unfinished needs of waiting steps): small graphs, and
	// each step at most once.
	std::vector<std::pair<uint64, size_t>> stack = { { Need, 0u } };
	std::set<uint64> seen = { Need };
	while (!stack.empty())
	{
		auto& [current, next] = stack.back();
		const SNode* node = FindNode(current);
		if (node == nullptr || next >= node->Needs.size())
		{
			stack.pop_back();
			continue;
		}
		const uint64 candidate = node->Needs[next++];
		const SNode* candidateNode = FindNode(candidate);
		if (candidateNode == nullptr || IsFinal(candidateNode->State))
		{
			continue;
		}
		if (candidate == Waiter)
		{
			Path.clear();
			for (size_t i = 1u; i < stack.size(); ++i)
			{
				Path.push_back(stack[i].first);
			}
			return true;
		}
		if (seen.insert(candidate).second)
		{
			stack.emplace_back(candidate, 0u);
		}
	}
	return false;
}

void CBuildScheduler::Fail (SNode& Node, FString Message)
{
	Node.Diagnostics.push_back(std::move(Message));
	Finish(Node, EBuildStepState::Failed);
}

void CBuildScheduler::Finish (SNode& Node, EBuildStepState State)
{
	if (Node.State == EBuildStepState::Queued)
	{
		Dequeue(Node);
	}
	Node.State = State;
	++(State == EBuildStepState::Succeeded ? Totals.Succeeded : (State == EBuildStepState::Failed ? Totals.Failed : Totals.Cancelled));
	const uint64 id = Node.Id;
	const FString key = Node.Key;
	MarkRequestsReady(id);

	// Wake what waited for it: restart a step whose last input this was, fail one whose input failed.
	const std::vector<uint64> waiters = std::move(Node.Waiters);
	for (const uint64 waiterId : waiters)
	{
		SNode* waiter = FindNode(waiterId);
		if (waiter == nullptr || waiter->State != EBuildStepState::WaitingForInputs)
		{
			continue;
		}
		--waiter->PendingNeeds;
		if (State != EBuildStepState::Succeeded)
		{
			Fail(*waiter, FString::Format("its input '{}' {}", key, State == EBuildStepState::Failed ? "failed" : "was cancelled"));
		}
		else if (waiter->PendingNeeds == 0u)
		{
			Enqueue(*waiter);
		}
	}

	// A finished step needs its inputs no more; they stay while anything else holds them.
	if (SNode* node = FindNode(id))
	{
		const std::vector<uint64> needs = std::move(node->Needs);
		node->PendingNeeds = 0u;
		for (const uint64 need : needs)
		{
			if (SNode* needed = FindNode(need))
			{
				std::erase(needed->Waiters, id);
			}
			Unpin(need);
		}
	}
}

void CBuildScheduler::MarkRequestsReady (uint64 NodeId)
{
	for (const auto& [id, record] : Requests)
	{
		if (record.Node == NodeId)
		{
			ReadyToCommit.insert(id);
		}
	}
}


// ---------------- Ticks ----------------

SBuildOutcome CBuildScheduler::MakeOutcome (uint64 RequestId, const SRequestRecord& Record) const
{
	if (Record.Failure)
	{
		SBuildOutcome outcome = *Record.Failure;
		outcome.Request = RequestId;
		return outcome;
	}
	SBuildOutcome outcome;
	outcome.Request = RequestId;
	outcome.Key = Record.Key;
	const SNode* node = FindNode(Record.Node);
	if (node != nullptr)
	{
		outcome.Restarts = node->Restarts;
		outcome.Diagnostics = node->Diagnostics;
	}
	if (Record.bCancelled || node == nullptr)
	{
		outcome.Outcome = EBuildOutcome::Cancelled;
		return outcome;
	}
	outcome.Outcome = node->State == EBuildStepState::Succeeded ? EBuildOutcome::Succeeded
		: (node->State == EBuildStepState::Failed ? EBuildOutcome::Failed : EBuildOutcome::Cancelled);
	if (outcome.Outcome == EBuildOutcome::Succeeded)
	{
		outcome.Output = node->Output;
	}
	return outcome;
}

uint32 CBuildScheduler::Tick ()
{
	CheckOwnerThread();
	RequeueStaleRuns();
	Admit();

	// Which requests commit now: every ready one (interactive), or the ready ones at the front of the
	// request order (deterministic). Their outcomes are taken and their state released first; the
	// callbacks run last, so a callback that makes or cancels requests sees a consistent scheduler.
	std::vector<uint64> committing;
	if (CommitOrder == EBuildCommitOrder::Completion)
	{
		committing.assign(ReadyToCommit.begin(), ReadyToCommit.end());
	}
	else
	{
		for (const auto& [id, record] : Requests)
		{
			if (!ReadyToCommit.contains(id))
			{
				break;
			}
			committing.push_back(id);
		}
	}

	std::vector<std::pair<FBuildCompletion, SBuildOutcome>> callbacks;
	callbacks.reserve(committing.size());
	for (const uint64 id : committing)
	{
		auto it = Requests.find(id);
		ReadyToCommit.erase(id);
		if (it == Requests.end())
		{
			continue;
		}
		callbacks.emplace_back(std::move(it->second.OnComplete), MakeOutcome(id, it->second));
		const uint64 node = it->second.Node;
		Requests.erase(it);
		if (node != 0u)
		{
			Unpin(node);
		}
		++Totals.Committed;
	}
	Admit();

	for (auto& [callback, outcome] : callbacks)
	{
		if (callback)
		{
			callback(outcome);
		}
	}
	return static_cast<uint32>(callbacks.size());
}

bool CBuildScheduler::PumpUntil (TFunctionRef<bool ()> Done, double TimeoutSeconds)
{
	CheckOwnerThread();
	using clock = std::chrono::steady_clock;
	const clock::time_point deadline = TimeoutSeconds >= 0.0
		? clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(TimeoutSeconds))
		: clock::time_point::max();
	for (;;)
	{
		Pool.PumpCompletions();
		Tick();
		if (Done())
		{
			return true;
		}
		// Nothing open: nothing this loop does can change Done() any more.
		if (Requests.empty())
		{
			return false;
		}
		// Something to commit already (a callback may have made it so): go round again at once.
		if (!ReadyToCommit.empty() && CommitOrder == EBuildCommitOrder::Completion)
		{
			continue;
		}
		const clock::time_point now = clock::now();
		if (now >= deadline)
		{
			return false;
		}
		const double remaining = deadline == clock::time_point::max() ? -1.0 : std::chrono::duration<double>(deadline - now).count();
		Pool.WaitForCompletions(remaining >= 0.0 ? std::min(remaining, 1.0) : 1.0);
	}
}

SBuildSchedulerStats CBuildScheduler::GetStats () const
{
	SBuildSchedulerStats stats = Totals;
	for (const auto& [id, node] : Nodes)
	{
		stats.Queued += node.State == EBuildStepState::Queued ? 1u : 0u;
		stats.Running += node.State == EBuildStepState::Running ? 1u : 0u;
		stats.Waiting += node.State == EBuildStepState::WaitingForInputs ? 1u : 0u;
	}
	stats.OpenRequests = static_cast<uint32>(Requests.size());
	return stats;
}

std::optional<EBuildStepState> CBuildScheduler::FindStepState (FStringView Key) const
{
	const auto it = NodesByKey.find(FString(Key));
	if (it == NodesByKey.end())
	{
		return std::nullopt;
	}
	return Nodes.at(it->second).State;
}
}
