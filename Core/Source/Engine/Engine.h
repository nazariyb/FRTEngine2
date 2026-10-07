#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "Core.h"
#include "CoreTypes.h"
#include "Timer.h"
#include "Config/Config.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Graphics/Render/RenderView.h"
#include "Input/InputActionLibrary.h"
#include "Input/InputSystem.h"
#include "Memory/MemoryPool.h"
#include "User/UserSettings.h"


namespace frt
{
class CImGuiLayer;
class CWindow;
}

namespace frt::graphics
{
class CMaterialLibrary;
class CRenderer;
}


namespace frt
{
struct SEngineDesc
{
	std::wstring WindowTitle = L"FRTEngine";
	// ImGui layout file. Empty: imgui.ini in the working directory (the project directory).
	std::filesystem::path UiIniPath;
	// Start with the window maximized instead of at the configured resolution.
	bool bMaximizeWindow = false;
	// The user's display settings may ask for DXGI exclusive fullscreen; false opens windowed
	// instead (tools: exclusive mode minimises on every focus change).
	bool bAllowExclusiveFullscreen = true;
};

// One view to render this frame and the camera to render it from.
struct SViewToRender
{
	graphics::CRenderView* View = nullptr;
	graphics::SCameraView Camera;
};

/**
 * The engine's services and the frame they share, for the hosts that drive them (the runtime
 * launcher and the editor).
 *
 * Owns every subsystem and brings them up and down explicitly, in dependency order (Gregory,
 * GEA "Subsystem Start-Up and Shut-Down") - never through static construction order:
 *   StartUp:  components, config (+ command line, user settings), memory, clock, window,
 *             renderer, input actions, UI.
 *   ShutDown: the exact reverse, after the GPU is idle.
 * Headless builds skip the window, renderer and UI, and resolve materials through a library
 * of their own; the class layout is the same either way.
 *
 * It knows nothing about worlds, games or play sessions: those belong to the hosts, which own
 * their loop (Muratori: the platform layer calls the game as a service). What has to happen in
 * a precise order lives in the frame calls instead, once:
 *
 *   while (Engine.PumpMessages())
 *   {
 *       const float Dt = Engine.BeginFrame();   // clock, input, GPU frame slot, UI frame
 *       ... UI, simulation, extraction (host) ...
 *       Engine.RenderFrame(Views);              // record, submit, present
 *   }
 */
class FRT_CORE_API CEngine
{
public:
	CEngine ();
	~CEngine ();
	CEngine (const CEngine&) = delete;
	CEngine& operator= (const CEngine&) = delete;

	bool StartUp (const SEngineDesc& Desc);
	void ShutDown ();

	// ---- frame ----
	// Dispatches every pending OS message. False once the application should quit.
	bool PumpMessages ();
	// Ticks the real-time clock, updates input, waits for this frame's GPU slot and opens the
	// UI frame. Returns the real time elapsed since the previous frame, in seconds.
	float BeginFrame ();
	// Records the views, then the UI over them, submits and presents. With no back buffer to
	// present to (minimised), only closes the UI frame.
	void RenderFrame (std::span<const SViewToRender> Views);
	void RequestQuit (int ExitCode = 0);
	int GetExitCode () const { return ExitCode; }
	uint64 GetFrameIndex () const { return FrameIndex; }

	// ---- services ----
	config::CConfig& GetConfig () { return Config; }
	SUserSettings& GetUserSettings () { return UserSettings; }
	const graphics::SDisplayOptions& GetDisplayOptions () const { return DisplayOptions; }
	CTimer& GetClock () { return *Clock; }
	input::CInputSystem& GetInput () { return Input; }
	input::CInputActionLibrary& GetInputActions () { return InputActions; }
	// The renderer's library, or a standalone one when headless.
	graphics::CMaterialLibrary& GetMaterials ();

	// Window, renderer and UI: null when headless.
	bool HasGraphics () const { return Graphics != nullptr; }
	CWindow* GetWindow ();
	graphics::CRenderer* GetRenderer ();
	CImGuiLayer* GetUi ();

	// Applies UserSettings.DisplaySettings to the window and swap chain (the DisplaySettings
	// panel's Apply), and saves them.
	void ApplyDisplaySettings ();

private:
	void OnWindowResize ();
	void OnLoseFocus ();
	void OnMinimize ();
	void OnRestoreFromMinimize ();
	bool IsExclusiveFullscreen () const;

	SEngineDesc Desc;
	bool bStarted = false;
	bool bQuitRequested = false;
	int ExitCode = 0;
	uint64 FrameIndex = 0u;

	// Declaration order is destruction order in reverse: the pool outlives everything that
	// allocates from it. Config and user settings depend only on the standard library.
	config::CConfig Config;
	SUserSettings UserSettings;
	memory::CMemoryPool MemoryPool;
	std::unique_ptr<CTimer> Clock;
	// Window, renderer, UI. One opaque block defined in Engine.cpp: headless builds compile none
	// of them, yet the class layout must not depend on FRT_HEADLESS.
	struct SGraphics;
	std::unique_ptr<SGraphics> Graphics;
	std::unique_ptr<graphics::CMaterialLibrary> HeadlessMaterials;
	graphics::SDisplayOptions DisplayOptions;
	input::CInputSystem Input;
	input::CInputActionLibrary InputActions;
};
}
