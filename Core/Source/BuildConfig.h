#pragma once

// What kind of build this is, and the switches derived from it. Macro-only and include-free, so
// that Asserts.h and UI/ImGuiConfig.h can include it without Core.h's Windows macros.
//
// Premake defines exactly one of these to 1 per configuration (Premake/workspace.lua):
//   FRT_BUILD_DEBUG        unoptimized, debug runtime
//   FRT_BUILD_DEVELOPMENT  optimized with debug information; asserts and developer tools on
//   FRT_BUILD_RELEASE      optimized; developer tools compiled out
// and FRT_MONOLITHIC=1 where Core and the game module are static libraries linked into the
// executable. Every derived switch below can still be forced from the command line.

#if !defined(FRT_BUILD_DEBUG) && !defined(FRT_BUILD_DEVELOPMENT) && !defined(FRT_BUILD_RELEASE)
	// Tooling and IDE parsers that do not see the generated project defines.
	#if defined(_DEBUG)
		#define FRT_BUILD_DEBUG 1
	#else
		#define FRT_BUILD_DEVELOPMENT 1
	#endif
#endif

#ifndef FRT_BUILD_DEBUG
	#define FRT_BUILD_DEBUG 0
#endif
#ifndef FRT_BUILD_DEVELOPMENT
	#define FRT_BUILD_DEVELOPMENT 0
#endif
#ifndef FRT_BUILD_RELEASE
	#define FRT_BUILD_RELEASE 0
#endif

#if (FRT_BUILD_DEBUG + FRT_BUILD_DEVELOPMENT + FRT_BUILD_RELEASE) != 1
	#error "Exactly one of FRT_BUILD_DEBUG, FRT_BUILD_DEVELOPMENT and FRT_BUILD_RELEASE must be 1"
#endif

// Core and the game linked into one executable: nothing is exported or imported, and the game
// module is found by the linker instead of LoadLibrary (Engine/GameModule.h).
#ifndef FRT_MONOLITHIC
	#define FRT_MONOLITHIC 0
#endif

// Reloading materials, input actions and shaders from disk while running.
#ifndef FRT_WITH_DEV_TOOLS
	#define FRT_WITH_DEV_TOOLS (!FRT_BUILD_RELEASE)
#endif

// The game module DLL loaded from a copy, so the build can overwrite it while it runs
// (Engine/GameModule.h).
#ifndef FRT_WITH_GAME_HOT_RELOAD
	#define FRT_WITH_GAME_HOT_RELOAD (FRT_WITH_DEV_TOOLS && !FRT_MONOLITHIC)
#endif

// Shaders compiled with -Zi -Qembed_debug, for PIX / RenderDoc source-level debugging.
#ifndef FRT_SHADER_DEBUG_INFO
	#define FRT_SHADER_DEBUG_INFO (!FRT_BUILD_RELEASE)
#endif

// The D3D12 debug layer, DXGI debug factory and DRED, unless FRT_D3D_DEBUG=0 is in the
// environment. Where this is 0 (Development), FRT_D3D_DEBUG=1 turns them on, so Development
// performance stays comparable with Release by default. Never compiled into Release.
#ifndef FRT_GPU_DEBUG_BY_DEFAULT
	#define FRT_GPU_DEBUG_BY_DEFAULT FRT_BUILD_DEBUG
#endif
