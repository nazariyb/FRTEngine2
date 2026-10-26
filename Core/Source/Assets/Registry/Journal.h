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
 *	3. A full write: WriteSnapshot(view, ...) on a worker; when it succeeded, Restart(view revision) on the
 *	   owner thread. Restart keeps this process's frames committed after that view (the worker ran while the
 *	   owner kept committing) and drops everything older, which the snapshot holds. A crash between the two
 *	   leaves a journal whose JournalBase no longer matches the snapshot: it is ignored at load, and the scan
 *	   finds the few changes it held (the snapshot's file stats are older than the disk's).
 *	4. A failed Append (a full disk, a write cut short) breaks the journal: part of the commit may be in the
 *	   file, and nothing appended after it would be read, so the file is closed and every later Append returns
 *	   false (IsBroken) until Restart or Open. The commits are not lost: Append keeps every commit's frames in
 *	   memory, written or not, and Restart writes the new journal from them. On the first false Append F3 logs
 *	   once and schedules a full write at once (step 3); a Restart that fails leaves the journal broken, and
 *	   the next full write tries again. A crash before that costs a rescan of what changed since the last
 *	   good commit, the same as a lost journal.
 *
 * Memory: the frames this process appended since the last Restart (or Open) stay in memory, so Restart reads
 * nothing back and never stalls the owner thread on the file - it writes the header and the kept frames in
 * place, unflushed (see StartNew). Full writes come at least every batch of 1000 records or 60 s idle, which
 * bounds them.
 *
 * Owner thread only (one CRegistryJournal per file, used where the commits happen). No engine allocator.
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
	double LastRestartMs = 0.0;       // the last Restart that succeeded (owner thread)
	uint32 FailedAppends = 0u;        // appends that broke the journal
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
	// false (the journal stays broken, the frames in memory) when the file cannot be written.
	bool Restart (uint64 JournalBase, FString* OutError = nullptr);

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
	bool bBroken = false;
	size_t FailAppendAfterBytes = ~size_t(0);   // tests: the next Append writes only this many bytes and fails
	SJournalStats Stats;
};
}
