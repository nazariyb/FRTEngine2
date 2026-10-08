#pragma once

// Dear ImGui build configuration, selected for every project by the workspace-wide
// IMGUI_USER_CONFIG define (Premake/workspace.lua), so all modules see the same layouts.
//
// ImGui and its Win32 / DX12 backends are compiled into their own ImGui DLL only
// (Premake/Projects/thirdparty.lua). Every module - Core, the editor, games - imports them, so the
// process has exactly one GImGui context and one allocator - the alternative, a second copy
// per module, needs SetCurrentContext() and SetAllocatorFunctions() at every DLL boundary (see
// "Context and Memory Allocators" in imgui.cpp). IMGUI_IMPL_API defaults to IMGUI_API, which
// exports the backends as well.
//
// In monolithic builds (Release) ImGui is a static library linked into the executable like
// everything else, so nothing is exported or imported.
//
// Deliberately does not include Core.h: it defines NOMINMAX, which must not leak into
// ImGui's own translation units. BuildConfig.h is macro-only.
//
// Headless builds compile no ImGui at all, so they leave IMGUI_API empty: an exported
// declaration would make the compiler emit references (e.g. ImGuiTextFilter's default
// constructor closure) to definitions that do not exist.
#include "BuildConfig.h"

#if !defined(FRT_HEADLESS) && !FRT_MONOLITHIC
	#if defined(FRT_IMGUI_EXPORTS)
		#define IMGUI_API __declspec(dllexport)
	#else
		#define IMGUI_API __declspec(dllimport)
	#endif
#endif

// 32-bit ImWchar. Windows delivers a typed supplementary-plane character (emoji, CJK Extension B)
// as a surrogate pair; with the default 16-bit ImWchar ImGui turns it into U+FFFD before
// ui::InputText (UI/ImGuiString.h) ever sees it. It changes ImWchar, a type in ImGui's API, so it
// must hold for the whole process - which this header does.
#define IMGUI_USE_WCHAR32
