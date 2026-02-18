#include "GameInstance.h"

#include <filesystem>
#include <iostream>

#include "Exception.h"
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
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

static void RebuildImGuiDx12Backend (graphics::CRenderer* renderer)
{
	if (!renderer)
	{
		return;
	}

	gImGuiDx12InitInfo.Device = renderer->GetDevice();
	gImGuiDx12InitInfo.CommandQueue = renderer->GetCommandQueue();
	gImGuiDx12InitInfo.SrvDescriptorHeap = renderer->GetDescriptorHeap().GetHeap();

	ImGui_ImplDX12_Shutdown();
	ImGui_ImplDX12_Init(&gImGuiDx12InitInfo);
}
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

	World = MemoryPool.NewUnique<CWorld>(Renderer.GetWeak());

	Camera = memory::NewShared<CCamera>();
#else
	World = MemoryPool.NewUnique<CWorld>();
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

	gImGuiDx12InitInfo = ImGui_ImplDX12_InitInfo();
	gImGuiDx12InitInfo.Device = Renderer->GetDevice();
	gImGuiDx12InitInfo.CommandQueue = Renderer->GetCommandQueue();
	gImGuiDx12InitInfo.NumFramesInFlight = render::constants::FrameResourcesBufferCount;
	gImGuiDx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.DSVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	gImGuiDx12InitInfo.UserData = Renderer.GetRawIgnoringLifetime();
	gImGuiDx12InitInfo.SrvDescriptorHeap = Renderer->ShaderDescriptorHeap.GetHeap();
	gImGuiDx12InitInfo.SrvDescriptorAllocFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			return ((CRenderer*)InitInfo->UserData)->ShaderDescriptorHeap.Allocate(OutCpuHandle, OutGpuHandle);
		};
	gImGuiDx12InitInfo.SrvDescriptorFreeFn =
		[] (ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
		{
			// TODO
		};
	ImGui_ImplDX12_Init(&gImGuiDx12InitInfo);

	Renderer->OnShaderDescriptorHeapRebuild +=
		[renderer = Renderer.GetRawIgnoringLifetime()] ()
		{
			RebuildImGuiDx12Backend(renderer);
		};
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

	std::filesystem::path cubeMaterialPath =
		std::filesystem::path("../Core/Content/Models/Cube") / ("cube_mat" + std::to_string(0) + ".frtmat");

	Cube = World->SpawnEntity();
	Cube->RenderModel.Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(1.f), 1),
			Renderer->GetMaterialLibrary().LoadOrCreateMaterial(cubeMaterialPath, {})));
	Cube->bRayTraced = false;

	// Cylinder = World->SpawnEntity();
	// Cylinder->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
	// 	graphics::SRenderModel::FromMesh(mesh::GenerateCylinder(1.f, 0.5, 1.f, 10u, 10u)));

	Sphere = World->SpawnEntity();
	Sphere->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::FromMesh(mesh::GenerateSphere(.1f, 10u, 10u)));
	Sphere->bRayTraced = false;

	// auto skullEnt = World->SpawnEntity();
	// skullEnt->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
	// 	graphics::SRenderModel::LoadFromFile(
	// 		R"(..\Core\Content\Models\Skull\scene.gltf)",
	// 		R"(..\Core\Content\Models\Skull\textures\defaultMat_baseColor.jpeg)"));
	// skullEnt->Transform.SetTranslation(1.f, 0.f, 0.f);
	// skullEnt->bRayTraced = false;

	auto duckEnt = World->SpawnEntity();
	duckEnt->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			R"(..\Core\Content\Models\Duck\Duck.gltf)",
			R"(..\Core\Content\Models\Duck\DuckCM.png)"));
	duckEnt->Transform.SetTranslation(0.f, 0.f, 0.f);
	duckEnt->bRayTraced = true;

	// TODO: When Sponza is added, the renderer crashes. Probably multiple sections aren't handled properly
	// auto sponzaEnt = World->SpawnEntity();
	// sponzaEnt->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
	// 	graphics::SRenderModel::LoadFromFile(
	// 		R"(..\Core\Content\Models\Sponza\Sponza.gltf)",
	// 		""));
	// // sponzaEnt->Transform.SetTranslation(1.f, 0.f, 0.f);
	// sponzaEnt->bRayTraced = false;

#ifndef FRT_HEADLESS
	World->InitializeRendering();
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
		Vector2f MouseDelta = InputSystem.GetMouseDelta();
		Vector3f CameraRotationVector = Vector3f::ZeroVector;
		CameraRotationVector += Vector3f::LeftVector * MouseDelta.y;
		CameraRotationVector += Vector3f::DownVector * MouseDelta.x;
		Camera->Transform.RotateBy(CameraRotationVector * DeltaSeconds * 1.f);

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
		Camera->MovementSpeed += WheelDelta * 0.5;

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
}

void GameInstance::Tick (float DeltaSeconds)
{
	++FrameCount;

#if !defined(FRT_HEADLESS)
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::ShowDemoWindow();
#endif
	CalculateFrameStats();
#if !defined(FRT_HEADLESS)
	DisplayUserSettings();

	Renderer->Tick();
	Camera->Tick(DeltaSeconds);
#endif

	UpdateEntities(DeltaSeconds);

	World->Tick(DeltaSeconds);
}

#ifndef FRT_HEADLESS
void GameInstance::Draw (float DeltaSeconds)
{
	Renderer->StartFrame();
	World->UploadCB(Renderer->GetCommandList());
	World->UpdateAccelerationStructures();
	World->Present(DeltaSeconds, Renderer->GetCommandList());
	Renderer->PrepareCurrentPass();

	ImGui::Render();
	{
		ID3D12DescriptorHeap* heaps[] = { Renderer->GetDescriptorHeap().GetHeap() };
		Renderer->GetCommandList()->SetDescriptorHeaps(_countof(heaps), heaps);
	}
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), Renderer->GetCommandList());

	Renderer->Draw();
}
#endif

long long GameInstance::GetFrameCount () const
{
	return FrameCount;
}

void GameInstance::CalculateFrameStats () const
{
	static int frameCount = 0;
	static float timeElapsed = 0.f;

	++frameCount;

	static float fps = 0.f, msPerFrame = 0.f;

	if (Timer->GetTotalSeconds() - timeElapsed >= 1.f)
	{
		fps = static_cast<float>(frameCount);
		msPerFrame = 1000.f / fps;

		frameCount = 0;
		timeElapsed += 1.f;
	}

#if !defined(FRT_HEADLESS)
	ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_NoResize);
	ImGui::Text("FPS: %.2f", fps);
	ImGui::Text("MS/frame: %.2f", msPerFrame);
	ImGui::End();
#else
	std::printf("FPS: %.2f; MS/frame: %.2f\n", fps, msPerFrame);
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

void GameInstance::DisplayUserSettings ()
{
	// TODO: this func is okay for now, but should be revisited later to at least remove reallocations on each frame

	ImGui::Begin("DisplaySettings");

	const auto strToChar = [] (void* UserData, int Idx) -> const char*
	{
		return static_cast<std::string*>(UserData)[Idx].c_str();
	};

	SDisplaySettings& displaySettings = UserSettings.DisplaySettings;

	{
		const auto monitorNames = DisplayOptions.GetNames();
		auto labelMonitor = "Monitor";
		ImGui::Combo(
			labelMonitor, &displaySettings.MonitorIndex, strToChar, (void*)monitorNames.data(),
			(int)monitorNames.size());
	}

	std::vector<uint64> resolutions = DisplayOptions.GetResolutionsEncoded(displaySettings.MonitorIndex);
	displaySettings.ResolutionIndex = math::ClampIndex(displaySettings.ResolutionIndex, resolutions.size() - 1u);

	{
		std::vector<std::string> resolutionStrs;
		resolutionStrs.reserve(resolutions.size());
		for (const auto& res : resolutions)
		{
			uint32 width, height;
			math::DecodeTwoFromOne(res, width, height);
			resolutionStrs.emplace_back(std::format("{}:{}", width, height));
		}

		auto labelResolution = "Resolution";
		ImGui::BeginDisabled(displaySettings.IsFullscreen());
		ImGui::Combo(
			labelResolution, &displaySettings.ResolutionIndex, strToChar, resolutionStrs.data(),
			(int)resolutionStrs.size());
		ImGui::EndDisabled();
	}

	std::vector<uint64> refreshRates = DisplayOptions.GetRefreshRatesEncoded(
		displaySettings.MonitorIndex, resolutions[displaySettings.ResolutionIndex]);
	{
		displaySettings.RefreshRateIndex = math::ClampIndex(displaySettings.RefreshRateIndex, refreshRates.size() - 1u);

		std::vector<std::string> rRStrs;
		rRStrs.reserve(refreshRates.size());
		for (const auto& rr : refreshRates)
		{
			uint32 numerator, denominator;
			math::DecodeTwoFromOne(rr, numerator, denominator);
			rRStrs.emplace_back(std::format("{:.2f}", (float)numerator / (float)denominator));
		}

		auto labelRR = "RefreshRate";
		ImGui::BeginDisabled(!displaySettings.IsFullscreen());
		ImGui::Combo(labelRR, &displaySettings.RefreshRateIndex, strToChar, rRStrs.data(), (int)rRStrs.size());
		ImGui::EndDisabled();
	}

	{
		const char* modeNames[] = { "Minimized", "Fullscreen", "Windowed", "Borderless" };
		auto labelFullscreen = "Fullscreen";
		ImGui::SliderInt(
			labelFullscreen,
			(int*)&displaySettings.FullscreenMode,
			1, (int32)EFullscreenMode::Borderless,
			modeNames[(int32)displaySettings.FullscreenMode],
			ImGuiSliderFlags_AlwaysClamp);
	}

	{
		const auto renderModeNames = enum_::GetValueNames<ERenderMode>();
		auto labelRenderMode = "RenderMode";
		ImGui::SliderInt(
			labelRenderMode,
			(int*)&displaySettings.RenderMode,
			0, (int32)ERenderMode::Count - 1,
			renderModeNames[(int32)displaySettings.RenderMode].data(),
			ImGuiSliderFlags_AlwaysClamp);
	}

	{
		auto labelVSync = "Enable VSync";
		ImGui::Checkbox(labelVSync, &displaySettings.bVSync);
	}

	if (ImGui::Button("Apply"))
	{
		if (displaySettings.IsFullscreen())
		{
			const uint64 fullscreenResolution = DisplayOptions.GetFullscreenResolutionEncoded(
				displaySettings.MonitorIndex);
			const auto resIt = std::ranges::find(resolutions, fullscreenResolution);
			auto resIndex = std::distance(resolutions.begin(), resIt);
			resIndex = math::ClampIndex(resIndex, resolutions.size() - 1u);
			displaySettings.ResolutionIndex = resIndex;
		}

		GetRenderer()->SetRenderMode(displaySettings.RenderMode);
		GetRenderer()->bVSyncEnabled = displaySettings.bVSync;

		uint32 numerator = 0;
		uint32 denominator = 0;
		math::DecodeTwoFromOne(refreshRates[displaySettings.RefreshRateIndex], numerator, denominator);
		GetRenderer()->DisplayRefreshRate = { numerator, denominator };

		Window->SetDisplaySettings(displaySettings, DisplayOptions);
	}

	ImGui::End();
}
#endif

void GameInstance::UpdateEntities (float DeltaSeconds)
{
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
