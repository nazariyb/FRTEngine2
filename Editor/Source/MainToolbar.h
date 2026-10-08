#pragma once

#include <optional>

#include "CoreTypes.h"

#include "EditorCommands.h"


namespace frt::editor
{
// What the toolbar shows this frame.
struct SToolbarState
{
	bool bPlaying = false;
	bool bCanPlay = true;          // a game is loaded
	bool bPaused = false;          // in edit mode: the next session starts paused
	double PlaySeconds = 0.0;      // the session's game time
	uint64 PlayFrames = 0u;        // ... and simulated frames
	const char* Message = nullptr; // a notice to show, or null
	bool bMessageIsError = false;
	// Shortcut texts for the tooltips ("" when unbound).
	const char* PlayShortcut = "";
	const char* PauseShortcut = "";
	const char* StepShortcut = "";
};

/**
 * Unity's toolbar: Play, Pause and Step centred under the main menu, the play state on the
 * right. Every frame, after the main menu bar and before the dock space, whose work area it
 * takes its row from. Returns the command a click asked for.
 */
std::optional<EEditorCommand> DrawMainToolbar (const SToolbarState& State);
}
