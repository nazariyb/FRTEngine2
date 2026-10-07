#pragma once

#include <memory>
#include <string>
#include <vector>

#include "CoreTypes.h"
#include "GameInstance.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/Scene/WorldExtraction.h"
#include "UI/DebugTools.h"

#include "CursorCapture.h"
#include "EditorCommands.h"
#include "EditorLayout.h"
#include "PlaySession.h"
#include "ViewportPanel.h"


namespace frt
{
class CEngine;
class CGameModule;
class CWorldScene;
}

namespace frt::input
{
struct SInputActionMapAsset;
}


namespace frt::editor
{
class CEditorSmokeTest;

/**
 * The editor: a host, like the launcher, that owns its loop and drives the engine's frame
 * (CEngine). It holds the world being edited, the play session running a copy of it, the
 * panels that show them, and the commands that act on all of these.
 *
 * Structure, kept flat on purpose (Blow, Muratori: the frame reads top to bottom):
 *   - commands (EditorCommands.h) are applied at the start of a frame, whoever queued them;
 *   - the UI is immediate mode: panels draw, and report what happened as plain data
 *     (SViewportFrame); the editor then routes camera and game input from those reports;
 *   - the world is updated, described to the renderer, and the visible viewports rendered.
 * New tools are new panels and new commands; nothing here is a framework to register with.
 *
 * The world is the game's content. The game module builds it once, through a short-lived game
 * instance, and is otherwise only run in play mode (CPlaySession) - the editor never ticks
 * game code on the edited world. As in Unity, both viewports show the world that is live: the
 * edit world in edit mode, the session's copy while playing. Only the Game view passes input
 * to the game, and only while it has focus.
 */
class CEditor
{
public:
	CEditor (CEngine& Engine, const CGameModule& Module);
	~CEditor ();
	CEditor (const CEditor&) = delete;
	CEditor& operator= (const CEditor&) = delete;

	// Builds the edit world, creates the viewports. False if the game could not be set up.
	bool StartUp ();
	// The editor's loop, until the window closes or Exit. Returns the process exit code.
	int Run ();
	void ShutDown ();

	// Queued; applied at the start of the next frame. A viewport target of Active is resolved
	// now: the viewport under the cursor, else the last one used.
	void Execute (EEditorCommand Command, EViewportTarget Viewport = EViewportTarget::Active);

	// ---- state, for tools and scripted runs ----
	CWorldScene& GetEditWorld () { return *EditWorld; }
	// The world the viewports show this frame: the play session's while playing.
	CWorldScene& GetActiveWorld ();
	const CPlaySession& GetPlaySession () const { return Play; }
	CViewportPanel& GetViewport (EViewportTarget Target);
	// The viewport shown over the editor (Shift+Space) or the screen (F11); null if none.
	const CViewportPanel* GetMaximizedViewport () const;
	bool IsFullscreen () const { return FullscreenViewport >= 0; }

private:
	void ApplyCommands ();
	void ForgetClosedViewports ();
	void ApplyCommand (const SEditorCommand& Command);
	void DrawMainMenu ();
	void DrawViewports (float FrameMilliseconds);
	void HandleShortcuts ();
	void UpdateCameraInput (float RealDeltaSeconds);
	SGameInput RouteGameInput () const;
	void StartPlay ();
	void StopPlay ();
	void RenderFrame ();
	void ApplyWindowMode ();
	int32 ResolveViewport (EViewportTarget Target) const;

	CEngine& Engine;
	const CGameModule& Module;

	std::unique_ptr<CWorldScene> EditWorld;
	graphics::CCamera SceneCamera;                     // the Scene view's camera: the editor's, not the world's
	input::SInputActionMapAsset* FlyActions = nullptr; // IAM_Editor: RMB + WASD

	CPlaySession Play;
	bool bStartPaused = false;                         // Pause pressed in edit mode
	bool bGameLook = false;                            // the game has the mouse captured this frame
	std::string PlayMessage;                           // why the last session ended early
	double PlayMessageTime = 0.0;

	// Indexed by EViewportTarget.
	static constexpr int32 ViewportCount = 2;
	CViewportPanel Viewports[ViewportCount];
	int32 ActiveViewport = 0;                          // last one used: commands and inspected view
	int32 MaximizedViewport = -1;
	int32 FullscreenViewport = -1;
	int32 FocusRequest = -1;                           // focus this viewport on the next frame
	float FrameMilliseconds = 0.0f;                    // smoothed, for the viewport readouts

	CEditorLayout Layout;
	CDebugTools Tools;
	graphics::SRenderExtractOptions Extract;
	CCursorCapture Capture;

	std::vector<SEditorCommand> Commands;
	std::unique_ptr<CEditorSmokeTest> SmokeTest;
	bool bStarted = false;
};
}
