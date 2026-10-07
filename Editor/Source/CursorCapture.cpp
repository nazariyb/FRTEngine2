#include "CursorCapture.h"

#include "imgui_internal.h"

#include "Window.h"


namespace frt::editor
{
void CCursorCapture::Update (CWindow& Window, bool bWant, const ImVec2& Min, const ImVec2& Max)
{
	if (!bWant || !Window.IsForeground())
	{
		Release(Window);
		return;
	}

	if (!bActive)
	{
		RestorePosition = Window.GetCursorClientPosition();
		bActive = true;
	}
	// Re-applied every frame: Windows drops the clip rectangle on focus and display changes.
	Window.ClipCursorToClientRect(
		static_cast<int32>(Min.x), static_cast<int32>(Min.y),
		static_cast<int32>(Max.x), static_cast<int32>(Max.y));
	// The Win32 backend hides the cursor for ImGuiMouseCursor_None. Set after every widget, so
	// nothing hovered later this frame overrides it.
	ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void CCursorCapture::Release (CWindow& Window)
{
	if (!bActive)
	{
		return;
	}
	Window.ReleaseCursorClip();
	// Put back where the drag started - unless the user has moved on to another window, or the
	// window is minimized or gone, where the cursor is no longer the editor's to move.
	if (Window.IsForeground() && !IsIconic(Window.GetHandle()))
	{
		Window.SetCursorClientPosition(RestorePosition);
		// The jump back is not mouse motion: nothing under the cursor may read it as a drag.
		ImGui::TeleportMousePos(ImVec2(static_cast<float>(RestorePosition.x), static_cast<float>(RestorePosition.y)));
	}
	bActive = false;
}
}
