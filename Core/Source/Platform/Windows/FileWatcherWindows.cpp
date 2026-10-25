#include "Platform/FileWatcher.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Platform/Thread.h"
#include "Platform/Timestamp.h"
#include "Platform/Windows/WindowsCommon.h"


/**
 * The Windows IFileWatcher (design §5.4, §8.15; measured facts in Local/Design/AssetManager/Research/
 * gap-watch-meta.md §1): ReadDirectoryChangesExW with ReadDirectoryNotifyExtendedInformation on every
 * root, all of them completing to one I/O completion port that one thread waits on.
 *
 * Why this shape:
 *	- One thread, never the caller's: a reader that stalls overflows the kernel's buffer (measured: a
 *	  2 KB buffer kept up with 2000 creates in 450 ms when its reader never blocked, and lost
 *	  everything when it stalled for 1.5 s). The thread re-arms, parses and queues, nothing else, and
 *	  the queue is behind a mutex held for a vector append. A completion port instead of completion
 *	  routines (UE's model): no alertable waits, so nothing couples it to the game thread.
 *	- The extended information carries the file id, so the two halves of a move pair by identity
 *	  instead of by adjacency (Raymond Chen, "Developing more confidence when tracking renames via
 *	  ReadDirectoryChangesW", 2026), and the attributes, so a directory is known as one (UE's proxy
 *	  guesses from the extension). File systems without it (FAT, and ReFS per the documentation) fall
 *	  back to ReadDirectoryChangesW: no identities, no entry types.
 *	- Two buffers per root: the next read is issued into one before the other is parsed, so the
 *	  kernel always has a request to complete into (UE, parcel and Beveridge's sample do the same).
 *
 * The OS's quirks mapped here onto the portable kinds of FileWatcher.h:
 *	- An overflow is a successful completion with 0 bytes (measured), or ERROR_NOTIFY_ENUM_DIR as the
 *	  documentation says: Overflow, after re-arming.
 *	- A case-only rename is REMOVED a, RENAMED_OLD a, RENAMED_NEW A, one id (S16, G3, G4): the REMOVED
 *	  is dropped, also when it ends one read and its RENAMED_OLD starts the next (held for 30 ms).
 *	- MODIFIED of a directory is the directory's own times changing because an entry inside changed
 *	  (the "dir MOD" of the measurements): its children report themselves, so it is dropped.
 *	- ERROR_ACCESS_DENIED (the root was deleted, measured) or any other error: RootLost. A renamed root
 *	  is silent - the handle follows the directory, and events keep arriving with names relative to
 *	  its new place - so every RootPollMs the root's final path (GetFinalPathNameByHandleW) is
 *	  compared with the one it was armed at; a difference is RootLost too. A lost root's path is
 *	  looked at every RootPollMs, and once it is a directory again it is armed again and reported as
 *	  Overflow, since everything in between was missed.
 *	- A name may come in its 8.3 short form ("unspecified which", the documentation says; never seen
 *	  when watching through the long path): expanded with GetLongPathNameW where that still works.
 */
namespace frt::platform
{
namespace
{
using windows::CScopedHandle;

// Completions of reads carry their root's id as the key (ids start at 1); commands carry 0.
constexpr ULONG_PTR CommandKey = 0u;

// Names, directory names, size, last write and creation (gap-watch-meta §1.1): not attributes or
// last access, which are noise for assets.
constexpr DWORD NotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
	| FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;

// The largest buffer a network share accepts.
constexpr DWORD NetworkBufferBytes = 64u * 1024u;
constexpr DWORD MinBufferBytes = 64u;

// How long a REMOVED that ends a read waits for the RENAMED_OLD of a case-only rename to begin the next.
constexpr DWORD HeldRemovedMs = 30u;

// How long the thread waits at shutdown for the cancelled reads to complete (their buffers must not
// be freed before).
constexpr DWORD ShutdownDrainMs = 5000u;


SFileIdentity MakeIdentity (uint64 VolumeSerial, uint64 FileId) noexcept
{
	// As Stat builds it (FileSystemWindows.cpp): the 64-bit volume serial, then the 128-bit FILE_ID_INFO
	// id - on NTFS the 64-bit file reference number, zero-extended, which is what an event carries.
	SFileIdentity identity;
	std::memcpy(identity.Bytes, &VolumeSerial, sizeof(VolumeSerial));
	std::memcpy(identity.Bytes + sizeof(VolumeSerial), &FileId, sizeof(FileId));
	return identity;
}

// The volume serial number that goes into identities of entries under Directory: FILE_ID_INFO's,
// else BY_HANDLE_FILE_INFORMATION's, as Stat takes it.
uint64 QueryVolumeSerial (HANDLE Directory, const BY_HANDLE_FILE_INFORMATION& Info) noexcept
{
	FILE_ID_INFO idInfo = {};
	if (GetFileInformationByHandleEx(Directory, FileIdInfo, &idInfo, sizeof(idInfo)))
	{
		return idInfo.VolumeSerialNumber;
	}
	return Info.dwVolumeSerialNumber;
}

// What a Win32 call that fills a path buffer returns (GetFinalPathNameByHandleW, GetLongPathNameW:
// the length without the terminator, or the size needed with it when the buffer is too small, or 0 on
// failure) as a path; empty on failure.
template <typename F>
std::filesystem::path QueryPathText (F&& Call)
{
	std::vector<wchar_t> buffer(MAX_PATH);
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const DWORD length = Call(buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0u)
		{
			return {};
		}
		if (length < buffer.size())
		{
			std::filesystem::path::string_type units(buffer.data(), length);   // UTF-16 from the OS
			return std::filesystem::path(std::move(units));
		}
		buffer.resize(static_cast<size_t>(length) + 1u);
	}
	return {};
}

std::filesystem::path QueryFinalPath (HANDLE Handle)
{
	return QueryPathText([Handle] (wchar_t* Buffer, DWORD Size)
	{
		return GetFinalPathNameByHandleW(Handle, Buffer, Size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	});
}

std::filesystem::path GetLongPath (const std::filesystem::path& Path)
{
	return QueryPathText([&Path] (wchar_t* Buffer, DWORD Size) { return GetLongPathNameW(Path.c_str(), Buffer, Size); });
}

// The first Units units of A and B are equal, ignoring case as NTFS does (ordinal, upcase table).
bool EqualsIgnoreCase (const wchar_t* A, const wchar_t* B, size_t Units) noexcept
{
	return CompareStringOrdinal(A, static_cast<int>(Units), B, static_cast<int>(Units), TRUE) == CSTR_EQUAL;
}

bool EqualsIgnoreCase (const std::filesystem::path& A, const std::filesystem::path& B) noexcept
{
	return A.native().size() == B.native().size() && EqualsIgnoreCase(A.c_str(), B.c_str(), A.native().size());
}

// A name component that may be an 8.3 short name: '~' in at most 12 units ("PROGRA~1.TXT").
bool HasShortNameComponent (const wchar_t* Name, size_t Units) noexcept
{
	size_t start = 0u;
	for (size_t i = 0u; i <= Units; ++i)
	{
		if (i == Units || Name[i] == L'\\')
		{
			const size_t length = i - start;
			if (length > 0u && length <= 12u)
			{
				for (size_t j = start; j < i; ++j)
				{
					if (Name[j] == L'~')
					{
						return true;
					}
				}
			}
			start = i + 1u;
		}
	}
	return false;
}

bool IsUnsupported (DWORD Error) noexcept
{
	return Error == ERROR_INVALID_FUNCTION || Error == ERROR_NOT_SUPPORTED || Error == ERROR_INVALID_PARAMETER
		|| Error == ERROR_INVALID_LEVEL || Error == ERROR_CALL_NOT_IMPLEMENTED;
}


class CFileWatcherWindows final : public IFileWatcher
{
public:
	explicit CFileWatcherWindows (const SFileWatcherDesc& InDesc)
		: Desc(InDesc)
	{
		Desc.BufferBytes = (Desc.BufferBytes < MinBufferBytes ? MinBufferBytes : Desc.BufferBytes + 7u) & ~7u;
		Desc.RootPollMs = Desc.RootPollMs == 0u ? 1u : Desc.RootPollMs;
		Desc.MaxQueuedEvents = Desc.MaxQueuedEvents == 0u ? 1u : Desc.MaxQueuedEvents;
		Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0u, 1u);
		if (Port != nullptr)
		{
			Thread = std::thread([this] { ThreadMain(); });
		}
		else
		{
			PortError = GetLastError();
		}
	}

	~CFileWatcherWindows () override
	{
		if (Port == nullptr)
		{
			return;
		}
		SCommand quit;
		quit.Type = ECommand::Quit;
		Run(quit);
		Thread.join();
		CloseHandle(Port);
	}

	CFileWatcherWindows (const CFileWatcherWindows&) = delete;
	CFileWatcherWindows& operator= (const CFileWatcherWindows&) = delete;

	uint32 Arm (const std::filesystem::path& Root, FString* OutError) override
	{
		if (Port == nullptr)
		{
			windows::SetError(OutError, "Cannot watch (no I/O completion port)", Root, PortError);
			return 0u;
		}
		std::error_code ec;
		SCommand arm;
		arm.Type = ECommand::Arm;
		arm.Path = std::filesystem::absolute(Root, ec);
		if (ec)
		{
			arm.Path = Root;
		}
		arm.RootId = NextRootId.fetch_add(1u, std::memory_order_relaxed);
		Run(arm);
		if (!arm.bSucceeded)
		{
			if (OutError != nullptr)
			{
				*OutError = std::move(arm.Error);
			}
			return 0u;
		}
		return arm.RootId;
	}

	void Disarm (uint32 RootId) override
	{
		if (RootId == 0u || Port == nullptr)
		{
			return;
		}
		SCommand disarm;
		disarm.Type = ECommand::Disarm;
		disarm.RootId = RootId;
		Run(disarm);

		// The thread queues nothing more for it: what is queued already goes too.
		const std::lock_guard lock(QueueMutex);
		std::erase_if(Queue, [RootId] (const SFileEvent& Event) { return Event.RootId == RootId; });
	}

	void TakeEvents (std::vector<SFileEvent>& Out) override
	{
		std::vector<SFileEvent> taken;
		{
			const std::lock_guard lock(QueueMutex);
			taken.swap(Queue);
		}
		if (Out.empty())
		{
			Out = std::move(taken);
			return;
		}
		Out.reserve(Out.size() + taken.size());
		for (SFileEvent& event : taken)
		{
			Out.push_back(std::move(event));
		}
	}

	bool WaitForEvents (double TimeoutSeconds) override
	{
		std::unique_lock lock(QueueMutex);
		const auto ready = [this] { return !Queue.empty(); };
		if (TimeoutSeconds < 0.0)
		{
			QueueSignal.wait(lock, ready);
			return true;
		}
		return QueueSignal.wait_for(lock, std::chrono::duration<double>(TimeoutSeconds), ready);
	}

private:
	enum class ECommand : uint8
	{
		Arm,
		Disarm,
		Quit,
	};

	// A request from a caller, run on the watcher thread (all reads are issued there), which the
	// caller waits for. Lives on the caller's stack.
	struct SCommand
	{
		ECommand Type = ECommand::Quit;
		std::filesystem::path Path;
		uint32 RootId = 0u;
		FString Error;
		bool bSucceeded = false;
		bool bDone = false;
	};

	struct SRoot
	{
		uint32 Id = 0u;
		std::filesystem::path Path;    // absolute, as armed
		std::filesystem::path FinalPath;    // GetFinalPathNameByHandleW when it was (re)opened
		std::filesystem::path LongPrefix;   // GetLongPathNameW of the root and a '\\': for 8.3 names
		std::optional<CScopedHandle> Directory;   // empty while lost (CScopedHandle does not move)
		uint64 VolumeSerial = 0u;
		OVERLAPPED Overlapped = {};    // the pending read's; SRoot lives in a unique_ptr, so it never moves
		std::vector<uint64> Buffers[2];   // 8-byte aligned (the records hold LARGE_INTEGERs)
		uint32 Active = 0u;            // the buffer the pending read fills
		DWORD BufferBytes = 0u;
		bool bExtended = true;         // ReadDirectoryChangesExW works on this volume
		bool bReadPending = false;
		bool bLost = false;
		bool bDisarmed = false;        // freed when its cancelled read completes
		std::optional<SFileEvent> HeldRemoved;
		uint64 HeldSince = 0u;
	};

	// ---- caller side ----

	void Run (SCommand& Command)
	{
		{
			const std::lock_guard lock(CommandMutex);
			Commands.push_back(&Command);
		}
		PostQueuedCompletionStatus(Port, 0u, CommandKey, nullptr);
		std::unique_lock lock(CommandMutex);
		CommandDone.wait(lock, [&Command] { return Command.bDone; });
	}

	// ---- watcher thread ----

	void ThreadMain ()
	{
		ConfigureThread(Desc.ThreadName, EThreadPriority::AboveNormal);
		const uint64 frequency = GetTimestampFrequency();
		const uint64 pollTicks = frequency * Desc.RootPollMs / 1000u;
		const uint64 heldTicks = frequency * HeldRemovedMs / 1000u;
		uint64 nextPoll = ReadTimestamp() + pollTicks;

		bool bQuit = false;
		while (!bQuit)
		{
			const uint64 now = ReadTimestamp();
			uint64 wakeAt = nextPoll;
			for (const auto& [id, root] : Roots)
			{
				if (root->HeldRemoved && root->HeldSince + heldTicks < wakeAt)
				{
					wakeAt = root->HeldSince + heldTicks;
				}
			}
			const DWORD timeoutMs = wakeAt <= now ? 0u : static_cast<DWORD>((wakeAt - now) * 1000u / frequency + 1u);

			DWORD bytes = 0u;
			ULONG_PTR key = 0u;
			OVERLAPPED* overlapped = nullptr;
			const BOOL bOk = GetQueuedCompletionStatus(Port, &bytes, &key, &overlapped, timeoutMs);
			const DWORD error = bOk ? static_cast<DWORD>(ERROR_SUCCESS) : GetLastError();
			if (overlapped != nullptr)
			{
				OnReadCompleted(static_cast<uint32>(key), error, bytes);
			}
			else if (bOk && key == CommandKey)
			{
				bQuit = RunCommands();
			}

			const uint64 after = ReadTimestamp();
			FlushHeldRemoved(after, heldTicks);
			if (after >= nextPoll)
			{
				PollRoots();
				nextPoll = after + pollTicks;
			}
		}
		Shutdown();
	}

	// Runs the queued commands; true after Quit.
	bool RunCommands ()
	{
		std::vector<SCommand*> commands;
		{
			const std::lock_guard lock(CommandMutex);
			commands.swap(Commands);
		}
		bool bQuit = false;
		for (SCommand* command : commands)
		{
			switch (command->Type)
			{
			case ECommand::Arm:
				command->bSucceeded = ArmRoot(command->Path, command->RootId, command->Error);
				break;
			case ECommand::Disarm:
				DisarmRoot(command->RootId);
				command->bSucceeded = true;
				break;
			case ECommand::Quit:
				bQuit = true;
				command->bSucceeded = true;
				break;
			}
		}
		{
			const std::lock_guard lock(CommandMutex);
			for (SCommand* command : commands)
			{
				command->bDone = true;
			}
		}
		CommandDone.notify_all();
		return bQuit;
	}

	bool ArmRoot (const std::filesystem::path& Path, uint32 Id, FString& OutError)
	{
		auto root = std::make_unique<SRoot>();
		root->Id = Id;
		root->Path = Path;
		root->BufferBytes = Desc.BufferBytes;
		const DWORD error = Open(*root, &OutError);
		if (error != ERROR_SUCCESS)
		{
			return false;
		}
		Roots.emplace(Id, std::move(root));
		return true;
	}

	// Opens the root's directory, associates it with the port and issues the first read. On failure the
	// handle is closed again and the error returned (with OutError, when given).
	DWORD Open (SRoot& Root, FString* OutError)
	{
		const std::filesystem::path win32 = windows::ToWin32Path(Root.Path);
		CScopedHandle directory(CreateFileW(win32.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			nullptr));
		if (!directory.IsValid())
		{
			const DWORD error = GetLastError();
			windows::SetError(OutError, "Cannot watch", Root.Path, error);
			return error;
		}
		BY_HANDLE_FILE_INFORMATION info = {};
		if (!GetFileInformationByHandle(directory.Get(), &info))
		{
			const DWORD error = GetLastError();
			windows::SetError(OutError, "Cannot watch", Root.Path, error);
			return error;
		}
		if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u)
		{
			windows::SetErrorText(OutError, "Cannot watch", Root.Path, "not a directory");
			return ERROR_DIRECTORY;
		}
		if (CreateIoCompletionPort(directory.Get(), Port, Root.Id, 0u) == nullptr)
		{
			const DWORD error = GetLastError();
			windows::SetError(OutError, "Cannot watch", Root.Path, error);
			return error;
		}

		Root.VolumeSerial = QueryVolumeSerial(directory.Get(), info);
		Root.FinalPath = QueryFinalPath(directory.Get());
		Root.LongPrefix = GetLongPath(win32);
		if (!Root.LongPrefix.empty() && Root.LongPrefix.native().back() != L'\\')
		{
			Root.LongPrefix += L'\\';
		}
		Root.Directory.emplace(directory.Release());
		Root.bLost = false;
		Root.bExtended = true;
		Root.Active = 0u;

		DWORD error = IssueRead(Root);
		if (error != ERROR_SUCCESS && IsUnsupported(error))
		{
			// No extended information here (not NTFS): names only.
			Root.bExtended = false;
			error = IssueRead(Root);
		}
		if (error == ERROR_INVALID_PARAMETER && Root.BufferBytes > NetworkBufferBytes)
		{
			// A network share refuses reads over 64 KB.
			Root.BufferBytes = NetworkBufferBytes;
			error = IssueRead(Root);
		}
		if (error != ERROR_SUCCESS)
		{
			Root.Directory.reset();
			windows::SetError(OutError, "Cannot watch", Root.Path, error);
			return error;
		}
		return ERROR_SUCCESS;
	}

	DWORD IssueRead (SRoot& Root)
	{
		std::vector<uint64>& buffer = Root.Buffers[Root.Active];
		if (buffer.size() * sizeof(uint64) != Root.BufferBytes)
		{
			buffer.assign(Root.BufferBytes / sizeof(uint64), 0u);
		}
		Root.Overlapped = {};
		const BOOL bQueued = Root.bExtended
			? ReadDirectoryChangesExW(Root.Directory->Get(), buffer.data(), Root.BufferBytes, TRUE, NotifyFilter, nullptr, &Root.Overlapped,
				nullptr, ReadDirectoryNotifyExtendedInformation)
			: ReadDirectoryChangesW(Root.Directory->Get(), buffer.data(), Root.BufferBytes, TRUE, NotifyFilter, nullptr, &Root.Overlapped,
				nullptr);
		if (!bQueued)
		{
			return GetLastError();
		}
		Root.bReadPending = true;
		return ERROR_SUCCESS;
	}

	void DisarmRoot (uint32 Id)
	{
		const auto it = Roots.find(Id);
		if (it == Roots.end())
		{
			return;
		}
		SRoot& root = *it->second;
		root.HeldRemoved.reset();
		if (root.bReadPending)
		{
			// Freed when the cancelled read completes: the kernel still owns its buffer.
			root.bDisarmed = true;
			CancelIoEx(root.Directory->Get(), &root.Overlapped);
			return;
		}
		Roots.erase(it);
	}

	void OnReadCompleted (uint32 Id, DWORD Error, DWORD Bytes)
	{
		const auto it = Roots.find(Id);
		if (it == Roots.end())
		{
			return;
		}
		SRoot& root = *it->second;
		root.bReadPending = false;
		if (root.bDisarmed)
		{
			Roots.erase(it);
			return;
		}
		if (root.bLost)
		{
			// The read LoseRoot cancelled: now the handle can go.
			root.Directory.reset();
			return;
		}

		if (Error == ERROR_SUCCESS || Error == ERROR_NOTIFY_ENUM_DIR)
		{
			// Re-arm first, into the other buffer, then parse the one that completed.
			const uint32 filled = root.Active;
			root.Active ^= 1u;
			const DWORD rearmError = IssueRead(root);

			std::vector<SFileEvent> events;
			if (Error == ERROR_NOTIFY_ENUM_DIR || Bytes == 0u)
			{
				// The kernel's buffer overflowed and was discarded.
				TakeHeldRemoved(root, events);
				events.push_back(MakeRootEvent(root, EFileEventKind::Overflow));
			}
			else
			{
				Parse(root, root.Buffers[filled], Bytes, events);
			}
			Push(events);

			if (rearmError != ERROR_SUCCESS)
			{
				LoseRoot(root);
			}
			return;
		}
		LoseRoot(root);
	}

	SFileEvent MakeRootEvent (const SRoot& Root, EFileEventKind Kind) const
	{
		SFileEvent event;
		event.RootId = Root.Id;
		event.Kind = Kind;
		event.Timestamp = ReadTimestamp();
		return event;
	}

	// The root is gone or no longer where it was armed: no more reads until PollRoots finds it again.
	void LoseRoot (SRoot& Root)
	{
		std::vector<SFileEvent> events;
		TakeHeldRemoved(Root, events);
		events.push_back(MakeRootEvent(Root, EFileEventKind::RootLost));
		Push(events);

		Root.bLost = true;
		if (Root.bReadPending)
		{
			CancelIoEx(Root.Directory->Get(), &Root.Overlapped);   // the handle closes at its completion
		}
		else
		{
			Root.Directory.reset();
		}
	}

	void PollRoots ()
	{
		for (auto& [id, rootPtr] : Roots)
		{
			SRoot& root = *rootPtr;
			if (root.bDisarmed)
			{
				continue;
			}
			if (root.bLost)
			{
				if (!root.bReadPending && Open(root, nullptr) == ERROR_SUCCESS)
				{
					std::vector<SFileEvent> events;
					events.push_back(MakeRootEvent(root, EFileEventKind::Overflow));
					Push(events);
				}
				continue;
			}
			if (root.Directory.has_value() && !root.FinalPath.empty())
			{
				// A renamed root (or a renamed directory above it) is silent otherwise.
				const std::filesystem::path finalPath = QueryFinalPath(root.Directory->Get());
				if (finalPath.empty() || !EqualsIgnoreCase(finalPath, root.FinalPath))
				{
					LoseRoot(root);
				}
			}
		}
	}

	void TakeHeldRemoved (SRoot& Root, std::vector<SFileEvent>& Out)
	{
		if (Root.HeldRemoved)
		{
			Out.push_back(std::move(*Root.HeldRemoved));
			Root.HeldRemoved.reset();
		}
	}

	void FlushHeldRemoved (uint64 Now, uint64 HeldTicks)
	{
		std::vector<SFileEvent> events;
		for (auto& [id, root] : Roots)
		{
			if (root->HeldRemoved && Now >= root->HeldSince + HeldTicks)
			{
				TakeHeldRemoved(*root, events);
			}
		}
		Push(events);
	}

	// The record's name, relative to the root, '/' separated, its 8.3 components expanded where the file
	// still exists.
	FString MakeRelativePath (const SRoot& Root, const wchar_t* Name, size_t Units) const
	{
		if (HasShortNameComponent(Name, Units) && !Root.LongPrefix.empty())
		{
			const std::filesystem::path full = windows::ToWin32Path(Root.Path / std::filesystem::path(Name, Name + Units));
			const std::filesystem::path longPath = GetLongPath(full);
			const size_t prefixUnits = Root.LongPrefix.native().size();
			if (longPath.native().size() > prefixUnits && EqualsIgnoreCase(longPath.c_str(), Root.LongPrefix.c_str(), prefixUnits))
			{
				FString path = FString::FromWide(longPath.c_str() + prefixUnits, longPath.native().size() - prefixUnits);
				path.ReplaceCharInline('\\', '/');
				return path;
			}
		}
		FString path = FString::FromWide(Name, Units);
		path.ReplaceCharInline('\\', '/');
		return path;
	}

	static std::optional<EFileEventKind> ToKind (DWORD Action) noexcept
	{
		switch (Action)
		{
		case FILE_ACTION_ADDED: return EFileEventKind::Added;
		case FILE_ACTION_REMOVED: return EFileEventKind::Removed;
		case FILE_ACTION_MODIFIED: return EFileEventKind::Modified;
		case FILE_ACTION_RENAMED_OLD_NAME: return EFileEventKind::RenamedOld;
		case FILE_ACTION_RENAMED_NEW_NAME: return EFileEventKind::RenamedNew;
		default: return std::nullopt;
		}
	}

	void Parse (SRoot& Root, const std::vector<uint64>& Buffer, DWORD Bytes, std::vector<SFileEvent>& Out)
	{
		const uint64 now = ReadTimestamp();
		const auto* base = reinterpret_cast<const uint8*>(Buffer.data());
		const size_t size = Bytes < Root.BufferBytes ? Bytes : Root.BufferBytes;

		std::vector<SFileEvent> events;
		TakeHeldRemoved(Root, events);
		size_t offset = 0u;
		while (true)
		{
			if (Root.bExtended)
			{
				if (offset + sizeof(FILE_NOTIFY_EXTENDED_INFORMATION) > size)
				{
					break;
				}
				const auto* info = reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION*>(base + offset);
				const bool bDirectory = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
				const std::optional<EFileEventKind> kind = ToKind(info->Action);
				// A directory's MODIFIED is its own times, changed by an entry inside: noise.
				if (kind && !(bDirectory && *kind == EFileEventKind::Modified))
				{
					SFileEvent event;
					event.RootId = Root.Id;
					event.Kind = *kind;
					event.EntryType = bDirectory ? EFileEntryType::Directory : EFileEntryType::File;
					event.Path = MakeRelativePath(Root, info->FileName, info->FileNameLength / sizeof(wchar_t));
					event.Identity = MakeIdentity(Root.VolumeSerial, static_cast<uint64>(info->FileId.QuadPart));
					event.Timestamp = now;
					events.push_back(std::move(event));
				}
				if (info->NextEntryOffset == 0u)
				{
					break;
				}
				offset += info->NextEntryOffset;
			}
			else
			{
				if (offset + sizeof(FILE_NOTIFY_INFORMATION) > size)
				{
					break;
				}
				const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
				if (const std::optional<EFileEventKind> kind = ToKind(info->Action))
				{
					SFileEvent event;
					event.RootId = Root.Id;
					event.Kind = *kind;
					event.Path = MakeRelativePath(Root, info->FileName, info->FileNameLength / sizeof(wchar_t));
					event.Timestamp = now;
					events.push_back(std::move(event));
				}
				if (info->NextEntryOffset == 0u)
				{
					break;
				}
				offset += info->NextEntryOffset;
			}
		}

		// The case-only rename's leading REMOVED (S16, G3, G4): same name and identity as the RENAMED_OLD
		// right after it. One that ends this read waits briefly for the next.
		for (size_t i = 0u; i < events.size(); ++i)
		{
			SFileEvent& event = events[i];
			if (event.Kind == EFileEventKind::Removed)
			{
				if (i + 1u < events.size())
				{
					const SFileEvent& next = events[i + 1u];
					if (next.Kind == EFileEventKind::RenamedOld && next.Path == event.Path && next.Identity == event.Identity)
					{
						continue;
					}
				}
				else
				{
					Root.HeldRemoved = std::move(event);
					Root.HeldSince = now;
					continue;
				}
			}
			Out.push_back(std::move(event));
		}
	}

	// Queues Events for the owner (the one place the thread takes the queue's lock).
	void Push (std::vector<SFileEvent>& Events)
	{
		if (Events.empty())
		{
			return;
		}
		{
			const std::lock_guard lock(QueueMutex);
			if (Queue.size() + Events.size() > Desc.MaxQueuedEvents)
			{
				// Nobody takes them: drop them all, and say so once per root, so the owner rescans.
				std::unordered_set<uint32> roots;
				for (const SFileEvent& event : Queue)
				{
					roots.insert(event.RootId);
				}
				for (const SFileEvent& event : Events)
				{
					roots.insert(event.RootId);
				}
				Queue.clear();
				const uint64 now = ReadTimestamp();
				for (const uint32 id : roots)
				{
					SFileEvent overflow;
					overflow.RootId = id;
					overflow.Kind = EFileEventKind::Overflow;
					overflow.Timestamp = now;
					Queue.push_back(std::move(overflow));
				}
			}
			else
			{
				for (SFileEvent& event : Events)
				{
					Queue.push_back(std::move(event));
				}
			}
		}
		Events.clear();
		QueueSignal.notify_all();
	}

	void Shutdown ()
	{
		// Cancel every read, then wait until each has completed: the kernel writes into the buffers
		// until then.
		for (auto& [id, root] : Roots)
		{
			if (root->bReadPending)
			{
				CancelIoEx(root->Directory->Get(), &root->Overlapped);
			}
		}
		const uint64 frequency = GetTimestampFrequency();
		const uint64 deadline = ReadTimestamp() + frequency * ShutdownDrainMs / 1000u;
		while (true)
		{
			bool bPending = false;
			for (const auto& [id, root] : Roots)
			{
				bPending = bPending || root->bReadPending;
			}
			const uint64 now = ReadTimestamp();
			if (!bPending || now >= deadline)
			{
				break;
			}
			DWORD bytes = 0u;
			ULONG_PTR key = 0u;
			OVERLAPPED* overlapped = nullptr;
			GetQueuedCompletionStatus(Port, &bytes, &key, &overlapped, static_cast<DWORD>((deadline - now) * 1000u / frequency + 1u));
			if (overlapped != nullptr)
			{
				const auto it = Roots.find(static_cast<uint32>(key));
				if (it != Roots.end())
				{
					it->second->bReadPending = false;
				}
			}
		}
		for (auto& [id, root] : Roots)
		{
			if (root->bReadPending)
			{
				// Never completed: leak the buffers rather than let the kernel write into freed memory.
				(void)root.release();
			}
		}
		Roots.clear();
	}

	SFileWatcherDesc Desc;
	HANDLE Port = nullptr;
	DWORD PortError = ERROR_SUCCESS;
	std::thread Thread;
	std::atomic<uint32> NextRootId = 1u;

	std::mutex CommandMutex;
	std::condition_variable CommandDone;
	std::vector<SCommand*> Commands;

	std::mutex QueueMutex;
	std::condition_variable QueueSignal;
	std::vector<SFileEvent> Queue;

	// The watcher thread's alone.
	std::unordered_map<uint32, std::unique_ptr<SRoot>> Roots;
};
}


std::unique_ptr<IFileWatcher> CreateFileWatcher (const SFileWatcherDesc& Desc)
{
	return std::make_unique<CFileWatcherWindows>(Desc);
}

EWriterProbe ProbeFileWriters (const std::filesystem::path& Path) noexcept
{
	// Reading, sharing reading and deleting but not writing: refused while any handle with write
	// access is open, or one that denies reading. Closed at once.
	const CScopedHandle file(CreateFileW(windows::ToWin32Path(Path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (file.IsValid())
	{
		return EWriterProbe::Free;
	}
	switch (GetLastError())
	{
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EWriterProbe::Held;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return EWriterProbe::Missing;
	default:
		return EWriterProbe::Failed;
	}
}
}
