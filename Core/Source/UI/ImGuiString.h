#pragma once

#include <span>

#include "imgui.h"

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * Dear ImGui over the engine's string types. ImGui text is UTF-8, like FString, so nothing is
 * converted: these only manage buffers.
 *
 * Labels, IDs and format strings stay const char*: literals, CStr(), or
 * FormatToBuffer(...).GetData() for per-frame text. An FStringView is not NUL-terminated - show it
 * with ui::TextUnformatted, or ImGui::Text("%.*s", int(View.Len()), View.GetData()), never "%s".
 *
 * Headless builds have no ImGui: these are declared there, but defined only where ImGui is.
 */
namespace frt::ui
{
// Edits an FString in place, imgui_stdlib-style: ImGuiInputTextFlags_CallbackResize grows Value as
// the text grows, and every other callback event (CharFilter, Completion, History, ...) goes to
// Callback with UserData. The resize callback is this wrapper's: Flags must not ask for it.
//
// MaxBytes > 0 limits the text to that many bytes: Value is first TruncateInline(MaxBytes)'d, then
// edited in a fixed buffer of MaxBytes + 1 bytes, where ImGui never inserts a partial UTF-8 sequence.
// MaxBytes is last so an ImGuiInputTextFlags argument can never bind to it.
FRT_CORE_API bool InputText (const char* Label, FString& Value, ImGuiInputTextFlags Flags = 0,
	ImGuiInputTextCallback Callback = nullptr, void* UserData = nullptr, uint32 MaxBytes = 0);
FRT_CORE_API bool InputTextWithHint (const char* Label, const char* Hint, FString& Value, ImGuiInputTextFlags Flags = 0,
	ImGuiInputTextCallback Callback = nullptr, void* UserData = nullptr, uint32 MaxBytes = 0);
FRT_CORE_API bool InputTextMultiline (const char* Label, FString& Value, const ImVec2& Size = ImVec2(0.0f, 0.0f),
	ImGuiInputTextFlags Flags = 0, ImGuiInputTextCallback Callback = nullptr, void* UserData = nullptr, uint32 MaxBytes = 0);

// The view's bytes exactly: no format string, no NUL needed.
inline void TextUnformatted (FStringView Text)
{
	ImGui::TextUnformatted(Text.GetData(), Text.GetData() + Text.Len());
}

// A combo over typed items: runtime lists (std::vector<FString>, TArray<FString>) and static name
// tables (enum_::GetValueNames<E>()), with no void* getter casting UserData at the call site.
// Index is the selected item, as for ImGui::Combo.
FRT_CORE_API bool Combo (const char* Label, int* Index, std::span<const FString> Items, int PopupMaxHeightInItems = -1);
FRT_CORE_API bool Combo (const char* Label, int* Index, std::span<const FStringLiteral> Items, int PopupMaxHeightInItems = -1);
}
