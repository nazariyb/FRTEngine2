#include "GameInstance.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

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

/**
 * SRV descriptor allocator handed to the ImGui DX12 backend.
 *
 * Since 1.92 the backend creates and destroys textures at runtime — the font atlas is
 * rasterized on demand and re-created whenever it grows or the requested size changes —
 * so descriptors must be recycled. DX12_DescriptorHeap only bumps, so freed handle pairs
 * are parked here and handed back before the heap advances. The backend defers destruction
 * until a texture has been unused for NumFramesInFlight frames, so a recycled slot is never
 * still referenced by an in-flight command list.
 *
 * Storage is fixed rather than a TArray: this lives in static storage, and TArray would free
 * through the primary CMemoryPool, which GameInstance already destroyed by then.
 */
static constexpr uint32 gImGuiSrvHeapCapacity = 64;

struct FImGuiSrvDescriptorAllocator
{
	struct FSlot
	{
		D3D12_CPU_DESCRIPTOR_HANDLE Cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE Gpu;
	};

	graphics::DX12_DescriptorHeap* Heap = nullptr;
	FSlot FreeSlots[gImGuiSrvHeapCapacity] = {};
	uint32 FreeCount = 0;

	void Allocate (D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
	{
		if (FreeCount > 0)
		{
			const FSlot Recycled = FreeSlots[--FreeCount];

			if (OutCpuHandle) *OutCpuHandle = Recycled.Cpu;
			if (OutGpuHandle) *OutGpuHandle = Recycled.Gpu;
			return;
		}

		Heap->Allocate(OutCpuHandle, OutGpuHandle);
	}

	void Free (D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
	{
		frt_assert(FreeCount < gImGuiSrvHeapCapacity);
		FreeSlots[FreeCount++] = FSlot{ CpuHandle, GpuHandle };
	}

	void Reset ()
	{
		Heap = nullptr;
		FreeCount = 0;
	}
};

static FImGuiSrvDescriptorAllocator gImGuiSrvAllocator;

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
	MemoryPool = memory::CMemoryPool(2_Gb);
	MemoryPool.MakeThisPrimaryInstance();

	Timer = new CTimer;

	SWindowParams windowParams;
	windowParams.StartX = 1000;
	windowParams.StartY = 1000;
	windowParams.Width = 1260;
	windowParams.Height = 720;
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

	Renderer = MemoryPool.NewUnique<CRenderer>(Window);
	Renderer->Resize(UserSettings.DisplaySettings.FullscreenMode == EFullscreenMode::Fullscreen);
	DisplayOptions = graphics::GetDisplayOptions(Renderer->GetAdapter());

	Camera = memory::NewShared<CCamera>();
	CameraInitialTransform.SetTranslation(4.9f, 2.2f, -1.5f);
	CameraInitialTransform.SetRotation(-0.038f, -1.356f, .0f);
	Camera->Transform = CameraInitialTransform;
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

	// Dedicated heap for ImGui — small (font atlas + user textures). Isolated from engine's
	// ShaderDescriptorHeap so engine-side rebuilds on material growth never disturb ImGui.
	// Slots are recycled by gImGuiSrvAllocator, so this only caps live textures, not churn.
	ImGuiDescriptorHeap = graphics::DX12_DescriptorHeap(
		Renderer->GetDevice(),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		gImGuiSrvHeapCapacity,
		D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	gImGuiSrvAllocator.Heap = &ImGuiDescriptorHeap;

	gImGuiDx12InitInfo = ImGui_ImplDX12_InitInfo();
	gImGuiDx12InitInfo.Device = Renderer->GetDevice();
	gImGuiDx12InitInfo.CommandQueue = Renderer->GetCommandQueue();
	gImGuiDx12InitInfo.NumFramesInFlight = render::constants::FrameResourcesBufferCount;
	gImGuiDx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.DSVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.UserData = &gImGuiSrvAllocator;
	gImGuiDx12InitInfo.SrvDescriptorHeap = ImGuiDescriptorHeap.GetHeap();
	gImGuiDx12InitInfo.SrvDescriptorAllocFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			((FImGuiSrvDescriptorAllocator*)InitInfo->UserData)->Allocate(OutCpuHandle, OutGpuHandle);
		};
	gImGuiDx12InitInfo.SrvDescriptorFreeFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
		{
			((FImGuiSrvDescriptorAllocator*)InitInfo->UserData)->Free(CpuHandle, GpuHandle);
		};
	ImGui_ImplDX12_Init(&gImGuiDx12InitInfo);
#endif
}

GameInstance::~GameInstance ()
{
#if !defined(FRT_HEADLESS)
	// Shutdown destroys ImGui's textures, which frees their descriptors back into the
	// allocator. Drop them — they point into a heap that is about to die, and the allocator
	// is a static that would otherwise hand them out again on a subsequent init.
	ImGui_ImplDX12_Shutdown();
	gImGuiSrvAllocator.Reset();

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
	std::cout << std::filesystem::current_path() << std::endl;

	// Headless has no renderer, so materials are resolved through a standalone library.
	// Same split as SRenderModel::LoadFromFile — see Graphics/Model.cpp.
#ifndef FRT_HEADLESS
	CMaterialLibrary& materialLibrary = Renderer->GetMaterialLibrary();
#else
	CMaterialLibrary materialLibrary;
#endif

	std::filesystem::path floorMaterialPath =
		std::filesystem::path("../Core/Content/Models/Floor") / ("floor_mat" + std::to_string(0) + ".frtmat.yml");
	auto floor = World.SpawnEntity("Floor");
	floor->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateGrid(10.f, 10.f, 16u, 16u),
			materialLibrary.LoadOrCreateMaterial(floorMaterialPath, {})));
	floor->bRayTraced = true;
	floor->Transform.SetTranslation(0.f, -1.f, 0.f);

	auto floorEnt = World.GetEcsWorld().Spawn();
	World.GetEcsWorld().Add<Comp_LocalTransform>(floorEnt);
	World.GetEcsWorld().Add<Comp_WorldTransform>(floorEnt);
	World.GetEcsWorld().Add<Comp_Name>(floorEnt, "Floor");

	std::filesystem::path pillarMaterialPath =
		std::filesystem::path("../Core/Content/Models/Pillar") / ("pillar_mat" + std::to_string(0) + ".frtmat.yml");
	auto pillar = World.SpawnEntity("Pillar");
	pillar->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(.65f, 1.8f, .65f), 1),
			// mesh::GenerateCylinder(0.65f, 0.65f, 1.8f, 20u, 2u),
			materialLibrary.LoadOrCreateMaterial(pillarMaterialPath, {})));
	pillar->bRayTraced = true;
	pillar->Transform.SetTranslation(-2.5f, -0.2f, 0.f);

	auto mirror = World.SpawnEntity("Mirror");
	mirror->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateQuad(3.f, 3.f),
			materialLibrary.LoadOrCreateMaterial(pillarMaterialPath, {})));
	mirror->Transform.SetTranslation(3.f, 1.f, -.5f);
	mirror->Transform.SetRotation(0.f, 0.f, math::PI_OVER_TWO);

	std::filesystem::path cubeMaterialPath =
		std::filesystem::path("../Core/Content/Models/Cube") / ("cube_mat" + std::to_string(0) + ".frtmat.yml");
	auto cube = World.SpawnEntity("Cube");
	cube->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(1.f), 1),
			materialLibrary.LoadOrCreateMaterial(cubeMaterialPath, {})));
	cube->Transform.SetTranslation(1.5f, 0.f, -1.5f);

	// Cylinder = World->SpawnEntity();
	// Cylinder->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
	// 	graphics::SRenderModel::FromMesh(mesh::GenerateCylinder(1.f, 0.5, 1.f, 10u, 10u)));

	Sphere = World.SpawnEntity("Sphere");
	Sphere->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::FromMesh(mesh::GenerateSphere(.3f, 30u, 30u)));

	auto skullEnt = World.SpawnEntity("Skull");
	skullEnt->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			R"(..\Core\Content\Models\Skull\scene.gltf)",
			R"(..\Core\Content\Models\Skull\textures\defaultMat_baseColor.jpeg)"));
	skullEnt->Transform.SetTranslation(-2.5f, 1.5f, 0.f);
	skullEnt->Transform.SetScale(Vector3f(.45f));
	skullEnt->RotationSpeed = Vector3f::UpVector * (math::PI_OVER_FOUR * 0.25f);

	auto duckEnt = World.SpawnEntity("Duck");
	duckEnt->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			R"(..\Core\Content\Models\Duck\Duck.gltf)",
			R"(..\Core\Content\Models\Duck\DuckCM.png)"));
	duckEnt->Transform.SetTranslation(0.f, 0.f, 0.f);

	// TODO: When Sponza is added, the renderer crashes. Probably multiple sections aren't handled properly
	auto sponzaEnt = World.SpawnEntity("Sponza");
	sponzaEnt->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			R"(..\Core\Content\Models\Sponza\Sponza.gltf)",
			""));
	// sponzaEnt->Transform.SetTranslation(1.f, 0.f, 0.f);
	sponzaEnt->bRayTraced = false;

	std::filesystem::path lightMaterialPath =
		std::filesystem::path("../Core/Content/Light") / ("light_mat" + std::to_string(0) + ".frtmat.yml");

	auto lightSource1 = World.SpawnEntity("Light 1");
	lightSource1->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		SRenderModel::FromMesh(mesh::GenerateSphere(.3f, 30u, 30u),
			materialLibrary.LoadOrCreateMaterial(lightMaterialPath, {})));
	lightSource1->Transform.SetTranslation(0.f, 2.f, -3.f);

	std::filesystem::path lightMaterialPath2 =
		std::filesystem::path("../Core/Content/Light") / ("light_mat" + std::to_string(2) + ".frtmat.yml");

	// auto lightSource2 = World.SpawnEntity("Light 2");
	// lightSource2->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
	// 	SRenderModel::FromMesh(mesh::GenerateQuad(1.f, 1.f),
	// 		materialLibrary.LoadOrCreateMaterial(lightMaterialPath2, {})));
	// lightSource2->Transform.SetTranslation(-2.5f, 2.85f, 0.f);
	// lightSource2->Transform.SetRotation(math::PI, 0.f, 0.f);

	graphics::SGeometryScript ceilingScript;
	using EShape = graphics::SGeometryScript::EShape;
	using EMerge = graphics::SGeometryScript::EMergeOp;

	// outer 10×6 wall
	ceilingScript.Ops.Add({ EShape::Rect, EMerge::AddBase,
		Vector2f(0.f, 0.f), Vector2f(5.1f, 3.f) });
	// window: 1×1.6 at (-1.5, 0)
	ceilingScript.Ops.Add({ EShape::Rect, EMerge::Cut,
		Vector2f(-1.5f, 0.f), Vector2f(0.5f, 0.8f) });
	// door: 0.8×2.4 at (1.5, -0.3)
	// ceilingScript.Ops.Add({ EShape::Rect, EMerge::Cut,
	// 	Vector2f(1.5f, -0.3f), Vector2f(0.4f, 1.2f) });

	auto ceiling = World.SpawnEntity("Ceiling");
	ceiling->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			graphics::mesh::BuildFromScript(ceilingScript),
			materialLibrary.LoadOrCreateMaterial(cubeMaterialPath, {})));
	ceiling->bRayTraced = true;
	ceiling->Transform.SetTranslation(-0.5f, 9.f, 0.f);
	ceiling->Transform.SetRotation(math::PI_OVER_TWO, 0.f, 0.f);
	ceiling->Transform.SetScale({ 1.5f, 1.f, 1.f });

	auto portal = World.SpawnEntity("Portal1");
	portal->Transform.SetTranslation(1.75f, 9.f, 0.f);   // window center
	portal->Transform.SetRotation(math::PI_OVER_TWO, 0.f, 0.f);   // window center
	portal->Portal = memory::NewShared<graphics::Comp_Portal>();
	portal->Portal->Normal = Vector3f(0, 0, -1);       // faces interior
	portal->Portal->Edge1  = Vector3f(0.8f, 0, 0);     // half-width
	portal->Portal->Edge2  = Vector3f(0, 0.8f, 0);     // half-height

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
		const ERenderMode NewRenderMode = enum_::NextValue(Renderer->GetRenderMode());

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
			RtSettings.SampleCount          = c.SampleCount;
			RtSettings.MaxBounces           = c.MaxBounces;
			RtSettings.RussianRouletteDepth = c.RussianRouletteDepth;
			RtSettings.bPortalPreFilter     = c.bPortalPreFilter;
			RtSettings.bCollectCounters     = true; // counters mandatory during a session
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
			RtSettings = SavedRtSettings;
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

	UpdateEntities(DeltaSeconds);
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

	Renderer->StartFrame();

	World.SubmitFrame(Renderer->GetCommandList());

	Renderer->PrepareCurrentPass();

	ImGui::Render();
	{
		// Bind ImGui's dedicated heap before its draw. ImGui_ImplDX12_RenderDrawData binds
		// its own heap internally too, but doing it here keeps state explicit and avoids
		// surprises if engine code later expects a particular heap bound after ImGui.
		ID3D12DescriptorHeap* heaps[] = { ImGuiDescriptorHeap.GetHeap() };
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

// UI panel bodies (DrawUI, DrawStatsPanel, DrawRaytracingPanel, DrawSkyPanel,
// DrawEditorPanel, DrawDisplaySettingsPanel) live in GameInstanceUI.cpp.
#endif

void GameInstance::UpdateEntities (float DeltaSeconds)
{
	if (World.IsPhasePaused(EUpdatePhase::Update))
	{
		return;
	}

	static float Angle = 0.0f;
	static float VerticalTime = 0.0f;

	Angle += 1.0f * DeltaSeconds;
	VerticalTime += DeltaSeconds;

	// Loop Angle to keep it within [0, 2PI]
	if (Angle > math::PI * 2.0f)
	{
		Angle -= math::PI * 2.0f;
	}

	float Radius = 1.0f;
	float Height = std::sin(VerticalTime * 2.0f) * 0.5f; // Oscillates between -0.5 and 0.5

	if (Cube)
	{
		Vector3f CubePos;
		CubePos.x = (Radius + 0.6f) * std::sin(Angle) + 1.f;
		CubePos.y = Height;
		CubePos.z = (Radius + 0.6f) * std::cos(Angle);
		Cube->Transform.SetTranslation(CubePos);
	}

	if (Sphere)
	{
		Vector3f SpherePos;
		SpherePos.x = (Radius + 1.0f) * std::sin(-Angle) + 1.f;
		SpherePos.y = -Height;
		SpherePos.z = (Radius + 1.0f) * std::cos(-Angle);
		Sphere->Transform.SetTranslation(SpherePos);
	}

	if (Cylinder)
	{
		Vector3f CylinderPos;
		CylinderPos.x = (Radius - .5f) * std::sin(-Angle) + 1.f;
		CylinderPos.y = -Height + 0.1f;
		CylinderPos.z = (Radius - .5f) * std::cos(-Angle);
		Cylinder->Transform.SetTranslation(CylinderPos);
	}
}

NAMESPACE_FRT_END
