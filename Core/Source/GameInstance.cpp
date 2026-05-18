#include "GameInstance.h"

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
#include "Graphics/Camera.h"
#include "Graphics/GeometryScript.h"
#include "Graphics/MeshGeneration.h"
#include "Graphics/Model.h"
#include "Graphics/Render/GraphicsUtility.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Graphics/Render/Renderer.h"
#include "Memory/Memory.h"

NAMESPACE_FRT_START
FRT_SINGLETON_DEFINE_INSTANCE(GameInstance)

using namespace graphics;
using namespace memory::literals;

#if !defined(FRT_HEADLESS)
static ImGui_ImplDX12_InitInfo gImGuiDx12InitInfo;
#endif

static std::filesystem::path ResolveInputContentRoot ()
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
		std::filesystem::path candidate = dir / "Core" / "Content" / "Input";
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

	std::filesystem::path fallback = current / "Core" / "Content" / "Input";
	std::filesystem::path absolute = std::filesystem::absolute(fallback, ec);
	return ec ? fallback : absolute;
}

static std::filesystem::path GetDefaultInputMapPath ()
{
	return ResolveInputContentRoot() / "IAM_Editor.frtinputmap";
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

	World.Initialize();
	MeshRenderer = World.MeshRenderer.GetWeak();

	Camera = memory::NewShared<CCamera>();
	CameraInitialTransform.SetTranslation(4.9f, 2.2f, -1.5f);
	CameraInitialTransform.SetRotation(-0.038f, -1.356f, .0f);
	Camera->Transform = CameraInitialTransform;

#else
	World = MemoryPool.NewUnique<Sys_MeshRenderer>();
#endif

	ActiveActionMap = InputActionLibrary.LoadOrCreateActionMap(GetDefaultInputMapPath());

#if !defined(FRT_HEADLESS)
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	ImGui_ImplWin32_Init(Window->GetHandle());

	// Dedicated heap for ImGui — small (font + few spares). Isolated from engine's
	// ShaderDescriptorHeap so engine-side rebuilds on material growth never disturb ImGui.
	ImGuiDescriptorHeap = graphics::DX12_DescriptorHeap(
		Renderer->GetDevice(),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		8,
		D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	gImGuiDx12InitInfo = ImGui_ImplDX12_InitInfo();
	gImGuiDx12InitInfo.Device = Renderer->GetDevice();
	gImGuiDx12InitInfo.CommandQueue = Renderer->GetCommandQueue();
	gImGuiDx12InitInfo.NumFramesInFlight = render::constants::FrameResourcesBufferCount;
	gImGuiDx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.DSVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.UserData = &ImGuiDescriptorHeap;
	gImGuiDx12InitInfo.SrvDescriptorHeap = ImGuiDescriptorHeap.GetHeap();
	gImGuiDx12InitInfo.SrvDescriptorAllocFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			((graphics::DX12_DescriptorHeap*)InitInfo->UserData)->Allocate(OutCpuHandle, OutGpuHandle);
		};
	gImGuiDx12InitInfo.SrvDescriptorFreeFn =
		[] (ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
		{
			// Heap is ring/bump-allocated and lives for the GameInstance lifetime — nothing to free per-slot.
		};
	ImGui_ImplDX12_Init(&gImGuiDx12InitInfo);
#endif
}

GameInstance::~GameInstance ()
{
#if !defined(FRT_HEADLESS)
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
	std::cout << std::filesystem::current_path() << std::endl;

	std::filesystem::path floorMaterialPath =
		std::filesystem::path("../Core/Content/Models/Floor") / ("floor_mat" + std::to_string(0) + ".frtmat.yml");
	auto floor = World.SpawnEntity("Floor");
	floor->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateGrid(10.f, 10.f, 16u, 16u),
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(floorMaterialPath, {})));
	floor->bRayTraced = true;
	floor->Transform.SetTranslation(0.f, -1.f, 0.f);

	std::filesystem::path pillarMaterialPath =
		std::filesystem::path("../Core/Content/Models/Pillar") / ("pillar_mat" + std::to_string(0) + ".frtmat.yml");
	auto pillar = World.SpawnEntity("Pillar");
	pillar->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(.65f, 1.8f, .65f), 1),
			// mesh::GenerateCylinder(0.65f, 0.65f, 1.8f, 20u, 2u),
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(pillarMaterialPath, {})));
	pillar->bRayTraced = true;
	pillar->Transform.SetTranslation(-2.5f, -0.2f, 0.f);

	auto mirror = World.SpawnEntity("Mirror");
	mirror->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateQuad(3.f, 3.f),
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(pillarMaterialPath, {})));
	mirror->Transform.SetTranslation(3.f, 1.f, -.5f);
	mirror->Transform.SetRotation(0.f, 0.f, math::PI_OVER_TWO);

	std::filesystem::path cubeMaterialPath =
		std::filesystem::path("../Core/Content/Models/Cube") / ("cube_mat" + std::to_string(0) + ".frtmat.yml");
	auto cube = World.SpawnEntity("Cube");
	cube->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(1.f), 1),
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(cubeMaterialPath, {})));
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
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(lightMaterialPath, {})));
	lightSource1->Transform.SetTranslation(0.f, 2.f, -3.f);

	std::filesystem::path lightMaterialPath2 =
		std::filesystem::path("../Core/Content/Light") / ("light_mat" + std::to_string(2) + ".frtmat.yml");

	// auto lightSource2 = World.SpawnEntity("Light 2");
	// lightSource2->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
	// 	SRenderModel::FromMesh(mesh::GenerateQuad(1.f, 1.f),
	// 		Renderer->GetMaterialLibrary().LoadOrCreateMaterial(lightMaterialPath2, {})));
	// lightSource2->Transform.SetTranslation(-2.5f, 2.85f, 0.f);
	// lightSource2->Transform.SetRotation(math::PI, 0.f, 0.f);

	graphics::SGeometryScript ceilingScript;
	using EShape = graphics::SGeometryScript::EShape;
	using EMerge = graphics::SGeometryScript::EMergeOp;

	// outer 10×6 wall
	ceilingScript.Ops.Add({ EShape::Rect, EMerge::AddBase,
		Vector2f(0.f, 0.f), Vector2f(5.f, 3.f) });
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
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(cubeMaterialPath, {})));
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
		if (ProfilingSession.ConsumeFinished())
		{
			RtSettings = SavedRtSettings;
			World.bAccumulationDirty = true;
		}
	}

	DrawUI();

	Renderer->Tick();
	Camera->Tick(DeltaSeconds);
#endif

	UpdateEntities(DeltaSeconds);
	World.RunFrame();
}

#ifndef FRT_HEADLESS
void GameInstance::Draw (float DeltaSeconds)
{
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
