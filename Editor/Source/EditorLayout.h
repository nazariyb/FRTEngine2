#pragma once

#include "imgui.h"


namespace frt::editor
{
/**
 * The editor's dock space: one dock tree over the window's work area (below the main menu and
 * the toolbar), which ImGui saves with the layout. The default arrangement is Unity's: Scene and
 * Game tabbed in the centre, tools on the right, stats along the bottom. It is built when no
 * saved layout exists and on Reset Layout.
 */
class CEditorLayout
{
public:
	// Every frame, after the main menu bar and the toolbar (which shrink the work area) and
	// before any panel. True on a frame the default layout was (re)built.
	bool Draw ();

	void RequestReset () { bResetRequested = true; }

private:
	static void BuildDefault (ImGuiID DockSpace);

	bool bResetRequested = false;
};
}
