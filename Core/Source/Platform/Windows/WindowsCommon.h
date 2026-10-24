#pragma once

// Shared by the Windows implementations in this directory only: Win32 types never leave
// Core/Source/Platform/Windows/ (the public declarations are in Core/Source/Platform/*.h, and
// Tools/Lint/CheckPlatform.py keeps Win32 out of the asset system's sources).

#include <filesystem>

#include <Windows.h>

#include "CoreTypes.h"
#include "Containers/String.h"
#include "Platform/Platform.h"


namespace frt::platform::windows
{
// Closes the handle it owns. INVALID_HANDLE_VALUE and nullptr both mean "none": CreateFileW
// reports failure with the first, most other calls with the second.
class CScopedHandle
{
public:
	CScopedHandle () noexcept = default;
	explicit CScopedHandle (HANDLE InHandle) noexcept : Handle(InHandle == INVALID_HANDLE_VALUE ? nullptr : InHandle) {}
	~CScopedHandle () { Reset(); }

	CScopedHandle (const CScopedHandle&) = delete;
	CScopedHandle& operator= (const CScopedHandle&) = delete;

	[[nodiscard]] bool IsValid () const noexcept { return Handle != nullptr; }
	[[nodiscard]] HANDLE Get () const noexcept { return Handle; }
	// Gives up ownership: the caller closes it.
	[[nodiscard]] HANDLE Release () noexcept
	{
		const HANDLE handle = Handle;
		Handle = nullptr;
		return handle;
	}

	// Closes the handle; false if CloseHandle failed (a write that was still being completed).
	bool Reset () noexcept
	{
		const bool bClosed = Handle == nullptr || CloseHandle(Handle) != 0;
		Handle = nullptr;
		return bClosed;
	}

private:
	HANDLE Handle = nullptr;
};

// "<What> '<Path>': <system message> (<code>)" into *OutError, when there is one.
inline void SetError (FString* OutError, FStringView What, const std::filesystem::path& Path, DWORD ErrorCode)
{
	if (OutError != nullptr)
	{
		*OutError = FString::Format("{} '{}': {} ({})", What, FString::FromPath(Path), GetSystemErrorMessage(ErrorCode), ErrorCode);
	}
}

// The same for an HRESULT (COM, the shell), shown in hex as Windows tools print them.
inline void SetErrorHresult (FString* OutError, FStringView What, const std::filesystem::path& Path, HRESULT Result)
{
	if (OutError != nullptr)
	{
		const uint32 code = static_cast<uint32>(Result);
		*OutError = FString::Format("{} '{}': {} (0x{:08X})", What, FString::FromPath(Path), GetSystemErrorMessage(code), code);
	}
}

// "<What> '<Path>': <Reason>" for failures that have no system error code.
inline void SetErrorText (FString* OutError, FStringView What, const std::filesystem::path& Path, FStringView Reason)
{
	if (OutError != nullptr)
	{
		*OutError = FString::Format("{} '{}': {}", What, FString::FromPath(Path), Reason);
	}
}

// FILETIME / LARGE_INTEGER times as the int64 file time of SFileStat (the same 100 ns ticks).
[[nodiscard]] inline int64 ToFileTime (const FILETIME& Time) noexcept
{
	return static_cast<int64>((static_cast<uint64>(Time.dwHighDateTime) << 32u) | Time.dwLowDateTime);
}

[[nodiscard]] inline int64 ToFileTime (const LARGE_INTEGER& Time) noexcept
{
	return static_cast<int64>(Time.QuadPart);
}
}
