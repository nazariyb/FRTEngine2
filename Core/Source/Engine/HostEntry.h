#pragma once

// For host executables only (the launcher, the editor): what has to live in the .exe itself. Their
// main calls platform::InitConsoleUtf8 first thing (Launcher.cpp, EditorMain.cpp); a new host must
// too, or its UTF-8 output shows garbled on a console in another code page.

#include "Core.h"
#include "Precision.h"

#if defined(_WINDOWS)
#include <Windows.h>

#include <cstdio>

#include "Containers/String.h"
#include "Platform/Platform.h"

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
// The host exits next, so the console - often the cmd.exe or FRT.bat window that started it - gets
// back the code pages platform::InitConsoleUtf8 replaced (RestoreConsoleCodePages) right away, not
// only at the very end.
inline void ReportStartupError (FStringView Message)
{
	std::fwrite(Message.GetData(), 1u, Message.Len(), stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	platform::RestoreConsoleCodePages();
	const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
	if (console == nullptr || console == INVALID_HANDLE_VALUE || GetFileType(console) == FILE_TYPE_UNKNOWN)
	{
		platform::ShowErrorMessage("FRTEngine", Message);
	}
}
}

#else
#error "Unsupported platform"
#endif
