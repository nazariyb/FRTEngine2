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

/**
 * The one way paths are compared and contained (the watcher's subscriptions and changes, the loaders
 * that ask whether a change is about one of their files, the shader root override). NormalizePath:
 * absolute (against the current directory) and lexically normal, without a trailing separator (a
 * volume root keeps its own). GetRelativePath: the names of Path below Directory, '/' separated (""
 * for Directory itself), or nullopt when Path is not at or below it - both normalized; names compare
 * as NTFS compares them (MakePathKey, Containers/StringMap.h) unless bCaseSensitive, in their own
 * spelling. Lexical only: links are not followed, and an 8.3 name is not its long name.
 */
[[nodiscard]] FRT_CORE_API std::filesystem::path NormalizePath (const std::filesystem::path& Path);
[[nodiscard]] FRT_CORE_API std::optional<FString> GetRelativePath (const std::filesystem::path& Path, const std::filesystem::path& Directory,
	bool bCaseSensitive = false);

[[nodiscard]] inline bool IsAtOrBelow (const std::filesystem::path& Path, const std::filesystem::path& Directory, bool bCaseSensitive = false)
{
	return GetRelativePath(Path, Directory, bCaseSensitive).has_value();
}

// IsAtOrBelow of the two paths as the disk resolves them, as far as each exists (std::filesystem::weakly_canonical: links and
// junctions followed, 8.3 names expanded, the names in their spelling on disk): for a decision about where a process may
// write, which no other spelling of the same directory may slip past (the scripted runs' scratch copy, Engine/Automation.h).
[[nodiscard]] FRT_CORE_API bool IsAtOrBelowResolved (const std::filesystem::path& Path, const std::filesystem::path& Directory);

template <NarrowText T>
std::filesystem::path NormalizePath (const T&) = delete;

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
 *
 * bNameSurrogate tells the links apart from every other reparse point: the entry stands for
 * another file or directory (a symbolic link, a junction or mount point, an app-execution alias),
 * so walking into it walks somewhere else - possibly up into its own ancestors. The asset scan
 * never descends into one and records nothing below it (design §5.4). Every other reparse point
 * is the entry itself with extra behaviour attached - a OneDrive placeholder, a deduplicated or
 * tiered file, a WSL special file - and is listed and walked like an ordinary entry; its size and
 * times come from the listing, so it is not opened (reading a cloud placeholder downloads it).
 * Windows: IsReparseTagNameSurrogate of the listing record's ReparsePointTag (the tag's
 * name-surrogate bit, set by Microsoft's tags for exactly the link kinds).
 */
struct SDirectoryEntry
{
	FStringView Name;
	SFileStat Stat;
	bool bReparsePoint = false;
	bool bNameSurrogate = false;   // implies bReparsePoint
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

// A whole file as bytes read past the OS file cache (Windows: FILE_FLAG_NO_BUFFERING): what reading the
// file's data costs the first time after a reboot, when nothing of it is cached (its metadata - the MFT
// record, the directories on the way - may still be). For benchmarks that report a cold column (§5.11);
// everything else reads through the cache (ReadFileBytes, Platform.h). nullopt, with the reason, if the
// file cannot be opened or read whole.
[[nodiscard]] FRT_CORE_API std::optional<std::vector<uint8>> ReadFileBytesUncached (const std::filesystem::path& Path, FString* OutError = nullptr);

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

// WriteFileAtomic with the temporary file named by the caller instead of beside Path: Temp must not
// exist yet (it is created new, so two writers never share one) and must be on Path's volume. For a
// store that keeps every unfinished write in a directory of its own (the derived-data cache's Tmp/,
// design §7.4): its directories never show a temporary name to a reader or a listing, and a sweep
// of one directory finds what crashed writers left. The same guarantees as WriteFileAtomic: Path
// holds Bytes afterwards or what it held before, and a failure deletes Temp. Creates the parent
// directories of both first when bCreateDirectories.
FRT_CORE_API bool WriteFileAtomicVia (const std::filesystem::path& Path, const std::filesystem::path& Temp, std::span<const uint8> Bytes,
	bool bCreateDirectories = true, FString* OutError = nullptr);

template <NarrowText T>
bool WriteFileBytes (const T&, std::span<const uint8>, bool = true, FString* = nullptr) = delete;
template <NarrowText T, NarrowText U>
bool ReplaceFileAtomic (const T&, const U&, FString* = nullptr) = delete;
template <NarrowText T>
bool WriteFileAtomic (const T&, std::span<const uint8>, bool = true, FString* = nullptr) = delete;
template <NarrowText T>
bool WriteFileAtomic (const T&, FStringView, bool = true, FString* = nullptr) = delete;
template <NarrowText T, NarrowText U>
bool WriteFileAtomicVia (const T&, const U&, std::span<const uint8>, bool = true, FString* = nullptr) = delete;


/**
 * Append-only files: logs whose records are only ever added at the end (the build state log, the
 * build journal, the derived-data cache's access logs; design §3.3, §7.4, §7.9). A crash leaves at
 * most a torn last record, which their readers detect and skip, so no append needs a temporary
 * file, and appending stays a single write of a few hundred bytes - not a rewrite of the log.
 *
 * Every Append lands at the end of the file in one piece, even with other processes appending to the
 * same file at the same time: their records never interleave inside one Append's bytes. Windows: the
 * file is opened with FILE_APPEND_DATA and no FILE_WRITE_DATA, so the system positions each WriteFile
 * at the end of the file under the file's lock (the documented append-only access; on local volumes,
 * which is where Local/ and the cache live, §3.1). The bytes are in the OS cache when Append returns:
 * they survive the process being killed, and reach the disk with Flush or in the background.
 *
 * The file is shared for reading, writing and deleting, so readers of the log (CBlockingFileReader)
 * and an atomic replace of it (a compaction) are never refused while it is open; after a replace,
 * Close and Open again to append to the new file. Not meant to be shared between threads without a
 * lock of the caller's. No engine allocator.
 */
class FRT_CORE_API CAppendFile
{
public:
	CAppendFile () noexcept = default;
	~CAppendFile ();

	CAppendFile (const CAppendFile&) = delete;
	CAppendFile& operator= (const CAppendFile&) = delete;
	CAppendFile (CAppendFile&& Other) noexcept;
	CAppendFile& operator= (CAppendFile&& Other) noexcept;

	// Opens Path for appending, creating it empty if it does not exist (and its parent directories
	// first when bCreateDirectories); closes whatever was open. False, with the reason, if it cannot.
	bool Open (const std::filesystem::path& Path, bool bCreateDirectories = true, FString* OutError = nullptr);
	template <NarrowText T>
	bool Open (const T&, bool = true, FString* = nullptr) = delete;

	void Close () noexcept;
	[[nodiscard]] bool IsOpen () const noexcept { return Handle != nullptr; }

	// Writes Bytes at the end of the file, in one write (see above). False when the write failed or
	// was short (a full disk): then part of Bytes may be in the file - a torn record for the reader.
	bool Append (std::span<const uint8> Bytes, FString* OutError = nullptr);
	// The file's size now, appends of other processes included. nullopt when not open or on failure.
	[[nodiscard]] std::optional<uint64> GetSize (FString* OutError = nullptr) const;
	// Everything appended so far is on disk when it returns true (FlushFileBuffers).
	bool Flush (FString* OutError = nullptr);

private:
	void* Handle = nullptr;   // the OS file handle; nullptr when closed
	std::filesystem::path Path;   // for error messages
};


// Deletes the file at Path for good - no Recycle Bin: for files of FRT's own caches and logs, which
// are rebuilt rather than restored (a corrupt cache file, a sweep's leftovers). True when no file is
// at Path afterwards, also when there was none. Never deletes a directory, and fails on a read-only
// file. Readers that have the file open keep reading it; its name is gone at once.
//
// Windows: SetFileInformationByHandle(FileDispositionInfoEx, FILE_DISPOSITION_FLAG_DELETE |
// FILE_DISPOSITION_FLAG_POSIX_SEMANTICS), so the name goes away immediately even while other handles
// hold the file open with FILE_SHARE_DELETE (as CBlockingFileReader does), and a writer may create a
// new file under the same name right after; FileDispositionInfo (delete on last close) where the file
// system lacks POSIX semantics.
FRT_CORE_API bool RemoveFile (const std::filesystem::path& Path, FString* OutError = nullptr);

template <NarrowText T>
bool RemoveFile (const T&, FString* = nullptr) = delete;


// Sets the last write time of the file at Path to now; its bytes and identity stay. For FRT's own
// caches, whose collectors judge a file by its write time: the derived-data cache's writer reuses a
// content-addressed value instead of writing it again, and marks it as just written so a collection
// running in another process does not delete it as an old leftover. False, with the reason, when
// there is no file at Path (a directory is not one) or its attributes cannot be written.
//
// Windows: CreateFileW with FILE_WRITE_ATTRIBUTES only, sharing everything, so no reader, writer or
// deleter of the file refuses it; then SetFileTime with the system time.
FRT_CORE_API bool TouchFile (const std::filesystem::path& Path, FString* OutError = nullptr);

template <NarrowText T>
bool TouchFile (const T&, FString* = nullptr) = delete;


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
