#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"


/**
 * Change events for directory trees (design §5.4 "The watcher", §8.15, §15 I14 and I27): what the OS
 * reports when files under a watched root are created, written, renamed, moved or deleted, as
 * portable events. One implementation per platform (Windows: Platform/Windows/FileWatcherWindows.cpp).
 *
 * This layer only reads the OS's reports and hands them on; it decides nothing. Pairing moves,
 * coalescing an atomic save or a VCS sync into one change, settling, rescans and subscribers are the
 * portable CFileWatcherService's (Assets/FileWatcherService.h), on every platform. The reason for the
 * split is measured: a reader that does real work between two reads overflows the OS's buffer and
 * loses every event in it, so the platform side runs on a thread of its own that only re-arms, parses
 * and queues, and never waits for its consumer.
 *
 * What callers may assume, on every platform (the portable subset of §8.15):
 *	- Events of one root arrive in the order the OS reported them; nothing is said about the order
 *	  across roots.
 *	- Events may be coalesced or lost. A loss is reported as Overflow, and the only correct answer to
 *	  it is a rescan of the root: events say "look here", never "this is what happened".
 *	- A rename within a root is RenamedOld then RenamedNew. A move between directories may instead be
 *	  Removed then Added (Windows does that), with the same identity where the platform reports one.
 *	- A moved, renamed or deleted directory is one event: nothing is reported for its children (it
 *	  is the consumer's job to enumerate a directory that moved in, and to forget one that moved out).
 *	- Identity is the identity platform::Stat reports for the file (FileSystem.h): compared for
 *	  equality only, and only to pair the two halves of a move. Atomic saves and VCS syncs change it.
 *	- Changes to a root directory itself are not reported; the root going away is RootLost.
 */
namespace frt::platform
{
enum class EFileEventKind : uint8
{
	Added,        // an entry appeared at Path: created, or moved or copied in
	Removed,      // the entry at Path is gone: deleted, moved out, or replaced
	Modified,     // a file's contents, size or times changed (Windows: usually when the writer closes it)
	RenamedOld,   // Path is the old name of an entry renamed within the root; its RenamedNew follows
	RenamedNew,   // Path is the new name of the entry of the RenamedOld before it
	Overflow,     // events of the root were lost (Path is empty): rescan it
	RootLost,     // the root went away - deleted, renamed, its volume gone (Path is empty). Nothing more is
	              // reported until it is back; then the root is armed again by itself, and Overflow says so
};

// What an event's entry is, where the platform says (Windows: ReadDirectoryChangesExW does; the plain
// ReadDirectoryChangesW fallback, on file systems without the extended information, does not).
enum class EFileEntryType : uint8
{
	Unknown,
	File,
	Directory,
};

struct SFileEvent
{
	uint32 RootId = 0u;
	EFileEventKind Kind = EFileEventKind::Modified;
	EFileEntryType EntryType = EFileEntryType::Unknown;
	// Relative to the root, '/' between names, in UTF-8 (WTF-8 for a name that is not valid UTF-16,
	// FString::FromWide). The case is the case the OS reported. Empty for Overflow and RootLost.
	FString Path;
	// The entry's identity, as Stat(<root>/<Path>) reports it while it exists (FileSystem.h), when the
	// platform reports one per event (Windows: ReadDirectoryChangesExW's file id with the root's volume
	// serial number); invalid otherwise. A Removed event carries the identity the entry had.
	SFileIdentity Identity;
	// platform::ReadTimestamp() when the watcher read the event from the OS.
	uint64 Timestamp = 0u;
};

struct SFileWatcherDesc
{
	// Bytes the OS may buffer per root between two reads; a burst larger than that is an Overflow.
	// 64 KiB is the most a network share accepts (larger reads fail there, and the watcher falls back
	// to 64 KiB by itself). A buffer smaller than one report (64 bytes, the minimum) overflows on every
	// change: what tests use to force an Overflow. Rounded up to 8.
	uint32 BufferBytes = 64u * 1024u;
	// How often a lost root is looked for, and armed roots are checked for having been renamed (which
	// Windows does not report: the handle follows the directory to its new name, §5.4).
	uint32 RootPollMs = 1000u;
	// Queued events beyond this, never taken, are dropped and replaced by one Overflow per root: a
	// consumer that stopped taking events must rescan, not run out of memory.
	uint32 MaxQueuedEvents = 256u * 1024u;
	// The watcher thread's name (platform::ConfigureThread); the thread that looks for lost and renamed
	// roots has the same with " Poll".
	FString ThreadName = "FRT File Watcher";
};

/**
 * One watcher over any number of roots: one OS thread for all of them (Windows: one I/O completion
 * port), started when the watcher is created and stopped when it is destroyed. Arm, Disarm,
 * TakeEvents and WaitForEvents may be called from any thread, but are meant for one owner thread (the
 * CFileWatcherService's); the watcher thread never calls back into its owner.
 */
class IFileWatcher
{
public:
	virtual ~IFileWatcher () = default;

	// Starts watching the directory Root and everything below it, recursively. Returns the root's id,
	// never 0 and never reused by this watcher, or 0 with the reason when Root is not a directory that
	// can be watched (missing, a file, no access, a file system without change notifications). Roots
	// may overlap; each reports its own events.
	virtual uint32 Arm (const std::filesystem::path& Root, FString* OutError = nullptr) = 0;
	template <NarrowText T>
	uint32 Arm (const T&, FString* = nullptr) = delete;

	// Stops watching RootId. Once it returns, TakeEvents never returns an event of that root again,
	// including ones already queued. An unknown or disarmed id is ignored.
	virtual void Disarm (uint32 RootId) = 0;

	// Appends every queued event to Out, in the order they were queued, and empties the queue.
	virtual void TakeEvents (std::vector<SFileEvent>& Out) = 0;

	// Sleeps until an event is queued (returns at once if one is), at most TimeoutSeconds (negative:
	// no limit). False on timeout.
	virtual bool WaitForEvents (double TimeoutSeconds) = 0;
};

// The platform's watcher: Windows ReadDirectoryChangesExW on one I/O completion port thread (falling
// back to ReadDirectoryChangesW, without identities, where a file system lacks the extended
// information). Never null.
[[nodiscard]] FRT_CORE_API std::unique_ptr<IFileWatcher> CreateFileWatcher (const SFileWatcherDesc& Desc = {});


/**
 * Whether some process still holds a file open for writing (§5.4: a Modified event arrives when a
 * writer closes the file, usually, but an editor may keep writing, and a reader that hashes the bytes
 * now would describe a version that never existed). The watcher service asks before telling anyone
 * a file changed, and asks again later while the answer is Held.
 *
 * Windows: CreateFileW for reading, sharing reading and deleting but not writing, closed at once: a
 * sharing violation means a handle with write access is open (or one that denies reading, as S19's
 * writer with share mode 0). The probe itself refuses a writer during the microseconds it is open -
 * the cost the research accepts (gap-watch-meta §1.6) - and never keeps the handle. Platforms
 * without share modes answer Free and rely on the settle window.
 */
enum class EWriterProbe : uint8
{
	Free,      // nobody writes it now
	Held,      // a writer holds it: ask again later
	Missing,   // there is no file at the path (any more)
	Failed,    // something else: a directory, no access
};

[[nodiscard]] FRT_CORE_API EWriterProbe ProbeFileWriters (const std::filesystem::path& Path) noexcept;

template <NarrowText T>
EWriterProbe ProbeFileWriters (const T&) noexcept = delete;
}
