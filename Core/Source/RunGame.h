#pragma once

#include "Core.h"
#include "Precision.h"


namespace frt
{
class GameInstance;

using GameFactory = GameInstance* (*) ();

// The standalone game: one world, one game instance, one view straight into the swap chain,
// the debug tools as an overlay (I). Returns the process exit code.
FRT_CORE_API int RunGame (GameFactory CreateGame);
}


#if defined(_WINDOWS)
#include <Windows.h>

// D3D12 Agility SDK opt-in. The loader only honours these exports when they come from the
// executable, never from a DLL, which is why they live in the macro the host module expands
// rather than in Core. D3D12Core.dll is copied to <exe dir>/D3D12/ by the Core prebuild step.
#define FRT_D3D12_AGILITY_EXPORTS\
	extern "C" { __declspec(dllexport) extern const unsigned int D3D12SDKVersion = 619u; }\
	extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

#if !defined(FRT_HEADLESS)
#define FRT_RUN_GAME(TGame)\
	FRT_D3D12_AGILITY_EXPORTS\
	int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)\
	{\
		FRT_VERIFY_REAL_PRECISION_ABI();\
		return ::frt::RunGame([] () -> ::frt::GameInstance* { return new TGame(); });\
	}
#else
#define FRT_RUN_GAME(TGame)\
	int main()\
	{\
		FRT_VERIFY_REAL_PRECISION_ABI();\
		return ::frt::RunGame([] () -> ::frt::GameInstance* { return new TGame(); });\
	}
#endif

#else
#error "Unsupported platform"
#endif
