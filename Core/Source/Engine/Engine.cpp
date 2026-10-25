#include "Engine/Engine.h"

#include <cstdio>

#if defined(_WINDOWS)
#include <Windows.h>
#endif

#include "BuildConfig.h"
#include "EnginePaths.h"
#include "Assets/DerivedDataCache.h"
#include "ECS/CoreComponents.h"
#include "Graphics/GraphicsComponents.h"
#include "Graphics/Render/MaterialLibrary.h"
#include "Jobs/WorkerPool.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Platform.h"
#include "Profiler/CpuProfiler.h"

#if !defined(FRT_HEADLESS)
#include "backends/imgui_impl_win32.h"
#include "Window.h"
#include "Graphics/Render/GraphicsUtility.h"
#include "Graphics/Render/Renderer.h"
#include "Graphics/Render/RHI/ShaderCache.h"
#include "UI/ImGuiLayer.h"
#endif


namespace frt
{
using namespace memory::literals;

#if !defined(FRT_HEADLESS)
struct CEngine::SGraphics
{
	// Declaration order is creation order; destruction (ShutDown) is explicit and reversed.
	std::unique_ptr<CWindow> Window;
	std::unique_ptr<graphics::CRenderer> Renderer;
	std::unique_ptr<CImGuiLayer> Ui;
};
#else
struct CEngine::SGraphics {};
#endif

CEngine::CEngine () = default;

CEngine::~CEngine ()
{
	ShutDown();
}

bool CEngine::StartUp (const SEngineDesc& InDesc)
{
	frt_assert(!bStarted);
	Desc = InDesc;

	// 1. Component ids. Before any world exists: the first GetComponentId<T>() fixes a type's id
	//    for good, and constructing a CWorld touches the registry.
	RegisterCoreComponents();
	graphics::RegisterGraphicsComponents();

	// 2. Config, deliberately before the memory pool: it depends on nothing but the standard
	//    library, and everything below may read settings. Layers, weakest first: engine,
	//    project, user, then -Section:Key=Value launch arguments (never saved).
	Config.LoadDefaultLayers();
	if (const uint32 applied = Config.ApplyCommandLineArguments(platform::GetCommandLineArguments()); applied > 0u
		&& !Desc.bToolHost)
	{
		std::printf("[config] %u command-line override(s)\n", applied);
	}
	LoadUserSettings(Config, UserSettings);

	// 3. Memory: the primary pool every TArray, component pool and managed object allocates from.
	MemoryPool = memory::CMemoryPool(2_Gb);
	MemoryPool.MakeThisPrimaryInstance();

	// 4. Worker threads, owned by this (the main) thread: the renderer's texture streaming decodes on
	//    them, and BeginFrame pumps their completions.
	WorkerPool = std::make_unique<jobs::CWorkerPool>();
	if (Desc.bToolHost)
	{
		// A tool host has what it needs (SEngineDesc::bToolHost).
		bStarted = true;
		return true;
	}

	// 4b. The derived-data cache, for the renderer's shader cache - its only client - in dev builds
	//     with a renderer: Release builds nothing at run time (shaders still compile there until the
	//     cook ships DXIL, M12), and headless builds have no shaders, so they neither resolve the
	//     store nor create one. A store that cannot be opened costs speed, never correctness:
	//     everything is built as without one.
#if FRT_WITH_DEV_TOOLS && !defined(FRT_HEADLESS)
	{
		DerivedData = std::make_unique<assets::CDerivedDataCache>();
		FString error;
		if (!DerivedData->OpenDefault(&Config, &error))
		{
			log::Warning("ddc", "no derived-data cache: {}", error);
			DerivedData.reset();
		}
	}
#endif

	// 5. Real-time clock.
	Clock = std::make_unique<CTimer>();
	Clock->Reset();

#if !defined(FRT_HEADLESS)
	// 6. Window. DPI awareness must precede its creation: otherwise the process stays
	//    DPI-unaware and Windows bitmap-stretches the swap chain on scaled monitors.
	SDisplaySettings& display = UserSettings.DisplaySettings;
	if (!Desc.bAllowExclusiveFullscreen && display.FullscreenMode == EFullscreenMode::Fullscreen)
	{
		display.FullscreenMode = EFullscreenMode::Windowed;
	}
	{
		const graphics::SResolution& resolution = display.GetResolution();
		const auto& [width, height] = resolution.Get();
		SWindowParams params;
		params.StartX = 1000;
		params.StartY = 1000;
		params.Width = resolution.IsValid() ? width : 1260;
		params.Height = resolution.IsValid() ? height : 720;
		params.ClassName = "FrtWindowClass";
		params.Title = Desc.WindowTitle;
		params.hInst = GetModuleHandle(nullptr);
		ImGui_ImplWin32_EnableDpiAwareness();
		Graphics = std::make_unique<SGraphics>();
		Graphics->Window = std::make_unique<CWindow>(params);
	}
	Graphics->Window->PostResizeEvent += [this] () { OnWindowResize(); };
	Graphics->Window->PostLoseFocusEvent += [this] () { OnLoseFocus(); };
	Graphics->Window->PostMinimizeEvent += [this] () { OnMinimize(); };
	Graphics->Window->PostRestoreFromMinimizeEvent += [this] () { OnRestoreFromMinimize(); };
	Input.SetDefaultWindow(static_cast<input::WindowId>(reinterpret_cast<uintptr_t>(Graphics->Window->GetHandle())));

	// 7. Renderer, and the display modes the adapter offers. Its programs come from the shader cache:
	//    -Ddc:Shaders=0 compiles every one, as before the cache (until M2); -Ddc:Verify=<percent>
	//    compiles that share of the hits again and compares the bytes (BUILD002 on a difference).
	graphics::rhi::SShaderCacheSettings shaderCache;
	if (DerivedData && Config.Get("Ddc", "Shaders", true))
	{
		shaderCache.Cache = DerivedData.get();
	}
	shaderCache.VerifyPercent = Config.Get("Ddc", "Verify", 0.0f);
	Graphics->Renderer = std::make_unique<graphics::CRenderer>(Graphics->Window.get(), *WorkerPool, shaderCache);
	Graphics->Renderer->Resize(display.FullscreenMode == EFullscreenMode::Fullscreen);
	DisplayOptions = graphics::GetDisplayOptions(Graphics->Renderer->GetAdapter());
	// Clamp the config's monitor index before anything indexes per-monitor data with it.
	display.MonitorIndex = DisplayOptions.ClampMonitorIndex(display.MonitorIndex);
	display.ResolutionIndex = DisplayOptions.GetResolutionIndex(static_cast<uint8>(display.MonitorIndex), display.GetResolution());
	display.RefreshRateIndex = DisplayOptions.GetRefreshRateIndex(static_cast<uint8>(display.MonitorIndex), display.RefreshRateTxt);
	Graphics->Renderer->bVSyncEnabled = display.bVSync;
	if (Desc.bMaximizeWindow)
	{
		ShowWindow(Graphics->Window->GetHandle(), SW_MAXIMIZE);
	}
#else
	HeadlessMaterials = std::make_unique<graphics::CMaterialLibrary>();
#endif

	// 8. UI.
#if !defined(FRT_HEADLESS)
	Graphics->Ui = std::make_unique<CImGuiLayer>();
	Graphics->Ui->StartUp(*Graphics->Window, *Graphics->Renderer, Desc.UiIniPath);
#endif

	bStarted = true;
	return true;
}

void CEngine::ShutDown ()
{
	if (!bStarted)
	{
		return;
	}
	bStarted = false;

	// The exact reverse of StartUp, with the GPU idle first: the UI backend and the renderer
	// release resources the last frames may still be using.
#if !defined(FRT_HEADLESS)
	if (Graphics)
	{
		// A host leaving through an exception may have left the frame mid-recording.
		Graphics->Renderer->AbandonFrame();
		Graphics->Renderer->FlushCommandQueue();
		Graphics->Ui->ShutDown();
		Graphics->Ui.reset();
		Graphics->Renderer.reset();
		Graphics->Window.reset();
		Graphics.reset();
	}
#endif
	HeadlessMaterials.reset();
	Clock.reset();
	// After the renderer, whose decodes it may still be running: drains (drops what is queued, waits
	// for what runs, destroys every completion left) and joins the threads.
	WorkerPool.reset();
	// After the pool's drain: nothing can be reading or filling it any more (DerivedDataCache.h).
	DerivedData.reset();
}

bool CEngine::PumpMessages ()
{
#if !defined(FRT_HEADLESS)
	MSG msg = {};
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			bQuitRequested = true;
			ExitCode = static_cast<int>(msg.wParam);
			break;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
#endif
	return !bQuitRequested;
}

float CEngine::BeginFrame ()
{
	// The CPU profiler's frame boundary: the frame that ends here is the one the Stats panel shows.
	profiler::CCpuProfiler::BeginFrame();
	FRT_CPU_SCOPE("Engine.BeginFrame");

	++FrameIndex;
	Clock->Tick();
	const float deltaSeconds = Clock->GetDeltaSeconds();

	Input.Update(deltaSeconds);
#if FRT_WITH_DEV_TOOLS
	InputActions.ReloadModifiedActions();
	InputActions.ReloadModifiedActionMaps();
#endif
	InputActions.EvaluateActionMaps(Input);

	{
		// What the workers finished since the last frame, before the host's simulation and extraction
		// see the frame: the frame's one completion pump (waits pump as well, see Jobs/WorkerPool.h).
		FRT_CPU_SCOPE("Engine.WorkerCompletions");
		WorkerPool->PumpCompletions();
	}

#if !defined(FRT_HEADLESS)
	// Waits for this frame's GPU slot and retires what it used. Before the UI, so everything the
	// UI creates or destroys (a resized view texture) belongs to this frame's fence.
	Graphics->Renderer->BeginFrame();
	Graphics->Ui->BeginFrame(Input);
#endif
	return deltaSeconds;
}

void CEngine::RenderFrame (std::span<const SViewToRender> Views)
{
	FRT_CPU_SCOPE("Engine.RenderFrame");
#if !defined(FRT_HEADLESS)
	// Minimised (or mid display-mode change): no back buffers. Close the UI frame, skip the GPU.
	if (!Graphics->Renderer->IsReadyToRender())
	{
		Graphics->Ui->EndFrameWithoutRender();
		return;
	}
	Graphics->Renderer->BeginRecording();
	Graphics->Renderer->PrepareScene();
	for (const SViewToRender& view : Views)
	{
		Graphics->Renderer->RenderView(*view.View, view.Camera);
	}
	Graphics->Renderer->BeginUiPass();
	Graphics->Ui->Render(*Graphics->Renderer);
	Graphics->Renderer->EndFrame();
#else
	(void)Views;
#endif
}

void CEngine::RequestQuit (int InExitCode)
{
	bQuitRequested = true;
	ExitCode = InExitCode;
}

CWindow* CEngine::GetWindow ()
{
#if !defined(FRT_HEADLESS)
	return Graphics ? Graphics->Window.get() : nullptr;
#else
	return nullptr;
#endif
}

graphics::CRenderer* CEngine::GetRenderer ()
{
#if !defined(FRT_HEADLESS)
	return Graphics ? Graphics->Renderer.get() : nullptr;
#else
	return nullptr;
#endif
}

CImGuiLayer* CEngine::GetUi ()
{
#if !defined(FRT_HEADLESS)
	return Graphics ? Graphics->Ui.get() : nullptr;
#else
	return nullptr;
#endif
}

graphics::CMaterialLibrary& CEngine::GetMaterials ()
{
#if !defined(FRT_HEADLESS)
	return Graphics->Renderer->GetMaterialLibrary();
#else
	return *HeadlessMaterials;
#endif
}

bool CEngine::IsExclusiveFullscreen () const
{
	return UserSettings.DisplaySettings.IsFullscreen();
}

void CEngine::ApplyDisplaySettings ()
{
#if !defined(FRT_HEADLESS)
	SDisplaySettings& display = UserSettings.DisplaySettings;
	display.MonitorIndex = DisplayOptions.ClampMonitorIndex(display.MonitorIndex);
	const uint8 monitorIndex = static_cast<uint8>(display.MonitorIndex);
	if (!Desc.bAllowExclusiveFullscreen && display.FullscreenMode == EFullscreenMode::Fullscreen)
	{
		display.FullscreenMode = EFullscreenMode::Borderless;
	}
	if (display.IsFullscreen())
	{
		const std::span<const graphics::SResolution> resolutions = DisplayOptions.GetResolutions(monitorIndex);
		const graphics::SResolution fullscreenResolution = DisplayOptions.GetFullscreenResolution(monitorIndex);
		int32 index = 0;
		for (; index < static_cast<int32>(resolutions.size()); ++index)
		{
			if (resolutions[index] == fullscreenResolution)
			{
				break;
			}
		}
		display.ResolutionIndex = math::ClampIndex(index, resolutions.size());
	}

	Graphics->Renderer->bVSyncEnabled = display.bVSync;
	const std::span<const uint64> refreshRates = DisplayOptions.GetRefreshRates(monitorIndex, display.ResolutionIndex);
	if (!refreshRates.empty())
	{
		uint32 numerator = 0u;
		uint32 denominator = 0u;
		math::DecodeTwoFromOne(refreshRates[math::ClampIndex(display.RefreshRateIndex, refreshRates.size())], numerator, denominator);
		Graphics->Renderer->DisplayRefreshRate = { numerator, denominator };
	}

	Graphics->Window->SetDisplaySettings(display, DisplayOptions);

	// Persisted on Apply rather than on every widget edit: the panel writes straight into the
	// settings, so saving per frame would rewrite the file while a slider is being dragged.
	SaveUserSettings(Config, UserSettings, DisplayOptions);
	Config.Save();
#endif
}

void CEngine::OnWindowResize ()
{
#if !defined(FRT_HEADLESS)
	Graphics->Renderer->Resize(IsExclusiveFullscreen());
#endif
}

void CEngine::OnLoseFocus ()
{
	Input.Clear();
}

void CEngine::OnMinimize ()
{
	Input.Clear();
#if !defined(FRT_HEADLESS)
	if (IsExclusiveFullscreen())
	{
		Graphics->Renderer->Resize(false);
		Clock->Pause();
	}
#endif
}

void CEngine::OnRestoreFromMinimize ()
{
#if !defined(FRT_HEADLESS)
	// Minimising left exclusive fullscreen (OnMinimize); windowed and borderless windows simply
	// come back as they were.
	if (IsExclusiveFullscreen())
	{
		Graphics->Renderer->Resize(true);
		Clock->Start();
	}
#endif
}
}
