#pragma once

#include <filesystem>
#include <optional>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/MountPaths.h"
#include "Containers/String.h"
#include "Platform/FileLock.h"


/**
 * One writer per mount (design §3.3, §4.7, §5.9): the process that writes a project's metas,
 * snapshots, journals and BuildState holds <project>/Local/Assets/Owner.lock, and the owner of
 * /Engine holds <engine>/Local/Assets/Owner.lock. The lock is always on (§4.8): a second process
 * that wants to write is refused and told who owns the mount, so it can say "Editor.exe, pid 1234,
 * owns /Project: close it, or run with --ci". Processes that are read-only by request (--ci,
 * -Assets:ReadOnly=1, the runtime) never take it.
 *
 * AssetTool takes it from M1 for every commandlet that writes; the editor takes the same lock from
 * M3 (CEngine, SEngineDesc::bAssetWriter), for the registry's snapshot, journal, Rules.cache and
 * ProjectId - and runs without writing them when another process holds it. The lock itself is platform::TryLockFileExclusive (the OS drops
 * it when the holder dies, so a crash never leaves a stale owner); this file is what the holder
 * writes into it - {pid, exe, start time} - and how a refused process reads that back.
 *
 * The lock file's text, UTF-8 with LF line endings, one Key=Value per line:
 *
 *     Pid=1234
 *     Exe=D:\FRTEngine2\Binaries\Win64\Editor-Development-Default.exe
 *     Started=2026-10-24T09:41:07Z
 *
 * Started is the holder's process start time in UTC, so a reused PID never reads as the same holder.
 */
namespace frt::assets
{
// The holder of an Owner.lock.
struct SLockOwner
{
	uint32 ProcessId = 0u;
	FString Executable;     // its full path, as the OS reports it
	FString StartTime;      // UTC, ISO 8601 to the second: "2026-10-24T09:41:07Z"

	// "Editor-Development-Default.exe, pid 1234, started 2026-10-24 09:41:07 UTC": what a refusal shows.
	[[nodiscard]] FRT_CORE_API FString Describe () const;

	[[nodiscard]] bool operator== (const SLockOwner& Other) const = default;
};

// <MountRoot>/Local/Assets/Owner.lock (GetMountAssetsLocalDir).
[[nodiscard]] FRT_CORE_API std::filesystem::path GetOwnerLockPath (const std::filesystem::path& MountRoot);

// This process, as it describes itself in an Owner.lock it holds.
[[nodiscard]] FRT_CORE_API SLockOwner GetThisProcessAsOwner ();

// A file time (100 ns ticks since 1601-01-01 UTC, platform::GetProcessStartTime) as Started writes
// it: "2026-10-24T09:41:07Z", the fraction of a second dropped.
[[nodiscard]] FRT_CORE_API FString FormatUtcTime (int64 FileTime);

// The lock file's text for Owner (the format above).
[[nodiscard]] FRT_CORE_API FString FormatOwnerLock (const SLockOwner& Owner);
// The holder back from that text: unknown keys are skipped, CRLF is read as LF. nullopt without a
// Pid that is a number other than 0 - a file some other tool wrote, or no lock file text at all.
[[nodiscard]] FRT_CORE_API std::optional<SLockOwner> ParseOwnerLock (FStringView Text);

/**
 * Takes the writer lock of the mount whose root is Root for this process (GetThisProcessAsOwner),
 * creating Local/Assets if needed. The lock is held until the returned object goes.
 *
 * nullopt when it is not taken. If another process owns the mount, *OutOwner names it: its
 * Describe(), or - for a lock file whose text is not an Owner.lock's - that text as it is, so the
 * user still sees something. Never empty in that case. On any other failure *OutOwner is empty and
 * *OutError says why (a caller reports that as an internal error, not as "owned").
 */
[[nodiscard]] FRT_CORE_API std::optional<platform::CExclusiveFileLock> TryTakeOwnerLock (const std::filesystem::path& Root,
	FString* OutOwner, FString* OutError = nullptr);
}
