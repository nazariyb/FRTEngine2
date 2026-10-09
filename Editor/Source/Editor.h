#pragma once

#include <memory>
#include <vector>

#include "CoreTypes.h"
#include "GameInstance.h"
#include "Containers/String.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/Scene/WorldExtraction.h"
#include "UI/DebugTools.h"

#include "CursorCapture.h"
#include "EditorCommands.h"
#include "EditorInput.h"
#include "EditorLayout.h"
#include "PlaySession.h"
#include "ViewportCamera.h"
#include "ViewportPanel.h"


namespace frt
{
class CEngine;
class CGameModule;
class CWorldScene;
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
 *
 * Hot reload (FRT_WITH_GAME_HOT_RELOAD): the editor watches the game module's build. A newer
 * one stops play, drops every world and instance holding the old code, loads the module again
 * and has it build the edit world anew, between frames. Rebuilt rather than kept: a world's
 * component pools are code of the module that filled it - a copy's too (CWorldScene::Clone) -
 * and there is no world serialization yet to carry it over. The world is code-authored
 * content, so the new code's world is the one to see; tweaks made in the tools since are lost.
 * If the new build does not load, the editor runs on with an empty world, no play mode and the
 * problem in the toolbar, until a build that does.
 *
 * The editor's own input is plain key bindings (CEditorInput, Editor/Config/EditorInput.ini):
 * chords that run commands, and keys that move the Scene camera, which navigates the way
 * Unreal's level viewport does (CViewportCameraController).
 */
class CEditor
{
public:
	// Module is loaded; the editor reloads it when a newer build appears.
	CEditor (CEngine& Engine, CGameModule& Module);
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
	// The viewport shown over the editor or the screen; null if none.
	const CViewportPanel* GetMaximizedViewport () const;
	bool IsFullscreen () const { return FullscreenViewport >= 0; }
	const CEditorInput& GetInput () const { return Input; }
	CEditorInput& GetInput () { return Input; }
	const graphics::CCamera& GetSceneCamera () const { return SceneCamera; }
	const CViewportCameraController& GetSceneCameraController () const { return SceneCameraController; }
	// Gives the viewport keyboard focus on the next frame.
	void FocusViewport (EViewportTarget Viewport);
	const CGameModule& GetGameModule () const { return Module; }
	CDebugTools& GetTools () { return Tools; }
	// Newer builds of the game module loaded so far.
	uint32 GetGameReloads () const { return GameReloads; }
	// Looks for a newer build of the game module at the start of the next frame.
	void PollGameModuleNow () { NextGamePoll = 0.0; }

private:
	void ApplyCommands ();
	void ForgetClosedViewports ();
	void ApplyCommand (const SEditorCommand& Command);
	void DrawMainMenu ();
	void DrawViewports ();
	void HandleShortcuts ();
	void UpdateCameraInput (float RealDeltaSeconds);
	SGameInput RouteGameInput () const;
	bool BuildEditWorld ();
	void PollGameModule ();
	void ReloadGame ();
	void StartPlay ();
	void StopPlay ();
	void ApplyInputConfig (bool bInitial);
	void PostNotice (FString Text, bool bError);
	void RenderFrame ();
	void ApplyWindowMode ();
	int32 ResolveViewport (EViewportTarget Target) const;

	CEngine& Engine;
	CGameModule& Module;
	double NextGamePoll = 0.0;                         // UI time to look for a newer build again
	uint32 GameReloads = 0u;
	FString GameProblem;                               // why there is no game or world; shown until a build fixes it

	std::unique_ptr<CWorldScene> EditWorld;
	graphics::CCamera SceneCamera;                     // the Scene view's camera: the editor's, not the world's
	CViewportCameraController SceneCameraController;

	CEditorInput Input;
	float ConfiguredCameraSpeed = 0.0f;                // the config's Speed, as last applied
	FString FullscreenExitHint;                        // the fullscreen viewport's corner text, from the bindings last applied

	CPlaySession Play;
	bool bStartPaused = false;                         // Pause pressed in edit mode
	bool bGameLook = false;                            // the game has the mouse captured this frame

	// A line in the toolbar for a few seconds: why play stopped, what the input config said.
	FString Notice;
	double NoticeTime = 0.0;
	bool bNoticeIsError = false;

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
