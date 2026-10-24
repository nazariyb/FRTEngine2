#include "Platform/FileSystem.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <system_error>
#include <vector>

#include "Containers/WideString.h"
#include "Platform/Windows/WindowsCommon.h"


namespace frt::platform
{
namespace
{
using windows::CScopedHandle;

// One directory listing call: room for hundreds of entries, and no larger than 64 KB, beyond which
// directory queries fail on some network shares.
constexpr DWORD DirectoryBufferBytes = 64u * 1024u;

// The most one WriteFile call is given: its count is a DWORD.
constexpr uint64 MaxWritePerCall = 1ull << 30u;

// ReplaceFileAtomic's retry on a target held open by someone else: sleeps doubling from 1 ms,
// at most 100 ms each, until about a second has passed.
constexpr DWORD ReplaceRetryBudgetMs = 1000u;
constexpr DWORD ReplaceRetryMaxSleepMs = 100u;

// UTF-16 units of the target's name kept in a temporary file's name: enough to recognise it, short
// enough that the decorations still fit the 255-unit component limit.
constexpr size_t TempNameUnits = 64u;

std::atomic<uint32> gTempFileCounter = 0u;


// ---------------- Identity ----------------

SFileIdentity MakeIdentity (uint64 VolumeSerial, const uint8 (&FileId)[16]) noexcept
{
	SFileIdentity identity;
	std::memcpy(identity.Bytes, &VolumeSerial, sizeof(VolumeSerial));
	std::memcpy(identity.Bytes + sizeof(VolumeSerial), FileId, sizeof(FileId));
	return identity;
}

// The volume serial number and file id that go into identities: FILE_ID_INFO's 64-bit serial and
// 128-bit id, else the 32-bit serial and 64-bit index every file system reports in
// BY_HANDLE_FILE_INFORMATION (for one without FILE_ID_INFO). Stat and EnumerateDirectory both take
// the serial from here, so their identities agree.
struct SHandleIds
{
	uint64 VolumeSerial = 0u;
	uint8 FileId[16] = {};
};

SHandleIds QueryHandleIds (HANDLE Handle, const BY_HANDLE_FILE_INFORMATION& Info) noexcept
{
	SHandleIds ids;
	FILE_ID_INFO idInfo = {};
	if (GetFileInformationByHandleEx(Handle, FileIdInfo, &idInfo, sizeof(idInfo)))
	{
		static_assert(sizeof(idInfo.FileId.Identifier) == sizeof(ids.FileId));
		ids.VolumeSerial = idInfo.VolumeSerialNumber;
		std::memcpy(ids.FileId, idInfo.FileId.Identifier, sizeof(ids.FileId));
		return ids;
	}

	// The 64-bit file index, zero-extended: what FILE_ID_INFO holds on NTFS too.
	const uint64 index = (static_cast<uint64>(Info.nFileIndexHigh) << 32u) | Info.nFileIndexLow;
	ids.VolumeSerial = Info.dwVolumeSerialNumber;
	std::memcpy(ids.FileId, &index, sizeof(index));
	return ids;
}

// Opens Path for its attributes only: no read, write or delete access, so no share mode of another
// process can refuse it. Directories need FILE_FLAG_BACKUP_SEMANTICS; links are followed.
HANDLE OpenForAttributes (const std::filesystem::path& Path) noexcept
{
	const std::filesystem::path win32 = windows::ToWin32Path(Path);
	return CreateFileW(win32.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

std::optional<SFileStat> StatHandle (HANDLE Handle, const std::filesystem::path& Path, FString* OutError)
{
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(Handle, &info))
	{
		windows::SetError(OutError, "Cannot stat", Path, GetLastError());
		return std::nullopt;
	}
	const SHandleIds ids = QueryHandleIds(Handle, info);

	SFileStat stat;
	stat.bDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
	stat.bReadOnly = (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0u;
	stat.Size = stat.bDirectory ? 0u : (static_cast<uint64>(info.nFileSizeHigh) << 32u) | info.nFileSizeLow;
	stat.LastWriteTime = windows::ToFileTime(info.ftLastWriteTime);
	stat.CreationTime = windows::ToFileTime(info.ftCreationTime);
	stat.Identity = MakeIdentity(ids.VolumeSerial, ids.FileId);
	return stat;
}


// ---------------- Writing ----------------

// Directory and every missing directory above it. CreateDirectoryW on extended-length paths, not
// std::filesystem::create_directories, which passes the path to Win32 as it is and so stops at
// MAX_PATH. Walks up only past missing parents, then creates downward.
void CreateDirectories (const std::filesystem::path& Directory)
{
	const std::filesystem::path win32 = windows::ToWin32Path(Directory);
	if (CreateDirectoryW(win32.c_str(), nullptr) || GetLastError() != ERROR_PATH_NOT_FOUND)
	{
		return;   // created, there already (ERROR_ALREADY_EXISTS), or a failure no parent fixes
	}
	const std::filesystem::path parent = Directory.parent_path();
	if (!parent.empty() && parent != Directory)
	{
		CreateDirectories(parent);
		CreateDirectoryW(win32.c_str(), nullptr);
	}
}

void CreateParentDirectories (const std::filesystem::path& Path)
{
	// A failure shows as the file's creation failing next.
	if (Path.has_parent_path())
	{
		CreateDirectories(Path.parent_path());
	}
}

bool WriteAll (HANDLE File, std::span<const uint8> Bytes, const std::filesystem::path& Path, FString* OutError)
{
	uint64 done = 0u;
	while (done < Bytes.size())
	{
		const uint64 remaining = Bytes.size() - done;
		const DWORD wanted = static_cast<DWORD>(remaining < MaxWritePerCall ? remaining : MaxWritePerCall);
		DWORD written = 0u;
		if (!WriteFile(File, Bytes.data() + done, wanted, &written, nullptr) || written == 0u)
		{
			windows::SetError(OutError, "Cannot write", Path, GetLastError());
			return false;
		}
		done += written;
	}
	return true;
}

// ".<up to TempNameUnits units of the name>.<pid>-<n>.tmp" beside Target: a dot-name and a .tmp,
// so asset scans and the watcher's built-in excludes skip it (§3.5.2), and unique per process and
// call. The name is cut between UTF-16 units, never inside a surrogate pair.
std::filesystem::path MakeTempPath (const std::filesystem::path& Target, uint32 Number)
{
	const std::filesystem::path fileName = Target.filename();
	const std::filesystem::path::string_type& name = fileName.native();
	size_t keep = name.size() < TempNameUnits ? name.size() : TempNameUnits;
	if (keep < name.size() && keep > 0u && IS_HIGH_SURROGATE(name[keep - 1u]))
	{
		--keep;
	}

	std::filesystem::path::string_type tempName = L".";
	tempName.append(name, 0u, keep);
	tempName += ToWide(FString::Format(".{}-{}.tmp", GetCurrentProcessId(), Number)).View();
	return Target.parent_path() / tempName;
}

bool HasAttribute (const std::filesystem::path& Path, DWORD Attribute) noexcept
{
	const DWORD attributes = GetFileAttributesW(windows::ToWin32Path(Path).c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & Attribute) != 0u;
}

// What RenamePosix did: Error is ERROR_SUCCESS or the code of the step that failed; bRenamed says
// whether the name switched over (a failed flush comes after it).
struct SRenameResult
{
	DWORD Error = ERROR_SUCCESS;
	bool bRenamed = false;
};

// Renames Temp over Target with POSIX semantics (FILE_RENAME_FLAG_POSIX_SEMANTICS, Windows 10 1709
// and later, NTFS): the name switches over even while other handles have the old target open with
// FILE_SHARE_DELETE - a reader keeps the old file, as on POSIX. MoveFileExW's classic semantics
// refuse that with access denied as long as any handle is open (measured).
//
// Then it flushes the renamed file through the same handle, so the rename is on disk when this
// returns - what MOVEFILE_WRITE_THROUGH asks of MoveFileExW. NTFS journals the rename but writes
// its log lazily; after a power loss the name could otherwise come back pointing at the old file,
// although the caller was told the write succeeded. The rename rewrites the file's own record (its
// $FILE_NAME), and FlushFileBuffers writes that record's changes and the log up to them, which
// holds the whole rename (NtFlushBuffersFileEx: without flags, data and metadata, then a hardware
// flush). FlushFileBuffers needs write access, hence FILE_WRITE_DATA; FILE_FLAG_WRITE_THROUGH asks
// the same of the rename itself.
SRenameResult RenamePosix (const std::filesystem::path& Temp, const std::filesystem::path& Target)
{
	const CScopedHandle file(CreateFileW(Temp.c_str(), DELETE | SYNCHRONIZE | FILE_WRITE_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_WRITE_THROUGH,
		nullptr));
	if (!file.IsValid())
	{
		return { GetLastError(), false };
	}

	// FILE_RENAME_INFO ends in the name; 8-byte aligned storage for its HANDLE member.
	const std::filesystem::path::string_type& name = Target.native();
	const size_t nameBytes = name.size() * sizeof(name[0]);
	std::vector<uint64> storage((sizeof(FILE_RENAME_INFO) + nameBytes + sizeof(uint64) - 1u) / sizeof(uint64) + 1u);
	auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
	info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
	info->RootDirectory = nullptr;
	info->FileNameLength = static_cast<DWORD>(nameBytes);
	std::memcpy(info->FileName, name.c_str(), nameBytes + sizeof(name[0]));
	const DWORD infoBytes = static_cast<DWORD>(storage.size() * sizeof(uint64));
	if (!SetFileInformationByHandle(file.Get(), FileRenameInfoEx, info, infoBytes))
	{
		return { GetLastError(), false };
	}
	return { FlushFileBuffers(file.Get()) ? static_cast<DWORD>(ERROR_SUCCESS) : GetLastError(), true };
}

// What a file system or server without POSIX renames answers (FAT, older Windows, some SMB servers).
bool IsUnsupportedRename (DWORD Error) noexcept
{
	return Error == ERROR_INVALID_PARAMETER || Error == ERROR_NOT_SUPPORTED || Error == ERROR_INVALID_FUNCTION
		|| Error == ERROR_INVALID_LEVEL;
}


// ---------------- Volumes ----------------

// CfGetSyncRootInfoByPath (cldapi.dll, Windows 10 1709+) succeeds for any path inside a Cloud Files
// sync root (OneDrive, and other sync clients built on that API). Loaded at run time: cfapi.h needs
// NTSTATUS from the native headers, and a missing cldapi.dll must not stop Core from loading. The
// two declarations below are cfapi.h's CF_SYNC_ROOT_INFO_BASIC and CF_SYNC_ROOT_BASIC_INFO.
constexpr int CloudSyncRootInfoBasic = 0;

struct SCloudSyncRootBasicInfo
{
	LARGE_INTEGER SyncRootFileId;
};

using FCfGetSyncRootInfoByPath = HRESULT (WINAPI*) (LPCWSTR FilePath, int InfoClass, void* InfoBuffer, DWORD InfoBufferLength,
	DWORD* ReturnedLength);

bool IsInCloudSyncRoot (const std::filesystem::path& Directory) noexcept
{
	static const FCfGetSyncRootInfoByPath getSyncRootInfo = []
	{
		const HMODULE module = LoadLibraryExW(L"cldapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		return module != nullptr
			? reinterpret_cast<FCfGetSyncRootInfoByPath>(reinterpret_cast<void*>(GetProcAddress(module, "CfGetSyncRootInfoByPath")))
			: nullptr;
	}();
	if (getSyncRootInfo == nullptr)
	{
		return false;
	}
	SCloudSyncRootBasicInfo info = {};
	DWORD returned = 0u;
	const std::filesystem::path win32 = windows::ToWin32Path(Directory);
	return SUCCEEDED(getSyncRootInfo(win32.c_str(), CloudSyncRootInfoBasic, &info, sizeof(info), &returned));
}

// The attributes cloud sync clients put on placeholders (files and folders whose bytes are not all
// local) and on files they manage.
constexpr DWORD CloudPlaceholderAttributes = FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
	| FILE_ATTRIBUTE_PINNED | FILE_ATTRIBUTE_UNPINNED | FILE_ATTRIBUTE_OFFLINE;

// The volume root holding Path ("C:\", "\\server\share\", a mounted folder), for GetDriveTypeW.
std::filesystem::path GetVolumeRoot (const std::filesystem::path& Path)
{
	// The root is a prefix of the absolute path, plus a trailing separator at most.
	const std::filesystem::path win32 = windows::ToWin32Path(Path);
	std::vector<wchar_t> root((win32.native().size() > MAX_PATH ? win32.native().size() : MAX_PATH) + 2u);
	if (!GetVolumePathNameW(win32.c_str(), root.data(), static_cast<DWORD>(root.size())))
	{
		return {};
	}
	return std::filesystem::path(root.begin(), std::find(root.begin(), root.end(), L'\0'));
}
}


// ---------------- Stat ----------------

std::optional<SFileStat> Stat (const std::filesystem::path& Path, FString* OutError)
{
	const CScopedHandle handle(OpenForAttributes(Path));
	if (!handle.IsValid())
	{
		windows::SetError(OutError, "Cannot stat", Path, GetLastError());
		return std::nullopt;
	}
	return StatHandle(handle.Get(), Path, OutError);
}

std::optional<SFileIdentity> GetFileIdentity (const std::filesystem::path& Path, FString* OutError)
{
	const std::optional<SFileStat> stat = Stat(Path, OutError);
	if (!stat.has_value())
	{
		return std::nullopt;
	}
	return stat->Identity;
}


// ---------------- Directory listing ----------------

bool EnumerateDirectory (const std::filesystem::path& Directory, TFunctionRef<bool (const SDirectoryEntry&)> OnEntry,
	FString* OutError)
{
	const CScopedHandle directory(CreateFileW(windows::ToWin32Path(Directory).c_str(),
		FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
	if (!directory.IsValid())
	{
		windows::SetError(OutError, "Cannot open the directory", Directory, GetLastError());
		return false;
	}

	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(directory.Get(), &info))
	{
		windows::SetError(OutError, "Cannot stat", Directory, GetLastError());
		return false;
	}
	if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u)
	{
		windows::SetErrorText(OutError, "Cannot list", Directory, "not a directory");
		return false;
	}
	const SHandleIds directoryIds = QueryHandleIds(directory.Get(), info);

	// FILE_ID_EXTD_DIR_INFO holds LARGE_INTEGERs: 8-byte aligned storage.
	std::vector<uint64> buffer(DirectoryBufferBytes / sizeof(uint64));
	const auto* bytes = reinterpret_cast<const uint8*>(buffer.data());

	SDirectoryEntry entry;
	FString name;
	// The first call restarts the listing explicitly (UE's FileJournalIterateDirectory does too).
	FILE_INFO_BY_HANDLE_CLASS infoClass = FileIdExtdDirectoryRestartInfo;
	bool bFirstCall = true;
	while (true)
	{
		if (!GetFileInformationByHandleEx(directory.Get(), infoClass, buffer.data(), DirectoryBufferBytes))
		{
			const DWORD error = GetLastError();
			// ERROR_NO_MORE_FILES ends every listing; ERROR_FILE_NOT_FOUND is an empty first page (a
			// volume root has no "." and ".." entries).
			if (error == ERROR_NO_MORE_FILES || (bFirstCall && error == ERROR_FILE_NOT_FOUND))
			{
				return true;
			}
			if (bFirstCall && (error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_LEVEL || error == ERROR_NOT_SUPPORTED))
			{
				windows::SetErrorText(OutError, "Cannot list", Directory,
					"the file system does not report file ids in directory listings (FileIdExtdDirectoryInfo)");
				return false;
			}
			windows::SetError(OutError, "Cannot list", Directory, error);
			return false;
		}
		infoClass = FileIdExtdDirectoryInfo;
		bFirstCall = false;

		size_t offset = 0u;
		while (true)
		{
			const auto* record = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO*>(bytes + offset);
			const size_t nameUnits = record->FileNameLength / sizeof(record->FileName[0]);
			const bool bDots = (nameUnits == 1u && record->FileName[0] == L'.')
				|| (nameUnits == 2u && record->FileName[0] == L'.' && record->FileName[1] == L'.');
			if (!bDots)
			{
				static_assert(sizeof(record->FileId.Identifier) == 16u);
				name = FString::FromWide(record->FileName, nameUnits);
				entry.Name = name.View();
				entry.Stat.bDirectory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
				entry.Stat.bReadOnly = (record->FileAttributes & FILE_ATTRIBUTE_READONLY) != 0u;
				entry.Stat.Size = entry.Stat.bDirectory ? 0u : static_cast<uint64>(record->EndOfFile.QuadPart);
				entry.Stat.LastWriteTime = windows::ToFileTime(record->LastWriteTime);
				entry.Stat.CreationTime = windows::ToFileTime(record->CreationTime);
				entry.Stat.Identity = MakeIdentity(directoryIds.VolumeSerial, record->FileId.Identifier);
				entry.bReparsePoint = (record->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
				if (!OnEntry(entry))
				{
					return true;
				}
			}
			if (record->NextEntryOffset == 0u)
			{
				break;
			}
			offset += record->NextEntryOffset;
		}
	}
}


// ---------------- Writing ----------------

bool WriteFileBytes (const std::filesystem::path& Path, std::span<const uint8> Bytes, bool bCreateDirectories, FString* OutError)
{
	if (bCreateDirectories)
	{
		CreateParentDirectories(Path);
	}

	CScopedHandle file(CreateFileW(windows::ToWin32Path(Path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file.IsValid())
	{
		windows::SetError(OutError, "Cannot create", Path, GetLastError());
		return false;
	}
	const bool bWritten = WriteAll(file.Get(), Bytes, Path, OutError);
	// Closing can still fail (a full disk, a network share): that is a failed write too.
	if (!file.Reset() && bWritten)
	{
		windows::SetError(OutError, "Cannot close", Path, GetLastError());
		return false;
	}
	return bWritten;
}

bool ReplaceFileAtomic (const std::filesystem::path& Temp, const std::filesystem::path& Target, FString* OutError)
{
	// Absolute (the rename's name would be resolved against the working directory too, but an
	// absolute one cannot surprise), and extended-length where long: WriteFileAtomic's temporary
	// name is longer than the target's.
	const std::filesystem::path temp = windows::ToWin32Path(Temp);
	const std::filesystem::path target = windows::ToWin32Path(Target);

	bool bPosix = true;
	DWORD sleepMs = 1u;
	DWORD sleptMs = 0u;
	while (true)
	{
		DWORD error = ERROR_SUCCESS;
		if (bPosix)
		{
			const SRenameResult rename = RenamePosix(temp, target);
			if (rename.bRenamed)
			{
				if (rename.Error == ERROR_SUCCESS)
				{
					return true;
				}
				// The new file is in place, but not known to be on disk: not the durable replace the
				// caller asked for. No retry - Temp is gone.
				if (OutError != nullptr)
				{
					*OutError = FString::Format("Replaced '{}' but cannot flush it to disk: {} ({})", FString::FromPath(Target),
						GetSystemErrorMessage(rename.Error), rename.Error);
				}
				return false;
			}
			error = rename.Error;
			if (IsUnsupportedRename(error))
			{
				bPosix = false;
				continue;
			}
		}
		else
		{
			if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				return true;
			}
			error = GetLastError();
		}

		// Transient: the target is open without FILE_SHARE_DELETE (a sharing violation, or access
		// denied while a delete is pending or, for MoveFileExW, while any handle is open), or the
		// temporary file is open without FILE_SHARE_WRITE. Access denied for a read-only file or a
		// directory is not.
		const bool bTransient = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
			|| (error == ERROR_ACCESS_DENIED && !HasAttribute(target, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)
				&& !HasAttribute(temp, FILE_ATTRIBUTE_READONLY));
		if (!bTransient || sleptMs >= ReplaceRetryBudgetMs)
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Format("Cannot replace '{}' with '{}': {} ({})", FString::FromPath(Target), FString::FromPath(Temp),
					GetSystemErrorMessage(error), error);
			}
			return false;
		}
		Sleep(sleepMs);
		sleptMs += sleepMs;
		sleepMs = sleepMs * 2u < ReplaceRetryMaxSleepMs ? sleepMs * 2u : ReplaceRetryMaxSleepMs;
	}
}

bool WriteFileAtomic (const std::filesystem::path& Path, std::span<const uint8> Bytes, bool bCreateDirectories, FString* OutError)
{
	if (!Path.has_filename())
	{
		windows::SetErrorText(OutError, "Cannot write", Path, "no file name");
		return false;
	}
	// Fails at the replace anyway: say so before writing the whole file.
	if (HasAttribute(Path, FILE_ATTRIBUTE_READONLY))
	{
		windows::SetErrorText(OutError, "Cannot write", Path, "the file is read-only");
		return false;
	}
	if (bCreateDirectories)
	{
		CreateParentDirectories(Path);
	}

	// CREATE_NEW: never reuse a file that happens to have the name (a temporary file left by a
	// crashed process that had the same id). Its name is longer than the target's, so its path may
	// pass MAX_PATH where the target's does not: opened by its extended-length form then.
	std::filesystem::path temp;
	HANDLE rawFile = INVALID_HANDLE_VALUE;
	for (uint32 attempt = 0u; attempt < 16u && rawFile == INVALID_HANDLE_VALUE; ++attempt)
	{
		temp = MakeTempPath(Path, gTempFileCounter.fetch_add(1u, std::memory_order_relaxed));
		rawFile = CreateFileW(windows::ToWin32Path(temp).c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (rawFile == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
		{
			break;
		}
	}
	CScopedHandle file(rawFile);
	if (!file.IsValid())
	{
		windows::SetError(OutError, "Cannot create a temporary file for", Path, GetLastError());
		return false;
	}

	bool bWritten = WriteAll(file.Get(), Bytes, temp, OutError);
	// On disk before the rename makes it visible: otherwise a power loss could leave the new name
	// pointing at bytes that were never written.
	if (bWritten && !FlushFileBuffers(file.Get()))
	{
		windows::SetError(OutError, "Cannot flush", temp, GetLastError());
		bWritten = false;
	}
	if (!file.Reset() && bWritten)
	{
		windows::SetError(OutError, "Cannot close", temp, GetLastError());
		bWritten = false;
	}

	if (!bWritten || !ReplaceFileAtomic(temp, Path, OutError))
	{
		// Gone already when the replace went through but could not be flushed.
		DeleteFileW(windows::ToWin32Path(temp).c_str());
		return false;
	}
	return true;
}

bool WriteFileAtomic (const std::filesystem::path& Path, FStringView Text, bool bCreateDirectories, FString* OutError)
{
	return WriteFileAtomic(Path, Text.AsBytes(), bCreateDirectories, OutError);
}


// ---------------- Volumes ----------------

std::optional<SVolumeInfo> QueryVolumeInfo (const std::filesystem::path& Path, FString* OutError)
{
	std::error_code ec;
	const std::filesystem::path absolute = std::filesystem::absolute(Path, ec);
	CScopedHandle pathHandle(OpenForAttributes(absolute));
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!pathHandle.IsValid() || !GetFileInformationByHandle(pathHandle.Get(), &info))
	{
		windows::SetError(OutError, "Cannot open", Path, GetLastError());
		return std::nullopt;
	}

	// For a file, the facts of the directory it is in.
	const bool bFile = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
	const std::filesystem::path directory = bFile ? absolute.parent_path() : absolute;
	const CScopedHandle handle(bFile ? OpenForAttributes(directory) : pathHandle.Release());
	if (!handle.IsValid() || (bFile && !GetFileInformationByHandle(handle.Get(), &info)))
	{
		windows::SetError(OutError, "Cannot open", directory, GetLastError());
		return std::nullopt;
	}

	wchar_t fileSystemName[MAX_PATH + 1] = {};
	DWORD serial = 0u;
	DWORD maxComponentLength = 0u;
	DWORD flags = 0u;
	if (!GetVolumeInformationByHandleW(handle.Get(), nullptr, 0u, &serial, &maxComponentLength, &flags, fileSystemName,
		MAX_PATH + 1))
	{
		windows::SetError(OutError, "Cannot query the volume of", directory, GetLastError());
		return std::nullopt;
	}

	SVolumeInfo volume;
	volume.FileSystemName = FString::FromWide(fileSystemName);
	volume.MaxComponentLength = maxComponentLength;
	volume.bVolumeCaseSensitive = (flags & FILE_CASE_SENSITIVE_SEARCH) != 0u;
	volume.bCasePreserving = (flags & FILE_CASE_PRESERVED_NAMES) != 0u;
	volume.bReadOnly = (flags & FILE_READ_ONLY_VOLUME) != 0u;

	// Fails on file systems without the attribute (FAT, older Windows): not case-sensitive then.
	FILE_CASE_SENSITIVE_INFO caseInfo = {};
	if (GetFileInformationByHandleEx(handle.Get(), FileCaseSensitiveInfo, &caseInfo, sizeof(caseInfo)))
	{
		volume.bDirectoryCaseSensitive = (caseInfo.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0u;
	}
	// What FRT's own opens do (FileSystem.h): the kernel's obcaseinsensitive plays no part.
	volume.bCaseSensitive = volume.bVolumeCaseSensitive && volume.bDirectoryCaseSensitive;

	const std::filesystem::path root = GetVolumeRoot(directory);
	if (!root.empty())
	{
		const UINT driveType = GetDriveTypeW(root.c_str());
		volume.bRemote = driveType == DRIVE_REMOTE;
		volume.bRemovable = driveType == DRIVE_REMOVABLE;
	}

	volume.bCloudPlaceholder = (info.dwFileAttributes & CloudPlaceholderAttributes) != 0u || IsInCloudSyncRoot(directory);
	return volume;
}
}
