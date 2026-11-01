#pragma once

#include <filesystem>
#include <memory>
#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Timer.h"
#include "Config/Config.h"
#include "Containers/String.h"
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

namespace frt::assets
{
class CAssetRegistry;
class CDerivedDataCache;
class CFileWatcherService;
class CMountTable;
}

namespace frt::graphics
{
class CMaterialLibrary;
class CRenderer;
}

namespace frt::jobs
{
class CWorkerPool;
}

namespace frt::platform
{
class CExclusiveFileLock;
}


namespace frt
{
struct SEngineDesc
{
	FString WindowTitle = "FRTEngine";
	// ImGui layout file. Empty: imgui.ini in the working directory (the project directory).
	std::filesystem::path UiIniPath;
	// Start with the window maximized instead of at the configured resolution.
	bool bMaximizeWindow = false;
	// The user's display settings may ask for DXGI exclusive fullscreen; false opens windowed
	// instead (tools: exclusive mode minimises on every focus change).
	bool bAllowExclusiveFullscreen = true;
	// A tool host (AssetTool), which runs no frame: StartUp brings up components, config (+ command
	// line, user settings), memory and the worker pool - the first steps, in the one order every host
	// shares - and nothing after them: no derived-data cache (a tool opens the store it is pointed at,
	// and must not create one to look), clock, window, renderer, materials or UI. It prints nothing on
	// standard output, which is the tool's (a --json stream). The frame calls and the services those
	// steps would have made are not for it.
	bool bToolHost = false;
	// This host writes the project's registry files when it can (design §5.9, the M3 lock deviation): StartUp takes
	// <project>/Local/Assets/Owner.lock and, holding it, lets the registry write its snapshot, journal, Rules.cache
	// and ProjectId. Only the editor sets it, when it opens a project; [Assets] ReadOnly=1 (every scripted run) skips
	// the attempt. Another process holding the lock is said once, and this host then writes nothing.
	bool bAssetWriter = false;
};

// One view to render this frame and the camera to render it from.
struct SViewToRender
{
	graphics::CRenderView* View = nullptr;
	graphics::SCameraView Camera;
};

/**
 * The engine's services and the frame they share, for the hosts that drive them (the runtime
 * launcher and the editor; AssetTool starts its first steps only, SEngineDesc::bToolHost).
 *
 * Owns every subsystem and brings them up and down explicitly, in dependency order (Gregory,
 * GEA "Subsystem Start-Up and Shut-Down") - never through static construction order:
 *   StartUp:  components, config (+ command line, user settings), memory, worker pool, derived-data
 *             cache, file watcher, mounts and asset registry (dev builds), clock, window, renderer, input
 *             actions, UI; a tool host stops after the worker pool (SEngineDesc::bToolHost).
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
	// Ticks the real-time clock, updates input, pumps the file watcher (dev builds), runs the worker
	// completions that have arrived, waits
	// for this frame's GPU slot and opens the UI frame. Returns the real time elapsed since the
	// previous frame, in seconds.
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
	// The engine's worker threads (Jobs/WorkerPool.h), owned by the main thread: BeginFrame pumps
	// their completions. Headless too. Valid from StartUp to ShutDown.
	jobs::CWorkerPool& GetWorkerPool () { return *WorkerPool; }
	// The machine's derived-data cache (Assets/DerivedDataCache.h; FRT_DDC_DIR, else [DerivedDataCache]
	// Path=, else the per-user data directory), which the renderer's shader cache reads and fills. Null
	// in Release, which builds nothing at run time, in headless builds, which have no shaders, and when
	// the store could not be opened (said once at start-up; everything is then built as if it were
	// empty). Valid from StartUp to ShutDown.
	assets::CDerivedDataCache* GetDerivedDataCache () { return DerivedData.get(); }
	// The one file watcher (Assets/FileWatcherService.h): what reloads materials, input actions, shaders
	// and the editor's INI files subscribes to, instead of polling. BeginFrame pumps it, so callbacks run
	// on the main thread during BeginFrame (before the input actions are evaluated). In Debug and
	// Development, headless too, and never null there - [Assets] Watch=0 keeps it from watching
	// anything (CFileWatcherService::SetEnabled). Null in Release, which reloads nothing, and in tool
	// hosts. Valid from StartUp until ShutDown destroys it, right after the renderer: subscribers
	// destroyed later (CEngine's own members, the hosts' objects) unsubscribe before ShutDown or just
	// drop their handles.
	assets::CFileWatcherService* GetFileWatcher () { return FileWatcher.get(); }
	// The asset registry (Assets/AssetRegistry.h): the project's mounts scanned, analyzed and kept current through the
	// file watcher, read-only in M3. Started in StartUp (its scan overlaps the window and renderer coming up) and
	// pumped in BeginFrame right after the worker completions; hosts wait for its initial scan before their first
	// BuildWorld (CAssetRegistry::WaitBeforeBuildWorld). In Debug and Development with a project, headless too; null
	// in Release, in tool hosts (AssetTool builds its own), without a project (the project manager) and with
	// [Assets] Registry=0. Never reachable from game code. Valid from StartUp until ShutDown, which ends it before
	// the file watcher and the worker pool.
	assets::CAssetRegistry* GetAssetRegistry () { return AssetRegistry.get(); }
	// Ends the asset registry now, as ShutDown would (its exit snapshot written), and lets the project's writer lock go:
	// for a host that keeps running without the project, such as the editor falling back to the project manager, whose
	// "Build and Open" starts another editor of the same project that must take the lock. GetAssetRegistry is null
	// afterwards.
	void EndAssetRegistry ();
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
	// Started after the memory pool and before the renderer (whose texture streaming submits to it),
	// stopped after the renderer.
	std::unique_ptr<jobs::CWorkerPool> WorkerPool;
	// Opened after the worker pool, closed after it: work in flight may still read or fill it until
	// the pool's drain. A member in every build, so the layout does not depend on FRT_WITH_DEV_TOOLS
	// or FRT_HEADLESS.
	std::unique_ptr<assets::CDerivedDataCache> DerivedData;
	// Dev builds only, created after the derived-data cache; destroyed in ShutDown after the renderer
	// (whose libraries subscribe) and before the worker pool. A member in every build, like DerivedData.
	std::unique_ptr<assets::CFileWatcherService> FileWatcher;
	// Dev builds: the process's mounts (installed for paths::ToMountPath while the engine runs), the project's writer
	// lock when this host holds it (SEngineDesc::bAssetWriter), and the registry, ended in ShutDown before the file
	// watcher it subscribes to. Members in every build, like FileWatcher.
	std::shared_ptr<const assets::CMountTable> MountTable;
	std::unique_ptr<platform::CExclusiveFileLock> AssetOwnerLock;
	std::unique_ptr<assets::CAssetRegistry> AssetRegistry;
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
