#pragma once

// For host executables only (the launcher, the editor): what has to live in the .exe itself.

#include "Core.h"
#include "Precision.h"

#if defined(_WINDOWS)
#include <Windows.h>

// D3D12 Agility SDK opt-in. The loader only honours these exports when they come from the
// executable, never from a DLL, so every host expands this once at file scope. D3D12Core.dll
// is copied to <exe dir>/D3D12/ by the Core prebuild step.
#define FRT_D3D12_AGILITY_EXPORTS\
	extern "C" { __declspec(dllexport) extern const unsigned int D3D12SDKVersion = 619u; }\
	extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

#else
#error "Unsupported platform"
#endif
