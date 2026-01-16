#include "GameInstance.h"

#include <filesystem>
#include <iostream>

#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/Renderer.h"
#include "Memory/Memory.h"

#include "imgui.h"
#include "Graphics/MeshGeneration.h"
#include "Graphics/Model.h"
#include "Graphics/Render/GraphicsUtility.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

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

GameInstance::GameInstance()
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

	Renderer = MemoryPool.NewUnique<CRenderer>(Window);
	Renderer->Resize(UserSettings.DisplaySettings.FullscreenMode == EFullscreenMode::Fullscreen);
	DisplayOptions = graphics::GetDisplayOptions(Renderer->GetAdapter());

	World = MemoryPool.NewUnique<CWorld>(Renderer.GetWeak());

	Camera = memory::NewShared<CCamera>();
#else
	World = MemoryPool.NewUnique<CWorld>();
#endif

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

GameInstance::~GameInstance()
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

CTimer& GameInstance::GetTime() const
{
	return *Timer;
}

bool GameInstance::HasGraphics() const
{
#if !defined(FRT_HEADLESS)
	return !!Renderer;
#else
	return false;
#endif
}

#if !defined(FRT_HEADLESS)
memory::TRefWeak<graphics::CRenderer> GameInstance::GetGraphics () const
{
	frt_assert(Renderer);
	return Renderer.GetWeak();
}
#endif

void GameInstance::Load()
{
	std::cout << std::filesystem::current_path() << std::endl;

	Cube = World->SpawnEntity();
	Cube->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::FromMesh(mesh::GenerateCube(Vector3f(.3f), 1)));
	// // World->SpawnEntity()->Mesh = mesh::GenerateGeosphere(1.f, 2u);
	Cylinder = World->SpawnEntity();
	Cylinder->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::FromMesh(mesh::GenerateCylinder(1.f, 0.5, 1.f, 10u, 10u)));

	Sphere = World->SpawnEntity();
	Sphere->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::FromMesh(mesh::GenerateSphere(.1f, 10u, 10u)));
	// World->SpawnEntity()->Mesh = mesh::GenerateGrid(1.f, 1.f, 10u, 10u);
	// World->SpawnEntity()->Mesh = mesh::GenerateGrid(1.f, 1.f, 10u, 10u);
	// World->SpawnEntity()->Mesh = mesh::GenerateQuad(1.f, 1.f);
	auto skullEnt = World->SpawnEntity();
	skullEnt->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(graphics::SRenderModel::LoadFromFile(
		R"(..\Core\Content\Models\Skull\scene.gltf)",
		R"(..\Core\Content\Models\Skull\textures\defaultMat_baseColor.jpeg)"));
	skullEnt->Transform.SetTranslation(1.f, 0.f, 0.f);
}

void GameInstance::Tick(float DeltaSeconds)
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

	Renderer->Tick(DeltaSeconds);
	Camera->Tick(DeltaSeconds);
#endif

	UpdateEntities(DeltaSeconds);

	World->Tick(DeltaSeconds);
}

#if !defined(FRT_HEADLESS)
void GameInstance::Draw(float DeltaSeconds)
{
	Renderer->StartFrame(*Camera);

	// TODO: temp
	if (!bLoaded)
	{
		Load();
		bLoaded = true;
	}

	World->Present(DeltaSeconds, Renderer->GetCommandList());

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), Renderer->GetCommandList());

	Renderer->Draw(DeltaSeconds, *Camera);
}
#endif

long long GameInstance::GetFrameCount() const
{
	return FrameCount;
}

void GameInstance::CalculateFrameStats() const
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
void GameInstance::OnWindowResize()
{
	Renderer->Resize(UserSettings.DisplaySettings.IsFullscreen());
}

void GameInstance::OnLoseFocus()
{}

void GameInstance::OnGainFocus()
{}

void GameInstance::OnMinimize()
{
	if (UserSettings.DisplaySettings.IsFullscreen())
	{
		Renderer->Resize(false);
		Timer->Pause();
	}
}

void GameInstance::OnRestoreFromMinimize()
{
	Window->SetDisplaySettings(UserSettings.DisplaySettings, DisplayOptions);
	if (UserSettings.DisplaySettings.IsFullscreen())
	{
		Timer->Start();
	}
}

void GameInstance::DisplayUserSettings()
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

	{
		std::vector<uint64> refreshRates = DisplayOptions.GetRefreshRatesEncoded(
			displaySettings.MonitorIndex, resolutions[displaySettings.ResolutionIndex]);
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
		ImGui::Combo(labelRR, &displaySettings.RefreshRateIndex, strToChar, rRStrs.data(), (int)rRStrs.size());
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
		CubePos.x = Radius * std::sin(Angle) + 1.f;
		CubePos.y = Height;
		CubePos.z = Radius * std::cos(Angle);
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
