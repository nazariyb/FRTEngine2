#include "GameInstance.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

#if defined(_WINDOWS)
#include <Windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

#include "Exception.h"
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include "Timer.h"
#include "Window.h"
#include "ECS/Comp_Name.h"
#include "ECS/World.h"
#include "Graphics/Camera.h"
#include "Graphics/GeometryScript.h"
#include "Graphics/MeshGeneration.h"
#include "Graphics/Model.h"
#include "Graphics/Render/GraphicsUtility.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Graphics/Render/Renderer.h"
#if !defined(FRT_HEADLESS)
// Exported classes with virtuals: seeing them emits references to code headless Core omits.
#include "Graphics/Render/Passes/Denoiser.h"
#include "Graphics/Render/Passes/PathTracer.h"
#include "Graphics/Render/Passes/PostProcess.h"
#include "Graphics/Render/Passes/Upscaler.h"
#include "Graphics/Render/Scene/GpuScene.h"
#endif
#include "EnginePaths.h"
#include "Math/Comp_LocalTransform.h"
#include "Math/Comp_WorldTransform.h"
#include "Memory/Memory.h"
#include "Profiler/ProfilingExport.h"
#include "Profiler/SceneDescriptor.h"

NAMESPACE_FRT_START
FRT_SINGLETON_DEFINE_INSTANCE(GameInstance)

using namespace graphics;
using namespace memory::literals;

#if !defined(FRT_HEADLESS)
static ImGui_ImplDX12_InitInfo gImGuiDx12InitInfo;

// ImGui's font atlas and user textures live in the renderer's bindless heap: one shader-visible
// heap for the whole frame, no SetDescriptorHeaps churn between engine passes and the UI.

// UI scale is user zoom (Ctrl+Wheel) times the monitor DPI scale, driving both fonts
// (style.FontScaleMain — 1.92 rasterizes on demand, so this stays crisp) and metrics
// (ScaleAllSizes). ScaleAllSizes is not idempotent, so every re-apply starts from an
// unscaled copy of the style rather than compounding onto the live one.
static ImGuiStyle gImGuiStyleUnscaled;
static float gUiZoom = 1.0f;
static float gUiScaleApplied = 0.0f;

static constexpr float gUiZoomMin = 0.4f;
static constexpr float gUiZoomMax = 3.0f;
static constexpr float gUiZoomStepPerNotch = 1.1f;

// -Automation:TimeOfDayFrame=N / TimeOfDayValue=v: a lighting change mid-run (renderer lag tests).
static int64 gAutomationTimeOfDayFrame = -1;
static float gAutomationTimeOfDayValue = 0.5f;

static void ApplyImGuiScale (float DpiScale)
{
	const float WantedScale = gUiZoom * DpiScale;
	if (WantedScale == gUiScaleApplied)
	{
		return;
	}

	ImGuiStyle& Style = ImGui::GetStyle();
	Style = gImGuiStyleUnscaled;
	Style.ScaleAllSizes(WantedScale);
	Style.FontScaleMain = WantedScale;

	gUiScaleApplied = WantedScale;
}
#endif

static std::filesystem::path ResolveContentSubdir (const char* Subdir)
{
	std::error_code ec;
	std::filesystem::path current = std::filesystem::current_path(ec);
	if (ec)
	{
		current = ".";
	}

	std::filesystem::path dir = current;
	while (true)
	{
		std::filesystem::path candidate = dir / "Core" / "Content" / Subdir;
		if (std::filesystem::exists(candidate, ec))
		{
			std::filesystem::path absolute = std::filesystem::absolute(candidate, ec);
			return ec ? candidate : absolute;
		}

		if (!dir.has_parent_path())
		{
			break;
		}

		const std::filesystem::path parent = dir.parent_path();
		if (parent == dir)
		{
			break;
		}

		dir = parent;
	}

	std::filesystem::path fallback = current / "Core" / "Content" / Subdir;
	std::filesystem::path absolute = std::filesystem::absolute(fallback, ec);
	return ec ? fallback : absolute;
}

static std::filesystem::path GetDefaultInputMapPath ()
{
	return ResolveContentSubdir("Input") / "IAM_Editor.frtinputmap";
}

GameInstance::GameInstance ()
	: FrameCount(0)
	, World(*this)
{
	// First, and deliberately before the memory pool: config depends on nothing but the
	// standard library, and every subsystem below is allowed to read settings out of it.
	// An absent or malformed file leaves the compiled-in defaults in place - see
	// Config/IniFile.h for why the format is kept this primitive.
	Config.LoadDefaultLayers();
#if defined(_WINDOWS)
	{
		// Strongest config layer: -Section:Key=Value launch arguments (never saved).
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		std::vector<std::string> arguments;
		for (int i = 1; argv && i < argc; ++i)
		{
			std::string narrow;
			for (const wchar_t* c = argv[i]; *c; ++c)
			{
				narrow.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
			}
			arguments.push_back(std::move(narrow));
		}
		if (argv)
		{
			LocalFree(argv);
		}
		const uint32 applied = Config.ApplyCommandLineArguments(arguments);
		if (applied > 0u)
		{
			std::printf("[config] %u command-line override(s)\n", applied);
		}
	}
#endif
	LoadUserSettings(Config, UserSettings);

	MemoryPool = memory::CMemoryPool(2_Gb);
	MemoryPool.MakeThisPrimaryInstance();

	Timer = new CTimer;

	const SResolution& resolution = UserSettings.DisplaySettings.GetResolution();
	const auto& [width, height] = resolution.Get();

	SWindowParams windowParams;
	windowParams.StartX = 1000;
	windowParams.StartY = 1000;
	windowParams.Width = resolution.IsValid() ? width : 1260;
	windowParams.Height = resolution.IsValid() ? height : 720;
	windowParams.ClassName = L"FrtWindowClass";
	windowParams.hInst = GetModuleHandle(nullptr);
#if !defined(FRT_HEADLESS)
	// Must precede window creation: otherwise the process stays DPI-unaware and Windows
	// bitmap-stretches the whole swapchain on scaled monitors.
	ImGui_ImplWin32_EnableDpiAwareness();

	Window = new CWindow(windowParams);

	Window->PostResizeEvent += std::bind(&GameInstance::OnWindowResize, this);
	Window->PostLoseFocusEvent += std::bind(&GameInstance::OnLoseFocus, this);
	Window->PostGainFocusEvent += std::bind(&GameInstance::OnGainFocus, this);
	Window->PostMinimizeEvent += std::bind(&GameInstance::OnMinimize, this);
	Window->PostRestoreFromMinimizeEvent += std::bind(&GameInstance::OnRestoreFromMinimize, this);

	InputSystem.SetDefaultWindow(
		static_cast<input::WindowId>(reinterpret_cast<uintptr_t>(Window->GetHandle())));

	{
		SDisplaySettings& displaySettings = UserSettings.DisplaySettings;

		Renderer = MemoryPool.NewUnique<CRenderer>(Window);
		Renderer->Resize(displaySettings.FullscreenMode == EFullscreenMode::Fullscreen);
		DisplayOptions = graphics::GetDisplayOptions(Renderer->GetAdapter());

		displaySettings.ResolutionIndex =
			DisplayOptions.GetResolutionIndex(displaySettings.MonitorIndex, displaySettings.GetResolution());
		displaySettings.RefreshRateIndex =
			DisplayOptions.GetRefreshRateIndex(displaySettings.MonitorIndex, displaySettings.RefreshRateTxt);

		displaySettings.MonitorIndex = DisplayOptions.ClampMonitorIndex(displaySettings.MonitorIndex);
		Renderer->SetRenderMode(displaySettings.RenderMode);
		Renderer->bVSyncEnabled = displaySettings.bVSync;

		Camera = memory::NewShared<CCamera>();
		CameraInitialTransform.SetTranslation(4.9f, 2.2f, -1.5f);
		CameraInitialTransform.SetRotation(-0.038f, -1.356f, .0f);
		Camera->Transform = CameraInitialTransform;

	}
#endif

	World.Initialize();
	MeshRenderer = World.MeshRenderer.GetWeak();

	ActiveActionMap = InputActionLibrary.LoadOrCreateActionMap(GetDefaultInputMapPath());

#if !defined(FRT_HEADLESS)
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	// UI font. The size given here becomes style.FontSizeBase; the scale factors set by
	// ApplyImGuiScale() are applied on top of it, and 1.92 re-rasterizes per size, so
	// zoomed text stays sharp. AddFontFromFileTTF() asserts on a missing file, hence the
	// existence check — a stripped content dir should degrade to the built-in font.
	const std::filesystem::path UiFontPath = ResolveContentSubdir("Fonts") / "Roboto-Medium.ttf";
	std::error_code UiFontEc;
	if (std::filesystem::exists(UiFontPath, UiFontEc))
	{
		io.Fonts->AddFontFromFileTTF(UiFontPath.string().c_str(), 16.0f);
	}
	else
	{
		io.Fonts->AddFontDefault();
	}

	// Reference style at scale 1.0 — ApplyImGuiScale() rebuilds the live style from this.
	gImGuiStyleUnscaled = ImGui::GetStyle();

	ImGui_ImplWin32_Init(Window->GetHandle());

	gImGuiDx12InitInfo = ImGui_ImplDX12_InitInfo();
	gImGuiDx12InitInfo.Device = Renderer->GetDevice();
	gImGuiDx12InitInfo.CommandQueue = Renderer->GetCommandQueue();
	gImGuiDx12InitInfo.NumFramesInFlight = graphics::rhi::MaxFramesInFlight;
	gImGuiDx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
	gImGuiDx12InitInfo.UserData = Renderer.GetRawIgnoringLifetime();
	gImGuiDx12InitInfo.SrvDescriptorHeap = Renderer->GetBindlessHeap();
	gImGuiDx12InitInfo.SrvDescriptorAllocFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			static_cast<CRenderer*>(InitInfo->UserData)->AllocateImGuiDescriptor(OutCpuHandle, OutGpuHandle);
		};
	gImGuiDx12InitInfo.SrvDescriptorFreeFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
		{
			static_cast<CRenderer*>(InitInfo->UserData)->FreeImGuiDescriptor(CpuHandle, GpuHandle);
		};
	ImGui_ImplDX12_Init(&gImGuiDx12InitInfo);

	ApplyLaunchConfig();
#endif
}

GameInstance::~GameInstance ()
{
#if !defined(FRT_HEADLESS)
	// Shutdown destroys ImGui's textures, which frees their descriptors back into the
	// allocator. Drop them — they point into a heap that is about to die, and the allocator
	// is a static that would otherwise hand them out again on a subsequent init.
	Renderer->FlushCommandQueue();
	ImGui_ImplDX12_Shutdown();

	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	delete Window;
	Window = nullptr;
#endif

	delete Timer;
	Timer = nullptr;
}

CTimer& GameInstance::GetTime () const
{
	return *Timer;
}

bool GameInstance::HasGraphics () const
{
#if !defined(FRT_HEADLESS)
	return !!Renderer;
#else
	return false;
#endif
}

#if !defined(FRT_HEADLESS)
memory::TRefWeak<graphics::CRenderer> GameInstance::GetRenderer () const
{
	frt_assert(Renderer);
	return Renderer.GetWeak();
}
#endif

const input::CInputActionMap* GameInstance::GetActiveInputActionMap () const
{
	return ActiveActionMap ? &ActiveActionMap->ActionMap : nullptr;
}

input::CInputActionMap* GameInstance::GetActiveInputActionMap ()
{
	return ActiveActionMap ? &ActiveActionMap->ActionMap : nullptr;
}

void GameInstance::Load ()
{
	std::cout << "Working path: " << std::filesystem::current_path() << std::endl;
}

void GameInstance::OnLoaded ()
{
#ifndef FRT_HEADLESS
	MeshRenderer->InitializeRendering();
#endif
}

void GameInstance::Input (float DeltaSeconds)
{
	InputSystem.Update(DeltaSeconds);
#ifndef RELEASE
	InputActionLibrary.ReloadModifiedActions();
	InputActionLibrary.ReloadModifiedActionMaps();
#endif
	if (ActiveActionMap)
	{
		ActiveActionMap->ActionMap.Evaluate(InputSystem);
	}

	const bool bCtrlDown = InputSystem.IsKeyDown(input::KeyCode::LeftCtrl)
		|| InputSystem.IsKeyDown(input::KeyCode::RightCtrl);

#if !defined(FRT_HEADLESS)
	// Ctrl+Wheel zooms the whole UI. ImGui discards wheel input while Ctrl is held
	// (UpdateMouseWheel bails unless io.FontAllowUserScaling), so no ImGui window scrolls
	// underneath; Tick() turns this into style scale before the next NewFrame().
	if (bCtrlDown)
	{
		const float ZoomWheelDelta = InputSystem.GetMouseWheelDelta();
		if (ZoomWheelDelta != 0.0f)
		{
			gUiZoom = math::Clamp(
				gUiZoom * std::pow(gUiZoomStepPerNotch, ZoomWheelDelta),
				gUiZoomMin,
				gUiZoomMax);
		}
	}
#endif

	if (bCtrlDown)
	{
		// ctrl is higher prio than non-ctrl actions
		return;
	}

#ifndef FRT_HEADLESS
	input::SInputActionState* EnableMoveState = ActiveActionMap->ActionMap.FindActionState("IA_EnableMove");
	if (EnableMoveState && EnableMoveState->bDown)
	{
		// Look
		const Vector2f ViewportSize = Window->GetWindowSize();
		const float ViewportAspectRatio = math::Max(ViewportSize.x, 1.0f) / math::Max(ViewportSize.y, 1.0f);
		const Vector2f MouseDelta = InputSystem.GetMouseDelta() * Camera->RotationSpeed;

		Vector3f CameraRotation = Camera->Transform.GetRotation();
		CameraRotation += Vector3f::LeftVector * (MouseDelta.y * DeltaSeconds);
		CameraRotation += Vector3f::DownVector * ((MouseDelta.x / ViewportAspectRatio) * DeltaSeconds);

		constexpr float CameraPitchLimit = math::PI_OVER_TWO - 0.001f;
		CameraRotation.x = math::Clamp(CameraRotation.x, -CameraPitchLimit, CameraPitchLimit);
		Camera->Transform.SetRotation(CameraRotation);

		// Speed
		Vector3f CameraMoveVector = Vector3f::ZeroVector;
		input::SInputActionState* MoveForwardState = ActiveActionMap->ActionMap.FindActionState("IA_MoveForward");
		CameraMoveVector += Vector3f::ForwardVector * MoveForwardState->Value;
		input::SInputActionState* MoveLeftState = ActiveActionMap->ActionMap.FindActionState("IA_MoveLeft");
		CameraMoveVector += Vector3f::LeftVector * MoveLeftState->Value;
		input::SInputActionState* MoveUpState = ActiveActionMap->ActionMap.FindActionState("IA_MoveUp");
		CameraMoveVector += Vector3f::UpVector * MoveUpState->Value;

		// Move
		const Vector3f localMove = CameraMoveVector;

		using namespace DirectX;
		const Vector3f rot = Camera->Transform.GetRotation(); // pitch, yaw, roll (radians)
		const XMMATRIX rotM = XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z);

		const XMVECTOR v = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&localMove));
		const XMVECTOR worldMoveV = XMVector3TransformNormal(v, rotM);

		Vector3f worldMove{};
		XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&worldMove), worldMoveV);

		const float WheelDelta = InputSystem.GetMouseWheelDelta();
		Camera->MovementSpeed = math::Max(Camera->MovementSpeed + WheelDelta * 0.3f, 0.001f);

		Camera->Transform.MoveBy(worldMove * DeltaSeconds * Camera->MovementSpeed);
	}

	if (InputSystem.WasKeyPressed(input::KeyCode::F2))
	{
		// Raster preview <-> path tracing (Hybrid is an alias of path tracing).
		const ERenderMode NewRenderMode = Renderer->GetRenderMode() == ERenderMode::Raster
			? ERenderMode::Raytracing : ERenderMode::Raster;

		// TODO: should be synced automatically
		UserSettings.DisplaySettings.RenderMode = NewRenderMode;
		Renderer->SetRenderMode(NewRenderMode);
	}

	if (InputSystem.WasKeyPressed(input::KeyCode::I))
	{
		bShowImGui = !bShowImGui;
	}
#endif

	if (InputSystem.WasKeyPressed(input::KeyCode::Space) && !InputSystem.IsMouseButtonDown(input::EMouseButton::Right))
	{
		World.TogglePhasePause(EUpdatePhase::Update);
	}
}

void GameInstance::Tick (float DeltaSeconds)
{
	++FrameCount;

#if !defined(FRT_HEADLESS)
	// Picks up the zoom Input() just set, plus the monitor scale — re-queried every frame so
	// moving the window to a differently scaled monitor needs no extra plumbing. Must run
	// before NewFrame(), which is where font size and metrics are resolved for the frame.
	ApplyImGuiScale(ImGui_ImplWin32_GetDpiScaleForHwnd(Window->GetHandle()));

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::DockSpaceOverViewport(
		0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
#endif
	CalculateFrameStats();
#if !defined(FRT_HEADLESS)
	{
		const auto [mw, mh] = Window->GetWindowSize();
		Metrics.Sample(
			Renderer->GetRayCounters().GetLastTotals(),
			Renderer->GetGpuProfiler().GetLastResults(),
			static_cast<double>(DeltaSeconds) * 1000.0,
			static_cast<uint32>(mw),
			static_cast<uint32>(mh));
	}

	if (ProfilingSession.IsActive())
	{
		ProfilingSession.Update(Metrics);

		if (ProfilingSession.HasCurrent())
		{
			const profiler::SProfileConfig& c = ProfilingSession.Current();
			graphics::SPathTracerSettings& pt = Renderer->Profile.PathTracer;
			pt.SamplesPerPixel      = c.SampleCount;
			pt.MaxBounces           = c.MaxBounces;
			pt.RussianRouletteDepth = c.RussianRouletteDepth;
			pt.bPortalPreFilter     = c.bPortalPreFilter;
			Renderer->Profile.Frame.bCollectRayCounters = true; // counters mandatory during a session
		}
		if (ProfilingSession.ConsumeProfileChanged())
		{
			World.bAccumulationDirty = true;
		}
		// One file per configuration, written the moment it finishes — before the next starts.
		const int32 doneIdx = ProfilingSession.ConsumeCompletedProfile();
		if (doneIdx >= 0 && static_cast<size_t>(doneIdx) < ProfilingSession.History().size())
		{
			const profiler::SProfileResult& res =
				ProfilingSession.History()[static_cast<size_t>(doneIdx)];
			profiler::WriteConfigTxt(
				SessionDir, static_cast<uint32>(doneIdx), res.Config, SessionSceneDesc);
			profiler::WriteConfigFramesCsv(
				SessionDir, static_cast<uint32>(doneIdx), res);
		}
		if (ProfilingSession.ConsumeFinished())
		{
			EndProfilingOverride();
			World.bAccumulationDirty = true;
		}
	}

	if (bShowImGui)
	{
		DrawUI();
	}

	Renderer->Tick();
	Camera->Tick(DeltaSeconds);
#endif

	World.RunFrame(DeltaSeconds);
}

#ifndef FRT_HEADLESS
void GameInstance::Draw (float DeltaSeconds)
{
	// Minimized (or mid display-mode change): there are no back buffers. The message loop
	// keeps calling us anyway, so end the ImGui frame Tick() opened and skip the GPU work.
	if (!Renderer->IsReadyToRender())
	{
		ImGui::EndFrame();
		return;
	}

	UpdateAutomation(DeltaSeconds);

	Renderer->StartFrame();

	World.SubmitFrame(Renderer->GetCommandList());

	Renderer->PrepareCurrentPass();

	ImGui::Render();
	{
		ID3D12DescriptorHeap* heaps[] = { Renderer->GetBindlessHeap() };
		Renderer->GetCommandList()->SetDescriptorHeaps(_countof(heaps), heaps);
	}
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), Renderer->GetCommandList());

	Renderer->Draw();
}
#endif

uint64 GameInstance::GetFrameCount () const
{
	return FrameCount;
}

void GameInstance::CalculateFrameStats ()
{
	++StatFrameAccum;

	if (Timer->GetTotalSeconds() - StatTimeElapsed >= 1.f)
	{
		StatFps = static_cast<float>(StatFrameAccum);
		StatMsPerFrame = 1000.f / StatFps;

		StatFrameAccum = 0;
		StatTimeElapsed += 1.f;
	}

#if defined(FRT_HEADLESS)
	std::printf("FPS: %.2f; MS/frame: %.2f\n", StatFps, StatMsPerFrame);
#endif
}

#if !defined(FRT_HEADLESS)
void GameInstance::OnWindowResize ()
{
	Renderer->Resize(UserSettings.DisplaySettings.IsFullscreen());
}

void GameInstance::OnLoseFocus ()
{
	InputSystem.Clear();
}

void GameInstance::OnGainFocus ()
{}

void GameInstance::OnMinimize ()
{
	InputSystem.Clear();
	if (UserSettings.DisplaySettings.IsFullscreen())
	{
		Renderer->Resize(false);
		Timer->Pause();
	}
}

void GameInstance::OnRestoreFromMinimize ()
{
	Window->SetDisplaySettings(UserSettings.DisplaySettings, DisplayOptions);
	if (UserSettings.DisplaySettings.IsFullscreen())
	{
		Timer->Start();
	}
}

namespace
{
bool ParseVector3 (const std::string& Text, Vector3f& Out)
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
	if (sscanf_s(Text.c_str(), "%f,%f,%f", &x, &y, &z) == 3)
	{
		Out = Vector3f(x, y, z);
		return true;
	}
	return false;
}
}

void GameInstance::ApplyLaunchConfig ()
{
	graphics::SRenderSettings& render = Renderer->Profile.Frame;
	graphics::SPathTracerSettings& pt = Renderer->Profile.PathTracer;

	// Session-only override of the persisted [Settings.Display] RenderMode (not written back).
	const std::string renderMode = Config.Get("Render", "RenderMode", "");
	if (renderMode == "Raytracing" || renderMode == "raytracing")
	{
		Renderer->SetRenderMode(ERenderMode::Raytracing);
	}
	else if (renderMode == "Raster" || renderMode == "raster")
	{
		Renderer->SetRenderMode(ERenderMode::Raster);
	}

	const std::string mode = Config.Get("Render", "Mode", "RealTime");
	render.Mode = (mode == "Reference" || mode == "reference") ? EPathTracingMode::Reference : EPathTracingMode::RealTime;
	render.RenderScale = Config.Get("Render", "RenderScale", render.RenderScale);
	render.bDenoiser = Config.Get("Render", "Denoiser", render.bDenoiser);
	render.ReferenceSampleTarget = Config.Get("Render", "SampleTarget", render.ReferenceSampleTarget);
	render.bCollectRayCounters = Config.Get("Render", "CollectRayCounters", render.bCollectRayCounters);
	render.FieldOfViewY = Config.Get("Render", "FieldOfViewY", render.FieldOfViewY);
	const std::string upscaler = Config.Get("Render", "Upscaler", "Temporal");
	if (upscaler == "None" || upscaler == "Off")
	{
		Renderer->SetUpscalerKind(EUpscalerKind::None);
	}

	pt.SamplesPerPixel = Config.Get("Render", "Spp", pt.SamplesPerPixel);
	pt.MaxBounces = Config.Get("Render", "MaxBounces", pt.MaxBounces);
	pt.RussianRouletteDepth = Config.Get("Render", "RussianRouletteDepth", pt.RussianRouletteDepth);
	pt.RisCandidates = Config.Get("Render", "RisCandidates", pt.RisCandidates);
	pt.RisCandidatesSecondary = Config.Get("Render", "RisCandidatesSecondary", pt.RisCandidatesSecondary);
	pt.IndirectClamp = Config.Get("Render", "IndirectClamp", pt.IndirectClamp);
	pt.bShaderExecutionReordering = Config.Get("Render", "SER", pt.bShaderExecutionReordering);
	pt.bInlineRayQueries = Config.Get("Render", "InlineRT", pt.bInlineRayQueries);
	pt.bNormalMaps = Config.Get("Render", "NormalMaps", pt.bNormalMaps);
	pt.bPortalGuiding = Config.Get("Render", "PortalGuiding", pt.bPortalGuiding);
	pt.bPortalPreFilter = Config.Get("Render", "PortalPreFilter", pt.bPortalPreFilter);
	pt.bPrimarySurfaceReplacement = Config.Get("Render", "PSR", pt.bPrimarySurfaceReplacement);
	pt.bDisableNee = Config.Get("Render", "DisableNee", pt.bDisableNee);
	pt.bDisableBsdfMis = Config.Get("Render", "DisableBsdfMis", pt.bDisableBsdfMis);
	pt.bWhiteFurnace = Config.Get("Render", "WhiteFurnace", pt.bWhiteFurnace);
	pt.DebugMode = Config.Get("Render", "DebugMode", pt.DebugMode);

	graphics::SRadianceCacheSettings& rc = Renderer->Profile.RadianceCache;
	rc.bEnabled = Config.Get("Render", "RadianceCache", rc.bEnabled);
	rc.CapacityLog2 = Config.Get("Render", "RcCapacityLog2", rc.CapacityLog2);
	rc.CellScale = Config.Get("Render", "RcCellScale", rc.CellScale);
	rc.UpdateTileSize = Config.Get("Render", "RcUpdateTile", rc.UpdateTileSize);
	rc.AccumulationFrames = Config.Get("Render", "RcAccumFrames", rc.AccumulationFrames);
	rc.StaleFrames = Config.Get("Render", "RcStaleFrames", rc.StaleFrames);
	rc.RoughnessThreshold = Config.Get("Render", "RcRoughness", rc.RoughnessThreshold);
	rc.FootprintScale = Config.Get("Render", "RcFootprint", rc.FootprintScale);
	rc.MinSampleWeight = Config.Get("Render", "RcMinSamples", rc.MinSampleWeight);
	rc.SampleClamp = Config.Get("Render", "RcSampleClamp", rc.SampleClamp);
	rc.bFeedback = Config.Get("Render", "RcFeedback", rc.bFeedback);
	rc.bJitter = Config.Get("Render", "RcJitter", rc.bJitter);
	rc.FeedbackMaxDepth = Config.Get("Render", "RcFeedbackDepth", rc.FeedbackMaxDepth);
	rc.SpecularShareMax = Config.Get("Render", "RcSpecShare", rc.SpecularShareMax);
	Renderer->GetScene().bPortalWeightedLightSelection =
		Config.Get("Render", "PortalLightSelection", Renderer->GetScene().bPortalWeightedLightSelection);

	graphics::SPostProcessSettings& post = Renderer->Profile.Post;
	post.bAutoExposure = Config.Get("Render", "AutoExposure", post.bAutoExposure);
	post.ManualExposureEV = Config.Get("Render", "ExposureEV", post.ManualExposureEV);
	post.ExposureCompensationEV = Config.Get("Render", "ExposureCompensationEV", post.ExposureCompensationEV);
	post.bBloom = Config.Get("Render", "Bloom", post.bBloom);
	// [Post] keys (-Post:Key=Value): bloom and metering parameters, for A/B captures.
	post.BloomIntensity = Config.Get("Post", "BloomIntensity", post.BloomIntensity);
	post.BloomRadius = Config.Get("Post", "BloomRadius", post.BloomRadius);
	post.BloomFalloff = Config.Get("Post", "BloomFalloff", post.BloomFalloff);
	post.BloomMips = Config.Get("Post", "BloomMips", post.BloomMips);
	post.HistogramLowPercentile = Config.Get("Post", "LowPercentile", post.HistogramLowPercentile);
	post.HistogramHighPercentile = Config.Get("Post", "HighPercentile", post.HistogramHighPercentile);
	post.MeteringCenterWeight = Config.Get("Post", "CenterWeight", post.MeteringCenterWeight);
	post.ToneMapper = static_cast<graphics::EToneMapper>(Config.Get("Post", "ToneMapper", static_cast<uint32>(post.ToneMapper)));

	// [Taa] keys (-Taa:Key=Value): in-house TAA / TAAU settings, for A/B captures.
	{
		graphics::STemporalUpscalerSettings& t = Renderer->Profile.Taa;
		t.HistoryBlend = Config.Get("Taa", "HistoryBlend", t.HistoryBlend);
		t.Sharpness = Config.Get("Taa", "Sharpness", t.Sharpness);
		t.VarianceGamma = Config.Get("Taa", "VarianceGamma", t.VarianceGamma);
		t.TemporalGamma = Config.Get("Taa", "TemporalGamma", t.TemporalGamma);
		t.bAntiFlicker = Config.Get("Taa", "AntiFlicker", t.bAntiFlicker);
		t.bVarianceClip = Config.Get("Taa", "VarianceClip", t.bVarianceClip);
		t.bLanczosKernel = Config.Get("Taa", "Lanczos", t.bLanczosKernel);
		t.bCatmullRomHistory = Config.Get("Taa", "CatmullRom", t.bCatmullRomHistory);
		t.bDepthTest = Config.Get("Taa", "DepthTest", t.bDepthTest);
		t.DebugMode = Config.Get("Taa", "Debug", t.DebugMode);
	}

	// [Denoiser] keys (-Denoiser:Key=Value), parsed by the pass itself.
	graphics::CDenoiser::ApplyConfig(Config, Renderer->Profile.Denoiser);

	TimeOfDay =Config.Get("Scene", "TimeOfDay", TimeOfDay);
	if (Config.Get("Scene", "PauseWorld", false))
	{
		World.TogglePhasePause(EUpdatePhase::Update);
	}
	Vector3f camPos;
	if (ParseVector3(Config.Get("Scene", "CameraPosition", ""), camPos))
	{
		Camera->Transform.SetTranslation(camPos);
	}
	Vector3f camRot;
	if (ParseVector3(Config.Get("Scene", "CameraRotation", ""), camRot))
	{
		Camera->Transform.SetRotation(camRot);
	}
	if (Config.Has("Display", "VSync"))
	{
		Renderer->bVSyncEnabled = Config.Get("Display", "VSync", Renderer->bVSyncEnabled);
	}
	if (Config.Has("Display", "ShowUi"))
	{
		bShowImGui = Config.Get("Display", "ShowUi", bShowImGui);
	}

	AutomationCaptureFrame = Config.Get("Automation", "CaptureFrame", -1);
	AutomationExitFrame = Config.Get("Automation", "ExitFrame", -1);
	AutomationProfileFrames = Config.Get("Automation", "ProfileFrames", 0);
	bAutomationMotion = ParseVector3(Config.Get("Automation", "CameraMove", ""), AutomationCameraMove);
	bAutomationMotion = ParseVector3(Config.Get("Automation", "CameraTurn", ""), AutomationCameraTurn) || bAutomationMotion;
	AutomationMotionStartFrame = Config.Get("Automation", "MotionStartFrame", 0);
	AutomationMotionEndFrame = Config.Get("Automation", "MotionEndFrame", -1);
	gAutomationTimeOfDayFrame = Config.Get("Automation", "TimeOfDayFrame", -1);
	gAutomationTimeOfDayValue = Config.Get("Automation", "TimeOfDayValue", TimeOfDay);
	bAutomationCaptureHdr = Config.Get("Automation", "CaptureHdr", true);
	const std::string capturePath = Config.Get("Automation", "CapturePath", "");
	AutomationCapturePath = capturePath.empty() ? (paths::GetCaptureDir() / "automation") : std::filesystem::path(capturePath);
}

void GameInstance::UpdateAutomation (float DeltaSeconds)
{
	const int64 frame = static_cast<int64>(FrameCount);
	if (frame == gAutomationTimeOfDayFrame)
	{
		// Picked up by the sky panel (ComputeSkyFromTimeOfDay) on the next UI frame.
		TimeOfDay = gAutomationTimeOfDayValue;
	}
	if (bAutomationMotion && frame >= AutomationMotionStartFrame && (AutomationMotionEndFrame < 0 || frame < AutomationMotionEndFrame))
	{
		using namespace DirectX;
		const Vector3f rot = Camera->Transform.GetRotation();
		const XMMATRIX rotM = XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z);
		const XMVECTOR local = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&AutomationCameraMove));
		Vector3f worldMove{};
		XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&worldMove), XMVector3TransformNormal(local, rotM));
		Camera->Transform.MoveBy(worldMove);
		Camera->Transform.SetRotation(rot + AutomationCameraTurn);
	}

	if (AutomationCaptureFrame >= 0 && !bAutomationCaptureRequested &&
		static_cast<int64>(FrameCount) >= AutomationCaptureFrame)
	{
		Renderer->RequestCapture(AutomationCapturePath, bAutomationCaptureHdr);
		bAutomationCaptureRequested = true;
		const Vector3f camPos = Camera->Transform.GetTranslation();
		const Vector3f camRot = Camera->Transform.GetRotation();
		std::printf("[automation] capture requested at frame %llu (%u spp accumulated) camera %.6f,%.6f,%.6f rotation %.6f,%.6f,%.6f\n",
			static_cast<unsigned long long>(FrameCount), Renderer->GetAccumulatedSamples(),
			camPos.x, camPos.y, camPos.z, camRot.x, camRot.y, camRot.z);
		std::fflush(stdout);
	}
	if (AutomationProfileFrames > 0 && AutomationExitFrame >= 0 &&
		static_cast<int64>(FrameCount) >= AutomationExitFrame - AutomationProfileFrames &&
		static_cast<int64>(FrameCount) < AutomationExitFrame)
	{
		for (const profiler::SGpuScopeResult& r : Renderer->GetGpuProfiler().GetLastResults())
		{
			if (!r.Name)
			{
				continue;
			}
			auto it = std::find_if(AutomationProfile.begin(), AutomationProfile.end(),
				[&](const auto& e) { return e.first == r.Name; });
			if (it == AutomationProfile.end())
			{
				AutomationProfile.push_back({ std::string(r.Name), { 0.0, 0u } });
				it = AutomationProfile.end() - 1;
			}
			it->second.first += r.DurationMs;
			++it->second.second;
		}
		AutomationCpuFrameMs += static_cast<double>(DeltaSeconds) * 1000.0;
		++AutomationCpuFrames;
	}
	if (AutomationExitFrame >= 0 && static_cast<int64>(FrameCount) >= AutomationExitFrame &&
		!Renderer->HasPendingCaptures())
	{
		for (const auto& e : AutomationProfile)
		{
			std::printf("[profile] %-28s %8.3f ms\n", e.first.c_str(), e.second.second ? e.second.first / e.second.second : 0.0);
		}
		if (AutomationCpuFrames > 0u)
		{
			std::printf("[profile] %-28s %8.3f ms\n", "CPU frame", AutomationCpuFrameMs / AutomationCpuFrames);
		}
		std::printf("[automation] exit at frame %llu\n", static_cast<unsigned long long>(FrameCount));
		std::fflush(stdout);
		AutomationExitFrame = -1;
		PostQuitMessage(0);
	}
}

void GameInstance::BeginProfilingOverride ()
{
	const graphics::SPathTracerSettings& pt = Renderer->Profile.PathTracer;
	SavedProfilingSettings.SamplesPerPixel = pt.SamplesPerPixel;
	SavedProfilingSettings.MaxBounces = pt.MaxBounces;
	SavedProfilingSettings.RussianRouletteDepth = pt.RussianRouletteDepth;
	SavedProfilingSettings.bPortalPreFilter = pt.bPortalPreFilter;
	SavedProfilingSettings.bCollectRayCounters = Renderer->Profile.Frame.bCollectRayCounters;
}

void GameInstance::EndProfilingOverride ()
{
	graphics::SPathTracerSettings& pt = Renderer->Profile.PathTracer;
	pt.SamplesPerPixel = SavedProfilingSettings.SamplesPerPixel;
	pt.MaxBounces = SavedProfilingSettings.MaxBounces;
	pt.RussianRouletteDepth = SavedProfilingSettings.RussianRouletteDepth;
	pt.bPortalPreFilter = SavedProfilingSettings.bPortalPreFilter;
	Renderer->Profile.Frame.bCollectRayCounters = SavedProfilingSettings.bCollectRayCounters;
}

// UI panel bodies (DrawUI, DrawStatsPanel, DrawRaytracingPanel, DrawSkyPanel,
// DrawEditorPanel, DrawDisplaySettingsPanel) live in GameInstanceUI.cpp.
#endif

NAMESPACE_FRT_END
