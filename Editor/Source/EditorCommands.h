#pragma once

#include "CoreTypes.h"


namespace frt::editor
{
/**
 * Everything the user can ask the editor to do, as data. Menus, the toolbar, shortcuts and
 * scripted runs all queue the same commands, which the editor applies at the start of the next
 * frame - one path per action, and never a world swapped out halfway through a frame.
 */
enum class EEditorCommand : uint8
{
	Play,                 // enter play mode
	Stop,                 // leave play mode
	TogglePlay,           // Ctrl+P
	TogglePause,          // Ctrl+Shift+P; in edit mode: start the next session paused
	Step,                 // Ctrl+Alt+P: one game frame, then paused
	ToggleMaximize,       // Shift+Space: the viewport fills the editor
	ToggleFullscreen,     // F11: the viewport fills the screen
	ToggleRenderMode,     // F2: path traced <-> raster preview
	ResetLayout,
	Exit,
};

// Which viewport a viewport command acts on.
enum class EViewportTarget : int8
{
	Active = -1,          // the one under the cursor, else the last one used
	Scene = 0,
	Game = 1,
};

struct SEditorCommand
{
	EEditorCommand Id = EEditorCommand::Exit;
	EViewportTarget Viewport = EViewportTarget::Active;
};
}
