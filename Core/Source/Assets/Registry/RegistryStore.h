#pragma once

#include <atomic>
#include <memory>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTables.h"
#include "Assets/Registry/RegistryView.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"


/**
 * The registry's single writer (design §5.2, §5.9): it applies SRegistryDelta batches on its owner thread
 * and publishes an immutable CRegistryView after each one. CAssetRegistry (F3) wraps it with scanning,
 * Analyze and the watcher; AssetTool and tests use it directly.
 *
 * Threading. Commit, Live, AdoptImage, CompactGraph and ForEachChangeSince belong to the owner thread (the
 * thread of the first Commit or AdoptImage; checked in builds with assertions). Acquire may be called from
 * any thread at any time. Views are published through std::atomic<std::shared_ptr<const CRegistryView>>;
 * on MSVC its load is not lock-free - an internal spin bit is held while the reference count is raised - and
 * that is the accepted meaning of "Acquire never blocks": it never waits for a commit or for another
 * reader's work. (The deprecated std::atomic_load free functions are not used.) A commit copies only the
 * pages it touches; a reader holding an older view keeps it, unchanged, for as long as it likes.
 *
 * A delta is atomic: one revision, one change-ring entry per record touched, and readers see all of it or
 * none of it. RegistryDelta.h states the semantics; the parts the store decides itself:
 *	- A delta item that breaks the contract (a malformed path, an unknown mount, a tag key the type does
 *	  not declare, a duplicate SubId) is skipped and reported as a code-0 Registry problem - on the record's
 *	  path for record items, else on the item's path - and logged; the rest of the delta applies. A mount
 *	  upsert that contradicts a known mount is reported on that mount's root.
 *	- Removing a file or a record also clears its Analyze and Registry problems (its path and "path#..."),
 *	  since nothing would ever clear them otherwise; Scan and Rules problems stay with their producers.
 *	- A record upsert for a file the store does not know creates the FILES row (class Asset).
 *	- A file named in some record's Companions has class Companion whatever its record; the store never
 *	  deletes a record because of it (F3 sends none for companions).
 *	- The Registry problems of a record are its edges' REF006 / PATH001 plus the contract problems of its
 *	  last upsert, recomputed whenever a target it names changes resolution.
 *
 * No user callback runs inside Commit; the change ring is read afterwards (ForEachChangeSince).
 */
namespace frt::assets
{
// What happened to an asset record (its sub-entries, tags and edges are part of it).
enum class ERecordChange : uint8 { Added, Removed, Modified };

struct SRecordChange
{
	uint64 Revision = 0u;
	ERecordChange Kind = ERecordChange::Modified;
	SAssetId Id;
	FString MountPath;
};

struct SRegistryCommitStats
{
	double Milliseconds = 0.0;          // the last Commit, AdoptImage or compaction, publication included
	uint32 RecordsChanged = 0u;
	bool bCompactedGraph = false;
	bool bCompactedTables = false;
	bool bReplayedImage = false;        // AdoptImage replayed the image as a delta instead of taking it in bulk
};


class FRT_CORE_API CRegistryStore
{
public:
	static constexpr uint32 ChangeRingCapacity = 64u * 1024u;

	CRegistryStore ();
	~CRegistryStore ();
	CRegistryStore (const CRegistryStore&) = delete;
	CRegistryStore& operator= (const CRegistryStore&) = delete;

	// Applies the delta whole, bumps the revision by one and publishes a new view. Owner thread.
	void Commit (SRegistryDelta&& Delta);

	// The current state. Owner thread only; valid until the next Commit.
	[[nodiscard]] const CRegistryView& Live () const noexcept;
	// The last published view (Live as of the last commit). Any thread.
	[[nodiscard]] std::shared_ptr<const CRegistryView> Acquire () const noexcept;

	[[nodiscard]] uint64 GetRevision () const noexcept;

	// Every record change with a revision above Revision, oldest first. False when the bounded ring has
	// dropped some of them (64k entries): refresh everything instead. Owner thread.
	bool ForEachChangeSince (uint64 Revision, TFunctionRef<void (const SRecordChange&)> Visit) const;

	/**
	 * Commits an image (a snapshot's tables, RegistryTables.h) as one delta over the current state - on an
	 * empty store, a load - rebuilding every index and re-resolving every target, then compacts the graph,
	 * sets the revision to Revision and publishes. A fresh store takes the image in bulk (its tables filled
	 * straight from the vectors, the CSR built once); a store that holds anything already, or an image with
	 * an item the delta contract rejects, gets the image replayed as a delta, with the same result. Images of different mounts can be adopted one after the
	 * other (Engine.snapshot, then AssetRegistry.snapshot). The change ring reports nothing for an image:
	 * readers refresh everything (ForEachChangeSince returns false for older revisions). A type name the
	 * table does not know leaves its records frt.Unknown with Schemaless | StaleTags and no tags. Labels
	 * without RuleLabelBit (authored, M4) are not kept yet. Owner thread.
	 */
	void AdoptImage (SRegistryImage&& Image, uint64 Revision);

	/**
	 * Compaction (§5.2 "Mutation strategy"). Edits leave garbage behind: masked and superseded edges, dead
	 * SUBENTRY and type-table rows (a record whose sub-entries changed), replaced label, companion and
	 * MultiValue lists. Commit never compacts, so an interactive commit's main-thread time stays one edit's:
	 * the owner (F3) asks NeedsCompaction at idle and inside PumpUntil and calls Compact then.
	 *	- NeedsCompaction: the graph has at least 64k garbage edges and a quarter as many as live ones, or the
	 *	  record tables have at least 4096 dead rows and a quarter of their rows dead, or at least 16k garbage
	 *	  pool items and a quarter of the pools.
	 *	- Compact: compacts what NeedsCompaction names and publishes; nothing when nothing is due.
	 *	- CompactGraph: rebuilds the forward and reverse CSR from the live edges. No row number changes, so it
	 *	  publishes without a new revision.
	 *	- CompactTables: rebuilds SUBENTRY (each live record's sub-entries in declared order, records by row),
	 *	  every type table without dead rows, the label, companion, Multi and STRINGS pools and the AssetId64
	 *	  index. Sub-entry and type-table row numbers change, so it publishes a new revision (SRecordIds of an
	 *	  older view fail their check) with no change-ring entry: no record's content changed. FILES, FOLDERS
	 *	  and ASSETS rows of removed items are not reclaimed (they are skipped by every scan); a restart, which
	 *	  adopts a snapshot of the live rows, drops them.
	 * Readers holding older views keep the columns they have. All four belong to the owner thread.
	 */
	[[nodiscard]] bool NeedsCompaction () const noexcept;
	void Compact ();
	void CompactGraph ();
	void CompactTables ();

	[[nodiscard]] const SRegistryCommitStats& GetLastCommitStats () const noexcept { return LastStats; }

	// What the store keeps resident (§5.11's budget): the live view's tables (CRegistryView::GetMemoryStats)
	// plus the writer's own state - folder lists, target and companion maps, problem subjects - and the
	// change ring. Owner thread.
	[[nodiscard]] SRegistryMemoryStats GetMemoryStats () const;

private:
	struct SRingEntry;
	struct SWriterState;
	class CWriter;
	friend class CWriter;

	void CheckOwnerThread ();
	void Publish ();
	[[nodiscard]] bool GraphNeedsCompaction () const noexcept;
	[[nodiscard]] bool RecordTablesNeedCompaction () const noexcept;
	void RunCompaction (bool bGraph, bool bTables);
	void CompactGraphTables ();
	void CompactRecordTables ();

	CRegistryView LiveView;
	std::unique_ptr<SWriterState> Writer;
	std::atomic<std::shared_ptr<const CRegistryView>> Published;
	uint64 Epoch = 1u;
	uint64 OwnerThread = 0u;
	SRegistryCommitStats LastStats;
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ERecordChange,
	FRT_ENUM_ENTRY(frt::assets::ERecordChange, Added),
	FRT_ENUM_ENTRY(frt::assets::ERecordChange, Removed),
	FRT_ENUM_ENTRY(frt::assets::ERecordChange, Modified));
