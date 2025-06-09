#include "GameInstance.h"

#include <filesystem>
#include <iostream>

#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Graphics/Renderer.h"
#include "Memory/Memory.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "Graphics/GraphicsUtility.h"
#include "Graphics/MeshGeneration.h"
#include "Graphics/RenderCommonTypes.h"

NAMESPACE_FRT_START
FRT_SINGLETON_DEFINE_INSTANCE(GameInstance)

using namespace graphics;
using namespace memory::literals;

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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	ImGui_ImplWin32_Init(Window->GetHandle());

	ImGui_ImplDX12_InitInfo imguiDx12InitInfo;
	imguiDx12InitInfo.Device = Renderer->GetDevice();
	imguiDx12InitInfo.CommandQueue = Renderer->GetCommandQueue();
	imguiDx12InitInfo.NumFramesInFlight = render::constants::FrameResourcesBufferCount;
	imguiDx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	imguiDx12InitInfo.DSVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	imguiDx12InitInfo.UserData = Renderer.GetRawIgnoringLifetime();
	imguiDx12InitInfo.SrvDescriptorHeap = Renderer->ShaderDescriptorHeap.GetHeap();
	imguiDx12InitInfo.SrvDescriptorAllocFn =
		[] (
		ImGui_ImplDX12_InitInfo* InitInfo,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			return ((CRenderer*)InitInfo->UserData)->ShaderDescriptorHeap.Allocate(OutCpuHandle, OutGpuHandle);
		};
	imguiDx12InitInfo.SrvDescriptorFreeFn =
		[] (ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
		{
			// TODO
		};
	ImGui_ImplDX12_Init(&imguiDx12InitInfo);
}

GameInstance::~GameInstance ()
{
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	delete Timer;
	delete Window;

	Timer = nullptr;
	Window = nullptr;
}

CTimer& GameInstance::GetTime () const
{
	return *Timer;
}

bool GameInstance::HasGraphics () const
{
	return !!Renderer;
}

memory::TRefWeak<graphics::CRenderer> GameInstance::GetGraphics () const
{
	frt_assert(Renderer);
	return Renderer.GetWeak();
}

void GameInstance::Load ()
{
	std::cout << std::filesystem::current_path() << std::endl;

	// World->SpawnEntity()->Mesh = mesh::GenerateCube(Vector3f(.3f), 1);
	World->SpawnEntity()->Mesh = mesh::GenerateGeosphere(1.f, 2u);
	World->SpawnEntity()->Mesh = mesh::GenerateSphere(.1f, 10u, 10u);
	// skullEnt->Model = Model::LoadFromFile(
	// 	R"(..\Core\Content\Models\Skull\scene.gltf)",
	// 	R"(..\Core\Content\Models\Skull\textures\defaultMat_baseColor.jpeg)");
}

void GameInstance::Tick (float DeltaSeconds)
{
	++FrameCount;

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::ShowDemoWindow();

	CalculateFrameStats();
	DisplayUserSettings();

	Renderer->Tick(DeltaSeconds);
	Camera->Tick(DeltaSeconds);
	World->Tick(DeltaSeconds);
}

void GameInstance::Draw (float DeltaSeconds)
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

	ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_NoResize);
	ImGui::Text("FPS: %.2f", fps);
	ImGui::Text("MS/frame: %.2f", msPerFrame);
	ImGui::End();
}

void GameInstance::OnWindowResize ()
{
	Renderer->Resize(UserSettings.DisplaySettings.IsFullscreen());
}

void GameInstance::OnLoseFocus ()
{}

void GameInstance::OnGainFocus ()
{}

void GameInstance::OnMinimize ()
{
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

NAMESPACE_FRT_END
