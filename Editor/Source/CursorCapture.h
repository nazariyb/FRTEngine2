#pragma once

#include "imgui.h"

#include "CoreTypes.h"
#include "Math/Math.h"


namespace frt
{
class CWindow;
}


namespace frt::editor
{
/**
 * The mouse, captured by a viewport camera while a button is held on it (Unity's and Unreal's
 * right-drag fly mode): the cursor is hidden and confined to the viewport, the camera reads
 * raw device motion - unaccelerated, and not stopped by the screen edge - and on release the
 * cursor reappears exactly where the drag started.
 */
class CCursorCapture
{
public:
	// Every frame, after the UI was built. bWant: some viewport wants the mouse this frame;
	// Min / Max: its image, in client pixels. Capture ends when bWant drops, the window loses
	// the foreground, or Release() is called.
	void Update (CWindow& Window, bool bWant, const ImVec2& Min, const ImVec2& Max);
	void Release (CWindow& Window);

	bool IsActive () const { return bActive; }

private:
	bool bActive = false;
	Vector2i RestorePosition = Vector2i(0, 0);
};
}
