#pragma once

#include <filesystem>
#include <optional>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Platform/Platform.h"


/**
 * A lock that the operating system lets go of when its holder dies (design §5.9, §8.15, §15 I27).
 *
 * One process at a time writes a project's metas, snapshots and BuildState: the owner, which holds
 * <project>/Local/Assets/Owner.lock (Assets/OwnerLock.h). A lock a process has to remove itself is
 * left behind by every crash, and then needs a staleness rule - a PID that may have been reused, a
 * timeout that a slow machine passes - and a "delete it if you are sure" prompt. So the lock is a
 * file the OS removes: open while the holder lives, gone as soon as its process ends, however it
 * ends (an exit, a crash, TerminateProcess, a debugger stopping it).
 *
 * The file also says who holds it: the holder writes Contents into it, and a process that is
 * refused reads them back (its PID, executable and start time, for a message that names it). Only
 * the holder's handle keeps the lock; reading the file takes nothing.
 *
 * Windows: CreateFileW(CREATE_ALWAYS) with FILE_FLAG_DELETE_ON_CLOSE, sharing nothing but reading:
 * a second open for writing, or for deleting or renaming the file, fails with a sharing violation
 * while the holder's handle is open, and the kernel closes that handle - deleting the file - when
 * the process ends. Readers open the file sharing read, write and delete, so they never block the
 * holder. A file left by a power cut (no process ever closed it) holds no handle: the next caller
 * takes it over and rewrites it. POSIX later: open + flock(LOCK_EX | LOCK_NB), which the kernel
 * drops with the process too; the file stays, and an flock on it is what counts.
 *
 * Locks are per open, not per process: taking a lock this process already holds fails as for any
 * other holder. Uses no engine allocator: works before the memory pool exists, on any thread.
 */
namespace frt::platform
{
class CExclusiveFileLock;

/**
 * Takes the lock at Path, creating the file - and its directory - if needed, and writes Contents
 * into it (UTF-8, as given; it must not be empty). Returns the lock, which holds it until it is
 * destroyed or Released, or until the process ends.
 *
 * nullopt when it is not taken:
 *   - another holder has it: *OutHolder receives what that holder wrote. A holder between creating
 *     the file and writing it, or one that is just letting go, is waited for, briefly; if its text
 *     still cannot be read, *OutHolder says so in words. So *OutHolder is never empty here.
 *   - anything else (no access, a directory in the way, a bad path): *OutHolder is cleared and
 *     *OutError says why.
 * *OutError is set in both cases ("... is held by another process" in the first).
 *
 * Retries for up to about half a second while the file is pending deletion (a holder that has just
 * let go while someone was reading it): never longer, so a tool that is refused says so at once.
 */
[[nodiscard]] FRT_CORE_API std::optional<CExclusiveFileLock> TryLockFileExclusive (const std::filesystem::path& Path,
	FStringView Contents, FString* OutHolder, FString* OutError = nullptr);

template <NarrowText T>
std::optional<CExclusiveFileLock> TryLockFileExclusive (const T&, FStringView, FString*, FString* = nullptr) = delete;


// The lock TryLockFileExclusive took: move-only, held until destroyed or Released.
class FRT_CORE_API CExclusiveFileLock
{
public:
	CExclusiveFileLock () noexcept = default;
	~CExclusiveFileLock ();

	CExclusiveFileLock (const CExclusiveFileLock&) = delete;
	CExclusiveFileLock& operator= (const CExclusiveFileLock&) = delete;
	CExclusiveFileLock (CExclusiveFileLock&& Other) noexcept;
	CExclusiveFileLock& operator= (CExclusiveFileLock&& Other) noexcept;

	[[nodiscard]] bool IsLocked () const noexcept { return Handle != nullptr; }
	// The path it was taken at, as the caller spelled it; empty when not locked.
	[[nodiscard]] const std::filesystem::path& GetPath () const noexcept { return Path; }

	// Lets go: the OS deletes the file (once no reader has it open either). Idempotent.
	void Release () noexcept;

private:
	friend FRT_CORE_API std::optional<CExclusiveFileLock> TryLockFileExclusive (const std::filesystem::path& Path,
		FStringView Contents, FString* OutHolder, FString* OutError);

	CExclusiveFileLock (void* InHandle, std::filesystem::path InPath) noexcept;

	void* Handle = nullptr;   // the OS handle that keeps the lock; nullptr when not locked
	std::filesystem::path Path;
};
}
