#include "Platform/FileLock.h"

#include <chrono>
#include <system_error>
#include <utility>

#include "Platform/Windows/WindowsCommon.h"


namespace frt::platform
{
namespace
{
using windows::CScopedHandle;

// How long a refused lock is tried again while the file is pending deletion, or the holder's text
// is not there yet: long enough for a killed holder's handles to close and a new holder to write
// its few bytes, short enough that a tool refused by a live owner says so at once.
constexpr std::chrono::milliseconds LockRetryBudget(500);
constexpr DWORD LockRetrySleepMs = 5u;

bool WriteContents (HANDLE File, FStringView Contents)
{
	DWORD written = 0u;
	return WriteFile(File, Contents.GetData(), Contents.Len(), &written, nullptr) && written == Contents.Len();
}
}


std::optional<CExclusiveFileLock> TryLockFileExclusive (const std::filesystem::path& Path, FStringView Contents,
	FString* OutHolder, FString* OutError)
{
	if (OutHolder != nullptr)
	{
		OutHolder->Clear();
	}
	if (OutError != nullptr)
	{
		OutError->Clear();
	}
	if (Contents.IsEmpty())
	{
		// A refused caller could not tell an empty holder text from one not written yet.
		windows::SetErrorText(OutError, "Cannot lock", Path, "no text to write into the lock file");
		return std::nullopt;
	}

	// Lock files live in short cache directories (<project>/Local/Assets): std::filesystem is enough.
	if (Path.has_parent_path())
	{
		std::error_code ec;
		std::filesystem::create_directories(Path.parent_path(), ec);
	}

	const std::filesystem::path win32 = windows::ToWin32Path(Path);
	const auto start = std::chrono::steady_clock::now();
	for (;;)
	{
		// Read sharing only: no other open may write, delete or rename the file while this handle is
		// open. DELETE access is what FILE_FLAG_DELETE_ON_CLOSE needs.
		const HANDLE handle = CreateFileW(win32.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		if (handle != INVALID_HANDLE_VALUE)
		{
			CScopedHandle file(handle);
			if (!WriteContents(file.Get(), Contents))
			{
				windows::SetError(OutError, "Cannot write the lock file", Path, GetLastError());
				return std::nullopt;   // closing the handle deletes the file
			}
			return CExclusiveFileLock(file.Release(), Path);
		}

		const DWORD error = GetLastError();
		const bool bTimeLeft = std::chrono::steady_clock::now() - start < LockRetryBudget;
		if (error == ERROR_SHARING_VIOLATION)
		{
			// Held. Its holder writes its text right after creating the file: an empty file is one in
			// between, a missing one a holder that has just let go - both are worth another try.
			std::optional<FString> holder = ReadFileText(Path, ETextFileEncoding::Raw);
			if (holder.has_value() && !holder->IsEmpty())
			{
				if (OutHolder != nullptr)
				{
					*OutHolder = std::move(*holder);
				}
				windows::SetErrorText(OutError, "Cannot lock", Path, "it is held by another process");
				return std::nullopt;
			}
			if (!bTimeLeft)
			{
				if (OutHolder != nullptr)
				{
					*OutHolder = "an unknown process (its lock file could not be read)";
				}
				windows::SetErrorText(OutError, "Cannot lock", Path, "it is held by another process");
				return std::nullopt;
			}
		}
		else if ((error != ERROR_ACCESS_DENIED && error != ERROR_DELETE_PENDING) || !bTimeLeft)
		{
			// A file pending deletion refuses every open with access denied until its last reader
			// closes it; any other failure will not clear by itself.
			windows::SetError(OutError, "Cannot lock", Path, error);
			return std::nullopt;
		}
		Sleep(LockRetrySleepMs);
	}
}


CExclusiveFileLock::CExclusiveFileLock (void* InHandle, std::filesystem::path InPath) noexcept
	: Handle(InHandle)
	, Path(std::move(InPath))
{
}

CExclusiveFileLock::~CExclusiveFileLock ()
{
	Release();
}

CExclusiveFileLock::CExclusiveFileLock (CExclusiveFileLock&& Other) noexcept
	: Handle(std::exchange(Other.Handle, nullptr))
	, Path(std::move(Other.Path))
{
	Other.Path.clear();
}

CExclusiveFileLock& CExclusiveFileLock::operator= (CExclusiveFileLock&& Other) noexcept
{
	if (this != &Other)
	{
		Release();
		Handle = std::exchange(Other.Handle, nullptr);
		Path = std::move(Other.Path);
		Other.Path.clear();
	}
	return *this;
}

void CExclusiveFileLock::Release () noexcept
{
	if (Handle != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(Handle));
		Handle = nullptr;
	}
	Path.clear();
}
}
