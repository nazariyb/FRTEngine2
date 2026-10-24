#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Platform/BlockingFileReader.h"
#include "Platform/Platform.h"


/**
 * The file layer of the asset system (design §8.15, §15 I3, I27): what it needs from the OS about
 * files, as portable declarations with one implementation per platform (Platform/Windows/ today).
 *
 * The callers assume the portable subset, never Win32 behaviour: identities are compared for
 * equality only; an atomic replace is atomic for readers and durable when it returns; case
 * sensitivity is asked per volume and per directory, never assumed; a directory listing returns
 * everything a scan needs in one pass, so unchanged files are never opened (which also keeps
 * Windows Defender out of the start-up path, §5.4).
 *
 * No engine allocator anywhere (std::vector, FString on the CRT heap): all of it is safe on worker
 * threads and before the memory pool exists. Paths are std::filesystem::path; names handed back are
 * UTF-8 (canonical WTF-8 for names that are not valid UTF-16, so they round-trip through ToPath).
 * Functions that can fail return false or nullopt and, when given OutError, say why in the user's
 * language (GetSystemErrorMessage), naming the path.
 */
namespace frt::platform
{
/**
 * Which file a path names, as opaque bytes compared for equality only. It survives a rename or
 * move within the volume; a copy, an atomic save (WriteFileAtomic, most editors' "save"), a fresh
 * clone or a sync client rewriting the file give a new one. So it pairs a move or copy seen by the
 * scan or the watcher (§4.7, §5.4) and detects a replaced file during a read; it is never an
 * asset's identity, and never written anywhere that outlives the machine.
 *
 * Windows: the volume serial number (64-bit) and the 128-bit file id of FILE_ID_INFO - on NTFS the
 * file reference number (MFT index and sequence number), on ReFS a true 128-bit id. POSIX later:
 * (st_dev, st_ino). All zero: no identity (the default; never a real file's).
 */
struct SFileIdentity
{
	uint8 Bytes[24] = {};

	[[nodiscard]] bool IsValid () const noexcept
	{
		for (const uint8 byte : Bytes)
		{
			if (byte != 0u)
			{
				return true;
			}
		}
		return false;
	}

	// For hash maps keyed by identity (pairing tables); in-memory only, like hash::HashBytes.
	[[nodiscard]] uint64 GetHash () const noexcept { return hash::HashBytes(Bytes, sizeof(Bytes)); }

	[[nodiscard]] bool operator== (const SFileIdentity& Other) const noexcept = default;
};

/**
 * What a scan compares against its record: size and last write time (a file is unchanged when both
 * are equal - never "newer than": NTFS tunnelling and CopyFile restore older times, §5.4), plus the
 * identity for pairing.
 *
 * Times are file times: 100 ns ticks since 1601-01-01 UTC, the unit of Windows' FILETIME and of
 * MSVC's std::filesystem::file_time_type (file_time_type(file_time_type::duration(Ticks)) converts).
 * Other platforms convert to it. The file system's clock is coarse (on Windows the system timer,
 * 1-16 ms), so two writes within one tick may leave the same time: equality means "probably
 * unchanged", which is why content hashes back it up where it matters (§5.4, §7.6).
 */
struct SFileStat
{
	uint64 Size = 0u;               // bytes; 0 for a directory
	int64 LastWriteTime = 0;        // file time (see above)
	int64 CreationTime = 0;         // file time
	SFileIdentity Identity;
	bool bDirectory = false;
	bool bReadOnly = false;         // the read-only attribute: writes and replaces fail (Perforce keeps unopened files so)
};

// The file or directory at Path, following symbolic links and junctions (the file a read would
// open). nullopt if it does not exist or cannot be opened even for its attributes. Stat never
// conflicts with another process's share mode: it asks for no read, write or delete access.
[[nodiscard]] FRT_CORE_API std::optional<SFileStat> Stat (const std::filesystem::path& Path, FString* OutError = nullptr);
// Stat(Path)->Identity, for callers that need nothing else (watcher pairing, §5.4).
[[nodiscard]] FRT_CORE_API std::optional<SFileIdentity> GetFileIdentity (const std::filesystem::path& Path, FString* OutError = nullptr);

template <NarrowText T>
std::optional<SFileStat> Stat (const T&, FString* = nullptr) = delete;
template <NarrowText T>
std::optional<SFileIdentity> GetFileIdentity (const T&, FString* = nullptr) = delete;


/**
 * One entry of a directory listing, valid only during the callback that receives it.
 *
 * Name is the entry's name (no directory part) in UTF-8; copy it to keep it. Stat is the entry as
 * the directory reports it, in the same call that listed it: no per-file open (§5.4). For a file
 * another process is still writing, the size and times may lag Stat() of the file until that
 * process closes it (NTFS updates directory entries lazily); identities always agree. A symbolic
 * link or junction is reported as itself (bReparsePoint), not followed: Stat() of its path
 * describes the target instead, and a scanner must not descend into one blindly (cycles).
 */
struct SDirectoryEntry
{
	FStringView Name;
	SFileStat Stat;
	bool bReparsePoint = false;
};

/**
 * Calls OnEntry(const SDirectoryEntry&) for every entry of Directory except "." and "..", in the
 * file system's order (NTFS: by name, upcase order; never rely on it). OnEntry returns bool, where
 * false stops the listing, or void (the overload below). Not recursive: a scan walks directories
 * itself, so it can skip excluded ones before reading them.
 *
 * Returns true when the listing ran to its end or OnEntry stopped it; false (with OutError) when
 * the directory cannot be opened or a read of the listing fails midway, possibly after some
 * entries were delivered.
 *
 * Windows: GetFileInformationByHandleEx(FileIdExtdDirectoryInfo) on the directory handle, 64 KB per
 * call (larger buffers fail on some network shares): name, size, times, attributes and the file id
 * in one pass, which FindFirstFileExW cannot give (no file id). UE does the same in its file
 * journal (WindowsPlatformFileJournal.cpp, FileJournalIterateDirectory). A file system that does
 * not implement the extended class (NTFS and ReFS do) fails with a message saying so; there is no
 * fallback without file ids.
 */
FRT_CORE_API bool EnumerateDirectory (const std::filesystem::path& Directory,
	TFunctionRef<bool (const SDirectoryEntry&)> OnEntry, FString* OutError = nullptr);

// A visitor that returns nothing sees every entry.
template <typename F>
	requires std::is_void_v<std::invoke_result_t<F&, const SDirectoryEntry&>>
bool EnumerateDirectory (const std::filesystem::path& Directory, F&& OnEntry, FString* OutError = nullptr)
{
	const auto visitAll = [&OnEntry] (const SDirectoryEntry& Entry) { OnEntry(Entry); return true; };
	return EnumerateDirectory(Directory, visitAll, OutError);
}

template <NarrowText T, typename F>
bool EnumerateDirectory (const T&, F&&, FString* = nullptr) = delete;


/**
 * Writes that never leave a torn file behind (§4.7, §15 I3). Every FRT write of sources, metas,
 * snapshots and DDC files goes through WriteFileAtomic; WriteFileBytes is the plain in-place write
 * for files nothing else reads concurrently (scratch output, test fixtures).
 */

// Replaces the file at Path with Bytes in place (created or truncated; binary). Creates the parent
// directories first when bCreateDirectories. A crash midway leaves a partial file: use
// WriteFileAtomic for anything that is read back. The engine's one in-place writer: WriteFileText
// (Platform.h) is this with the text's bytes.
FRT_CORE_API bool WriteFileBytes (const std::filesystem::path& Path, std::span<const uint8> Bytes, bool bCreateDirectories = true,
	FString* OutError = nullptr);

// Moves Temp over Target in one step: readers see the old file or the new one, never a mix, and
// once it returns true the new file survives a crash or power loss. Target may not exist yet. Both
// must be on the same volume (normally the same directory: WriteFileAtomic), and Temp must be
// writable (it is a file the caller just wrote). Temp keeps its identity and Target's old identity
// is gone; a reader that has the old Target open keeps reading the old bytes. On failure Temp is
// left where it was, for the caller to delete or retry - except in one case: the rename went
// through but could not be flushed to disk (a device error), where Target already holds the new
// file, Temp is gone, and false says the replace is not known to be durable.
//
// Windows: a rename with POSIX semantics (SetFileInformationByHandle(FileRenameInfoEx,
// FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS), Windows 10 1709+ on NTFS),
// which replaces a target other handles hold open with FILE_SHARE_DELETE - CBlockingFileReader's
// own, a Stat in another thread - followed by FlushFileBuffers on the renamed file, which on NTFS
// writes its metadata and the log that holds the rename. Where the file system does not support
// it, MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), which fails while any
// handle is open. Both are journaled metadata operations on NTFS: after a crash the name holds the
// old file or the new one. Paths of MAX_PATH units or more are passed in their extended-length
// form ("\\?\"). A target someone holds open without FILE_SHARE_DELETE (an indexer, a virus
// scanner, a viewer) fails with a sharing violation or access denied: retried for about a second
// with growing sleeps, then reported. UE retries its moves for five seconds
// (FFileManagerGeneric::Move, 10 x 0.5 s), but deletes the target first, so its move is not
// atomic; an interactive save here would rather fail visibly than hang. A read-only target fails
// at once (no retry: it will not clear by itself).
FRT_CORE_API bool ReplaceFileAtomic (const std::filesystem::path& Temp, const std::filesystem::path& Target, FString* OutError = nullptr);

// Path holds Bytes afterwards, or - after any failure or crash - exactly what it held before. The
// bytes go to a new temporary file in the same directory (".<name>.<pid>-<n>.tmp": a dot-name and
// a .tmp, both excluded from asset scans, §3.5.2), are flushed to disk (FlushFileBuffers), then
// ReplaceFileAtomic swaps it in. A failure deletes the temporary file; a crash can leave one
// behind, never a partial Path. Path gets a new identity. Creates the parent directories first
// when bCreateDirectories. The temporary file's name is longer than Path's; on Windows it is opened
// by its extended-length path where that passes MAX_PATH, so WriteFileAtomic works wherever
// WriteFileBytes does.
FRT_CORE_API bool WriteFileAtomic (const std::filesystem::path& Path, std::span<const uint8> Bytes, bool bCreateDirectories = true,
	FString* OutError = nullptr);
// The same for text: its bytes as given (UTF-8 without a BOM, line endings untouched).
FRT_CORE_API bool WriteFileAtomic (const std::filesystem::path& Path, FStringView Text, bool bCreateDirectories = true,
	FString* OutError = nullptr);

template <NarrowText T>
bool WriteFileBytes (const T&, std::span<const uint8>, bool = true, FString* = nullptr) = delete;
template <NarrowText T, NarrowText U>
bool ReplaceFileAtomic (const T&, const U&, FString* = nullptr) = delete;
template <NarrowText T>
bool WriteFileAtomic (const T&, std::span<const uint8>, bool = true, FString* = nullptr) = delete;
template <NarrowText T>
bool WriteFileAtomic (const T&, FStringView, bool = true, FString* = nullptr) = delete;


// Moves a file or directory to the user's Recycle Bin, so an editor delete stays recoverable
// (§4.6). True only when it landed there: on a volume without a Recycle Bin (a network share, most
// removable drives) it refuses instead of deleting permanently, and leaves the item in place.
// Shows no UI. Blocking; may take a moment.
//
// Windows: IFileOperation with FOFX_RECYCLEONDELETE (the documented successor of SHFileOperation),
// on the calling thread, which it initializes for COM as a single-threaded apartment if nothing
// did yet (and uninitializes after). IFileOperation needs an STA: on a thread already in the
// multithreaded apartment the operation runs on a short-lived helper thread instead.
FRT_CORE_API bool MoveToTrash (const std::filesystem::path& Path, FString* OutError = nullptr);

template <NarrowText T>
bool MoveToTrash (const T&, FString* = nullptr) = delete;


/**
 * What the asset system must know about the volume and directory a mount lives in (§3.1, §5.2).
 *
 * bCaseSensitive is whether names that differ only in case are different files to FRT's own opens
 * here, which decides how the registry's path index compares (§5.2). On Windows that is not the
 * volume flag alone: NTFS reports FILE_CASE_SENSITIVE_SEARCH (it can hold such names), but
 * CreateFileW asks for a case-insensitive lookup (OBJ_CASE_INSENSITIVE, unless
 * FILE_FLAG_POSIX_SEMANTICS, which FRT never passes), and NTFS grants it everywhere except in a
 * directory with the per-directory case-sensitive attribute ("fsutil file setCaseSensitiveInfo",
 * made for WSL). So bCaseSensitive = bVolumeCaseSensitive && bDirectoryCaseSensitive. The kernel's
 * obcaseinsensitive = 0 changes nothing for FRT: it only stops Windows from forcing case-insensitive
 * lookups on callers that ask for case-sensitive ones (POSIX-semantics tools), which can leave
 * names that differ only in case in an ordinary directory - PATH001 reports those.
 *
 * bRemote and bCloudPlaceholder trigger the one-time warning of §3.1 for a Local/ cache on a
 * network share or a cloud-synced folder (OneDrive Files On-Demand): reading a placeholder
 * downloads it, and sync clients rewrite files behind FRT's back.
 */
struct SVolumeInfo
{
	FString FileSystemName;           // "NTFS", "ReFS", "exFAT", ...
	uint32 MaxComponentLength = 0u;   // longest file name, in the file system's units (UTF-16 on Windows)
	bool bCaseSensitive = false;      // see above: what lookups in this directory do
	bool bVolumeCaseSensitive = false;     // the volume can keep names that differ only in case
	bool bDirectoryCaseSensitive = false;  // the directory's own case-sensitive attribute
	bool bCasePreserving = false;     // names keep the case they were created with
	bool bRemote = false;             // a network share or mapped network drive
	bool bRemovable = false;          // a removable drive (USB stick, card)
	bool bReadOnly = false;           // the volume is read-only
	bool bCloudPlaceholder = false;   // inside a cloud sync root, or itself a cloud placeholder
};

// The facts above for the directory Path (for a file: its directory). nullopt if Path does not
// exist or its volume cannot be queried.
//
// Windows: GetVolumeInformationByHandleW (file system name and flags), GetFileInformationByHandleEx
// (FileCaseSensitiveInfo), GetDriveTypeW of the volume root, and for the cloud:
// CfGetSyncRootInfoByPath (any Cloud Files sync root - OneDrive, and others built on that API) or a
// placeholder attribute on the directory itself (FILE_ATTRIBUTE_RECALL_ON_OPEN,
// RECALL_ON_DATA_ACCESS, PINNED, UNPINNED, OFFLINE).
[[nodiscard]] FRT_CORE_API std::optional<SVolumeInfo> QueryVolumeInfo (const std::filesystem::path& Path, FString* OutError = nullptr);

template <NarrowText T>
std::optional<SVolumeInfo> QueryVolumeInfo (const T&, FString* = nullptr) = delete;


/**
 * Reading a whole file while knowing it did not change underneath (§5.4 "Hashing is race-free").
 *
 * A content hash of a file that an editor rewrites while it is read would describe bytes that
 * never existed together; stored in the snapshot or a DDC key, it would yield a stale hit later.
 * So the reader stats the file before (size, last write time, identity), reads it in chunks
 * through CBlockingFileReader, and stats it again: any difference, a size other than the one
 * stat saw, or a read that ends early means Changed, and the caller discards what it computed and
 * retries after the watcher's settle window. A file that is in the middle of being written counts
 * as Changed too: one that vanished between the first stat and the open, and one held by a writer
 * that denies reading or has locked the bytes being read (EFileFailure::InUse: an editor saving
 * with share mode 0; §5.4 backs off on a sharing violation). A file some process keeps locked for
 * good stays Changed on every retry; the caller's retry limit turns that into an error. Failed:
 * the file could not be read at all - missing at the start, a directory, no access.
 *
 * What it cannot see: bytes overwritten in place with neither the size nor the last write time
 * changing - a rewrite within one tick of the file system's clock (SFileStat), or further writes by
 * a process that had already written to the file through a handle it still holds (NTFS stamps the
 * time at a write, measured; nothing guarantees a new stamp for each one). Atomic saves always
 * change the identity, so they are always seen. The watcher's probe for writers that still hold a
 * file and builds comparing the hash of the bytes they read with the hash in their key (§5.4) close
 * that gap where it matters.
 */
enum class EStableRead : uint8
{
	Stable,     // every byte was delivered, from one unchanged version of the file
	Changed,    // the file changed during the read: discard the result
	Failed,     // could not be read at all
};

// Default chunk size: large enough that per-call overhead vanishes, small enough to stay in cache
// for a hash that consumes it.
inline constexpr uint32 StableReadChunkBytes = 1024u * 1024u;

// OnChunk(std::span<const uint8> Bytes) receives the file's bytes in order, in chunks of at most
// ChunkBytes (only the last one shorter; an empty file delivers none). Its spans are valid during
// the call only. When the result is not Stable, everything OnChunk saw must be thrown away.
template <typename F>
EStableRead ReadFileChunksStable (const std::filesystem::path& Path, F&& OnChunk, uint32 ChunkBytes = StableReadChunkBytes)
{
	const std::optional<SFileStat> before = Stat(Path);
	if (!before.has_value() || before->bDirectory)
	{
		return EStableRead::Failed;
	}

	CBlockingFileReader reader;
	if (!reader.Open(Path))
	{
		// Stat found it a moment ago: replaced or deleted since, or held by a writer denying reads.
		const EFileFailure failure = reader.GetLastFailure();
		if (failure == EFileFailure::InUse || failure == EFileFailure::NotFound)
		{
			return EStableRead::Changed;
		}
		return Stat(Path).has_value() ? EStableRead::Failed : EStableRead::Changed;
	}
	if (reader.GetSize() != before->Size)
	{
		return EStableRead::Changed;
	}

	std::vector<uint8> buffer(static_cast<size_t>(before->Size < ChunkBytes ? before->Size : (ChunkBytes > 0u ? ChunkBytes : 1u)));
	uint64 offset = 0u;
	while (offset < before->Size)
	{
		const uint64 wanted = before->Size - offset < buffer.size() ? before->Size - offset : buffer.size();
		const std::optional<uint64> read = reader.ReadAt(offset, std::span<uint8>(buffer.data(), static_cast<size_t>(wanted)));
		if (!read.has_value())
		{
			// A writer's lock on these bytes clears; a device error does not.
			return reader.GetLastFailure() == EFileFailure::InUse ? EStableRead::Changed : EStableRead::Failed;
		}
		if (*read == 0u)
		{
			return EStableRead::Changed;   // shorter than stat said: truncated while read
		}
		OnChunk(std::span<const uint8>(buffer.data(), static_cast<size_t>(*read)));
		offset += *read;
	}
	reader.Close();

	const std::optional<SFileStat> after = Stat(Path);
	const bool bSame = after.has_value() && after->Size == before->Size && after->LastWriteTime == before->LastWriteTime
		&& after->Identity == before->Identity && !after->bDirectory;
	return bSame ? EStableRead::Stable : EStableRead::Changed;
}

template <NarrowText T, typename F>
EStableRead ReadFileChunksStable (const T&, F&&, uint32 = StableReadChunkBytes) = delete;
}
