#pragma once

// For host executables only (the launcher, the editor): what has to live in the .exe itself.

#include "Core.h"
#include "Precision.h"

#if defined(_WINDOWS)
#include <Windows.h>

#include <cstdio>
#include <string>

// D3D12 Agility SDK opt-in. The loader only honours these exports when they come from the
// executable, never from a DLL, so every host expands this once at file scope. D3D12Core.dll
// is copied to <exe dir>/D3D12/ by the Core prebuild step.
#define FRT_D3D12_AGILITY_EXPORTS\
	extern "C" { __declspec(dllexport) extern const unsigned int D3D12SDKVersion = 619u; }\
	extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

namespace frt
{
// A host that cannot start says why: on the console it was started from (scripts, CI), else in a
// message box - a windowed program started from Explorer or the IDE has nowhere else to say it.
inline void ReportStartupError (const std::string& Utf8Message)
{
	std::fprintf(stderr, "%s\n", Utf8Message.c_str());
	std::fflush(stderr);
	const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
	if (console == nullptr || console == INVALID_HANDLE_VALUE || GetFileType(console) == FILE_TYPE_UNKNOWN)
	{
		const int length = MultiByteToWideChar(CP_UTF8, 0, Utf8Message.c_str(), -1, nullptr, 0);
		std::wstring wide(static_cast<size_t>(length > 0 ? length : 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Utf8Message.c_str(), -1, wide.data(), length);
		MessageBoxW(nullptr, wide.c_str(), L"FRTEngine", MB_OK | MB_ICONERROR);
	}
}

inline std::wstring WidenUtf8 (const std::string& Utf8)
{
	const int length = MultiByteToWideChar(CP_UTF8, 0, Utf8.c_str(), static_cast<int>(Utf8.size()), nullptr, 0);
	std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Utf8.c_str(), static_cast<int>(Utf8.size()), wide.data(), length);
	return wide;
}
}

#else
#error "Unsupported platform"
#endif
