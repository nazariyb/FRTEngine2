#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Platform/FileSystem.h"


/**
 * The registry journal (design §5.3): every commit's delta since the last full snapshot, appended beside that
 * snapshot (GetJournalPath), so a crash or a kill loses nothing a full write would have kept and a full write
 * is needed only now and then (clean exit, a large batch, 60 s idle). Load = snapshot + journal replay
 * (Snapshot.h ReadSnapshots).
 *
 * File: a 40-byte header - Magic "FRTAJRN1", FormatVersion (the delta encoding included), 4 reserved bytes,
 * ProjectId, JournalBase (the revision of the snapshot the journal continues) - then record frames
 * (Assets/RecordFrames.h, the build state log's frames), appended with platform::CAppendFile. A commit is one
 * or more frames of kind 1 (a delta larger than a frame holds is split), all written in one Append, so a
 * crash tears at most the last commit; a reader stops at the first frame that fails its check or the first
 * incomplete commit and drops the rest (the torn tail). Deltas build on each other, so nothing after a bad
 * frame is replayed. Types are stored by their stable names (a per-commit table), never by TypeId, so a
 * build with another type table replays them right; a name this build does not know becomes frt.Unknown with
 * Schemaless | StaleTags, the snapshot's rule.
 *
 * A journal belongs to one snapshot file and keeps only that file's mounts: a commit's delta is split by
 * mount (FilterDeltaByMount) and the parts for other mounts are not written; a commit with no part here
 * writes nothing.
 *
 * Revisions. Each commit frame records the store revision the delta was committed at, in the numbering of
 * the process that appended it. Replay follows file order and never compares revisions across processes;
 * Restart compares them only against the frames this process appended (a view of this process's store).
 *
 * The protocol with a full write (F3):
 *	1. Start-up: LoadSnapshots, then Open(path, ProjectId, the loaded snapshot's JournalBase - nullopt when
 *	   none was loaded - and the file's mount names). A journal that continues that snapshot is kept (a torn
 *	   tail is cut off first) and appended to; any other journal is started anew.
 *	2. Every commit: Append(delta, the revision the commit will get) before CRegistryStore::Commit (the commit
 *	   goes ahead whatever Append returns: the journal only saves work).
 *	3. A full write, with the journal's file work on the worker too (§5.3: writing never stalls the owner):
 *	   on the owner thread, BeginRestart(view revision) where the view is acquired; on the worker,
 *	   WriteSnapshot(view, ...) and, when it succeeded, CJournalRestart::Prepare - the new journal (header and
 *	   the commits after the view that were appended so far) written beside the old one and renamed over it,
 *	   and opened for appending; on the owner thread, FinishRestart in the job's completion, which swaps the
 *	   handle and appends the commits made since BeginRestart from memory (they went into the old file, which
 *	   the rename left to the old handle) - one append. The journal then keeps this process's frames
 *	   committed after that view and drops everything older, which the snapshot holds. A Prepare that failed
 *	   makes FinishRestart do the whole Restart on the owner thread. Restart(view revision) is the same in one
 *	   call on the owner thread. One restart at a time. A crash before the rename leaves a journal whose
 *	   JournalBase no longer matches the snapshot: it is ignored at load, and the scan finds the few changes
 *	   it held (the snapshot's file stats are older than the disk's); a crash after it, a journal of the new
 *	   snapshot without the commits made since BeginRestart (the scan finds those too).
 *	4. A failed Append (a full disk, a write cut short) breaks the journal: part of the commit may be in the
 *	   file, and nothing appended after it would be read, so the file is closed and every later Append returns
 *	   false (IsBroken) until Restart or Open. The commits are not lost: Append keeps every commit's frames in
 *	   memory, written or not, and Restart writes the new journal from them. On the first false Append F3 logs
 *	   once and schedules a full write at once (step 3); a Restart that fails leaves the journal broken, and
 *	   the next full write tries again. A crash before that costs a rescan of what changed since the last
 *	   good commit, the same as a lost journal.
 *
 * Memory: the frames this process appended since the last Restart (or Open) stay in memory, so a restart reads
 * nothing back - Restart writes the header and the kept frames in place, unflushed (see StartNew), and
 * CJournalRestart does that on a worker. Full writes come at least every batch of 1000 records or 60 s idle,
 * which bounds them.
 *
 * Owner thread only (one CRegistryJournal per file, used where the commits happen), except CJournalRestart's
 * Prepare, which runs on a worker and touches only its own object and the file system. No engine allocator.
 */
namespace frt::assets
{
inline constexpr uint8 JournalMagic[8] = { 'F', 'R', 'T', 'A', 'J', 'R', 'N', '1' };
inline constexpr uint32 JournalFormatVersion = 2u;   // 2: types by stable name
inline constexpr uint32 JournalHeaderBytes = 40u;

// The delta encoding (the journal's commit payload): every member of SRegistryDelta, Sequence included.
FRT_CORE_API void EncodeRegistryDelta (const SRegistryDelta& Delta, std::vector<uint8>& Out);
// nullopt when the bytes are not exactly one encoded delta.
[[nodiscard]] FRT_CORE_API std::optional<SRegistryDelta> DecodeRegistryDelta (std::span<const uint8> Bytes);

/**
 * The part of a delta whose paths lie in the mounts Keep accepts (by mount name): removes, upserts, problem
 * clears and problems by their path's first segment, rule files and mount upserts by their mount name.
 * Sequence is kept. A delta for a set of mounts is what a journal of a snapshot of those mounts holds.
 */
[[nodiscard]] FRT_CORE_API SRegistryDelta FilterDeltaByMount (const SRegistryDelta& Delta, TFunctionRef<bool (FStringView MountName)> Keep);

// A journal file as read back.
struct SJournalContents
{
	bool bFound = false;              // a file exists
	bool bHeaderValid = false;        // magic and format version match
	SGuid ProjectId;
	uint64 JournalBase = 0u;
	std::vector<SRegistryDelta> Deltas;   // whole commits, in file order
	std::vector<uint64> Revisions;        // per delta: the revision its process committed it at
	uint64 ValidBytes = 0u;           // the header and the whole commits
	uint64 FileBytes = 0u;
	bool bTornTail = false;           // FileBytes > ValidBytes: dropped
	FString Message;                  // why the file or its tail was not used
};

// Reads and decodes a journal. bDecode false: frames are checked and counted, deltas not decoded.
[[nodiscard]] FRT_CORE_API SJournalContents ReadJournal (const std::filesystem::path& Path, bool bDecode = true);

struct SJournalStats
{
	uint32 Commits = 0u;              // commits written by this object (empty ones not counted)
	uint64 Bytes = 0u;                // bytes written by this object
	double LastAppendMs = 0.0;        // the last Append that wrote: filter, encode and write
	uint32 Restarts = 0u;
	double LastRestartMs = 0.0;       // the last restart that succeeded, on the owner thread: Restart, or BeginRestart + FinishRestart
	double LastRestartFileMs = 0.0;   // ... of which file work: Restart's closing, writing and reopening; FinishRestart's
	                                  // handle swap (the old one closed) and append of the later commits
	double LastRestartWorkerMs = 0.0; // the last FinishRestart's CJournalRestart::Prepare, on the worker (0 after a Restart)
	uint32 FailedAppends = 0u;        // appends that broke the journal
};

/**
 * A journal restart prepared on a worker (protocol step 3 in the header comment): made by
 * CRegistryJournal::BeginRestart on the owner thread, Prepare on a worker after the snapshot was written, back
 * to CRegistryJournal::FinishRestart on the owner thread. Movable; dropping one that was prepared leaves the
 * new journal on disk without the commits made since BeginRestart (a rescan finds them).
 */
class FRT_CORE_API CJournalRestart
{
public:
	CJournalRestart () = default;
	CJournalRestart (CJournalRestart&&) noexcept = default;
	CJournalRestart& operator= (CJournalRestart&&) noexcept = default;

	// The worker's part: the new journal written beside the old one (<journal>.next), renamed over it and
	// opened for appending. False, with the reason, when any step failed (the old journal stays; FinishRestart
	// then restarts on the owner thread).
	bool Prepare (FString* OutError = nullptr);
	[[nodiscard]] bool IsPrepared () const noexcept { return bPrepared; }
	[[nodiscard]] uint64 GetJournalBase () const noexcept { return Base; }

private:
	friend class CRegistryJournal;

	std::filesystem::path Path;
	SGuid ProjectId;
	uint64 Base = 0u;
	uint64 Generation = 0u;            // the journal's when BeginRestart made it: a later Open, Close or restart makes it stale
	size_t CoveredBytes = 0u;          // the journal's in-memory frames when BeginRestart made it
	std::vector<uint8> Frames;         // of those, the commits after Base
	platform::CAppendFile File;        // the new journal, open for appending (Prepare)
	bool bPrepared = false;
	double PrepareMs = 0.0;
};

class FRT_CORE_API CRegistryJournal
{
public:
	CRegistryJournal () = default;
	CRegistryJournal (const CRegistryJournal&) = delete;
	CRegistryJournal& operator= (const CRegistryJournal&) = delete;

	/**
	 * Opens the journal at Path for appending. ContinueFrom: the JournalBase of the snapshot that was loaded;
	 * a journal whose header names it (and ProjectId) is kept, its torn tail cut off. nullopt, or any other
	 * header, starts a new journal (JournalBase = ContinueFrom, or 0). MountNames: the snapshot file's mounts
	 * (every mount when empty). False, with the reason, when the file cannot be written.
	 */
	bool Open (const std::filesystem::path& Path, const SGuid& ProjectId, std::optional<uint64> ContinueFrom, std::vector<FString> MountNames,
		FString* OutError = nullptr);
	void Close () noexcept;
	[[nodiscard]] bool IsOpen () const noexcept { return File.IsOpen(); }

	// Appends the part of Delta for this journal's mounts as committed at Revision (one write). A delta with
	// no such part writes nothing and returns true. False when the write failed or the journal is broken: the
	// commit is kept in memory for the next Restart (protocol step 4).
	bool Append (const SRegistryDelta& Delta, uint64 Revision, FString* OutError = nullptr);

	// After a full snapshot of a view at JournalBase was written: a new journal at JournalBase that keeps the
	// frames this process appended after that revision, from memory, then reopened. Clears a broken state;
	// false (the journal stays broken, the frames in memory) when the file cannot be written. All of it on
	// the calling (owner) thread: the file work can take milliseconds on a busy machine, so a full write
	// prepares it on its worker instead (BeginRestart, CJournalRestart::Prepare, FinishRestart).
	bool Restart (uint64 JournalBase, FString* OutError = nullptr);

	// Protocol step 3, the owner thread's part where the full write's view is acquired (JournalBase: its
	// revision): copies the commits after it appended so far (none, when the view is the current revision).
	[[nodiscard]] CJournalRestart BeginRestart (uint64 JournalBase);
	// The owner thread's part once the worker is done: a prepared restart takes the new file's handle and
	// appends the commits made since BeginRestart (one write); one the worker could not prepare restarts here
	// (Restart). Clears a broken state. False when the restart is stale (an Open, Close or restart came in
	// between: nothing changes) or failed (the journal is broken, the frames in memory).
	bool FinishRestart (CJournalRestart&& Prepared, FString* OutError = nullptr);

	// An Append failed (or a Restart did) since the last Restart or Open: F3 schedules a full write.
	[[nodiscard]] bool IsBroken () const noexcept { return bBroken; }

	[[nodiscard]] uint64 GetJournalBase () const noexcept { return JournalBase; }
	[[nodiscard]] const std::filesystem::path& GetPath () const noexcept { return Path; }
	[[nodiscard]] const SJournalStats& GetStats () const noexcept { return Stats; }

private:
	friend struct SRegistryJournalTestAccess;   // Core-Test: injects a write cut short

	bool StartNew (uint64 Base, std::span<const uint8> Frames, FString* OutError);
	[[nodiscard]] bool Keeps (FStringView MountName) const noexcept;

	platform::CAppendFile File;
	std::filesystem::path Path;
	SGuid ProjectId;
	uint64 JournalBase = 0u;
	std::vector<FString> MountNames;
	std::vector<uint8> OwnFrames;     // every commit this process appended since the last Restart or Open, written or not
	uint64 Generation = 0u;           // bumped by Open, Close and every restart (stale CJournalRestarts)
	double BeginRestartMs = 0.0;      // the last BeginRestart's time, for FinishRestart's stats
	bool bBroken = false;
	size_t FailAppendAfterBytes = ~size_t(0);   // tests: the next Append writes only this many bytes and fails
	SJournalStats Stats;
};
}
