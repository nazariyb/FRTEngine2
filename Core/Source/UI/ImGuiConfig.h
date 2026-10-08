#pragma once

// Dear ImGui build configuration, selected for every project by the workspace-wide
// IMGUI_USER_CONFIG define (Premake/workspace.lua), so all modules see the same layouts.
//
// ImGui and its Win32 / DX12 backends are compiled into their own ImGui DLL only
// (Premake/thirdparty.lua). Every module - Core, the editor, games - imports them, so the
// process has exactly one GImGui context and one allocator - the alternative, a second copy
// per module, needs SetCurrentContext() and SetAllocatorFunctions() at every DLL boundary (see
// "Context and Memory Allocators" in imgui.cpp). IMGUI_IMPL_API defaults to IMGUI_API, which
// exports the backends as well.
//
// Deliberately does not include Core.h: it defines NOMINMAX, which must not leak into
// ImGui's own translation units.
//
// Headless builds compile no ImGui at all, so they leave IMGUI_API empty: an exported
// declaration would make the compiler emit references (e.g. ImGuiTextFilter's default
// constructor closure) to definitions that do not exist.
#if !defined(FRT_HEADLESS)
	#if defined(FRT_IMGUI_EXPORTS)
		#define IMGUI_API __declspec(dllexport)
	#else
		#define IMGUI_API __declspec(dllimport)
	#endif
#endif
