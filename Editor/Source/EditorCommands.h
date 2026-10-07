#pragma once

#include "CoreTypes.h"
#include "Enum.h"


namespace frt::editor
{
/**
 * Everything the user can ask the editor to do, as data. Menus, the toolbar, shortcuts and
 * scripted runs all queue the same commands, which the editor applies at the start of the next
 * frame - one path per action, and never a world swapped out halfway through a frame.
 *
 * Every command can be bound to keys by its name in the [Commands] section of the editor's
 * input config (EditorInput.h); the defaults are in Editor/Config/EditorInput.ini.
 */
enum class EEditorCommand : uint8
{
	Play,                 // enter play mode
	Stop,                 // leave play mode
	TogglePlay,
	TogglePause,          // in edit mode: start the next session paused
	Step,                 // one game frame, then paused
	ToggleMaximize,       // the viewport fills the editor
	ToggleFullscreen,     // the viewport fills the screen
	ExitFullscreen,       // back from fullscreen; nothing otherwise
	ToggleRenderMode,     // path traced <-> raster preview
	ResetLayout,
	Exit,
	Count
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


// Command names as the input config spells them.
FRT_DECLARE_ENUM_REFLECTION(
	frt::editor::EEditorCommand,
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, Play),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, Stop),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, TogglePlay),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, TogglePause),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, Step),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, ToggleMaximize),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, ToggleFullscreen),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, ExitFullscreen),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, ToggleRenderMode),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, ResetLayout),
	FRT_ENUM_ENTRY(frt::editor::EEditorCommand, Exit)
);
