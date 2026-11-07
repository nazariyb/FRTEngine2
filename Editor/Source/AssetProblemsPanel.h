#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "CoreTypes.h"
#include "Assets/Meta/ProblemList.h"
#include "Containers/String.h"


namespace frt
{
class CEngine;
}


namespace frt::editor
{
class CAssetsPanel;

/**
 * The Asset problems panel (design §12.8, M4's H4; edits [14], [18], [33]): every problem `validate` would list, over every
 * mount of the editor's registry, filterable by severity, code, type and folder and grouped by asset. Each entry has "Go to"
 * (the Assets panel lists its asset; the inspector target arrives with M6), "Open" (the file it names, and its line and
 * column to copy) and the fix-its M4 has (Assets/Meta/ProblemList.h): the lifecycle's actions, applied through
 * CAssetRepairs::ApplyFix and disabled where this process writes nothing, and the commands a person runs elsewhere, shown
 * and copied, never run.
 *
 * The list is RunValidation's, the very function the verb prints (ListAssetProblems), run on a worker over a view the
 * registry handed out: when the panel first draws, again when the revision moved while it shows (at most once a second),
 * and whenever something asks (RequestRefresh, or a fix waiting for its outcome). A fix applied in the live editor only
 * queues (AssetRepairs.h), so its outcome (fixed, refused with VCS002, or no change) is read from the lists that follow.
 * It polls the revision as the Assets panel does: no subscription.
 */
class CAssetProblemsPanel
{
public:
	static constexpr const char* Name = "Asset problems";
	static constexpr double MinRefreshSeconds = 1.0;
	static constexpr uint64 NeverRan = ~0ull;

	~CAssetProblemsPanel ();

	// Every frame, beside the other panels (a docked window left out for a frame gives up its dock node). Assets: "Go to".
	void Draw (CEngine& Engine, CAssetsPanel& Assets);
	// The list again at the next Draw, shown or not (scripted runs).
	void RequestRefresh () { bRefreshRequested = true; }
	// Blocks until the list in flight, if any, has arrived and is taken (the smoke test; before the registry goes).
	void WaitForRefresh ();
	// A new list now, on a worker as always, waited for (the smoke test).
	void RefreshNow (CEngine& Engine);
	// "Go to": the Assets panel lists entry Index's asset (its record, or what its folder holds) and comes to the front.
	void GoTo (CAssetsPanel& Assets, uint32 Index);
	// Before the registry shuts down: waits for the job, which reads the registry's mounts, rules and providers.
	void ShutDown ();

	[[nodiscard]] const std::vector<assets::SProblemEntry>& GetEntries () const { return Entries; }
	[[nodiscard]] const std::vector<assets::SProblemGroup>& GetGroups () const { return Groups; }
	// The revision of the view the list was made from (NeverRan before the first).
	[[nodiscard]] uint64 GetShownRevision () const { return RanAt; }
	[[nodiscard]] bool IsRefreshing () const { return Job != nullptr; }
	[[nodiscard]] const assets::SProblemFilter& GetFilter () const { return Filter; }
	void SetFilter (const assets::SProblemFilter& NewFilter);

	bool bOpen = true;

private:
	struct SJob
	{
		std::atomic<bool> bRan = false;        // the work ran to its end: Entries are complete
		std::atomic<bool> bFinished = false;   // its callable is gone (run, or dropped by a drain)
		std::vector<assets::SProblemEntry> Entries;
		uint64 Revision = 0u;
		double Milliseconds = 0.0;
	};

	// Sets bFinished when the work callable dies, run or dropped.
	struct SFinishGuard
	{
		std::shared_ptr<SJob> Job;
		SFinishGuard (std::shared_ptr<SJob> InJob) : Job(std::move(InJob)) {}
		SFinishGuard (SFinishGuard&& Other) noexcept = default;
		SFinishGuard& operator= (SFinishGuard&&) = delete;
		~SFinishGuard () { if (Job) { Job->bFinished.store(true, std::memory_order_release); } }
	};

	struct SRow
	{
		uint32 Group = 0u;
		int32 Entry = -1;    // -1: the group's header
	};

	void StartRefresh (CEngine& Engine);
	// Takes a finished job's list; false while it runs.
	bool TakeJob ();
	void Rebuild ();
	void ResolvePendingFixes ();
	void DrawFilters ();
	void DrawEntry (CEngine& Engine, CAssetsPanel& Assets, uint32 Index);
	void ApplyFix (CEngine& Engine, uint32 Index, const assets::SProblemFixIt& Fix);
	void Post (FString Text, bool bError);

	std::vector<assets::SProblemEntry> Entries;
	std::vector<std::optional<std::vector<assets::SProblemFixIt>>> FixIts;   // per entry, asked when first drawn
	std::vector<assets::SProblemGroup> Groups;
	std::vector<SRow> Rows;
	std::vector<FString> Types;
	std::set<FString> Collapsed;             // assets whose group is folded
	assets::SProblemFilter Filter;
	assets::SValidationCounts Counts;
	std::shared_ptr<SJob> Job;
	uint64 RanAt = NeverRan;
	double RefreshedAt = -1.0e9;             // ImGui time of the last start
	double Milliseconds = 0.0;
	bool bRefreshRequested = false;
	bool bRowsDirty = true;
	std::vector<assets::SPendingFix> Pending;
	FString Status;
	bool bStatusIsError = false;
};
}
