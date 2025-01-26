#include "GameInstance.h"

#include <filesystem>
#include <iostream>

#include "Graphics/Renderer.h"
#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Memory/Memory.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"
#include "Graphics/GraphicsUtility.h"
#include "Graphics/RenderCommonTypes.h"

NAMESPACE_FRT_START

FRT_SINGLETON_DEFINE_INSTANCE(GameInstance)

using namespace graphics;

GameInstance::GameInstance()
	: _frameCount(0)
{
	_timer = new Timer;

	WindowParams windowParams;
	windowParams.startX = 1000;
	windowParams.startY = 1000;
	windowParams.width = 1260;
	windowParams.height = 720;
	windowParams.className = L"FrtWindowClass";
	windowParams.hInst = GetModuleHandle(nullptr);
	_window = new Window(windowParams);

	memory::DefaultAllocator::InitMasterInstance(1 * memory::GigaByte);
	_renderer = new Renderer(_window);
	DisplayOptions = graphics::GetDisplayOptions(_renderer->GetAdapter());

	World = memory::New<CWorld>();

	Camera = memory::New<CCamera>();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	ImGui_ImplWin32_Init(_window->GetHandle());

	ImGui_ImplDX12_InitInfo imguiDx12InitInfo;
	imguiDx12InitInfo.Device = _renderer->GetDevice();
	imguiDx12InitInfo.CommandQueue = _renderer->GetCommandQueue();
	imguiDx12InitInfo.NumFramesInFlight = _renderer->FrameBufferSize;
	imguiDx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	imguiDx12InitInfo.DSVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	imguiDx12InitInfo.UserData = _renderer;
	imguiDx12InitInfo.SrvDescriptorHeap = _renderer->ShaderDescriptorHeap.GetHeap();
	imguiDx12InitInfo.SrvDescriptorAllocFn =
		[](ImGui_ImplDX12_InitInfo* InitInfo, D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			return ((Renderer*)InitInfo->UserData)->ShaderDescriptorHeap.Allocate(OutCpuHandle, OutGpuHandle);
		};
	imguiDx12InitInfo.SrvDescriptorFreeFn =
		[](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
		{
			// TODO
		};
	ImGui_ImplDX12_Init(&imguiDx12InitInfo);
}

GameInstance::~GameInstance()
{
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	delete _timer;
	delete _window;
	delete _renderer;

	_timer = nullptr;
	_window = nullptr;
	_renderer = nullptr;
}

Timer& GameInstance::GetTime() const
{
	return *_timer;
}

bool GameInstance::HasGraphics() const
{
	return !!_renderer;
}

Renderer& GameInstance::GetGraphics() const
{
	frt_assert(_renderer);
	return *_renderer;
}

void GameInstance::Load()
{
	auto skullEnt = World->SpawnEntity();
	std::cout << std::filesystem::current_path() << std::endl;
	skullEnt->Model = Model::LoadFromFile(
		R"(..\Core\Content\Models\Skull\scene.gltf)",
		R"(..\Core\Content\Models\Skull\textures\defaultMat_baseColor.jpeg)");
}

void GameInstance::Tick(float DeltaSeconds)
{
	++_frameCount;

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::ShowDemoWindow();

	CalculateFrameStats();
	DisplayUserSettings();

	World->Tick(DeltaSeconds);
	Camera->Tick(DeltaSeconds);
}

void GameInstance::Draw(float DeltaSeconds)
{
	_renderer->StartFrame(*Camera);

	World->Present(DeltaSeconds, _renderer->GetCommandList());

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _renderer->GetCommandList());

	_renderer->Draw(DeltaSeconds, *Camera);
}

long long GameInstance::GetFrameCount() const
{
	return _frameCount;
}

void GameInstance::CalculateFrameStats() const
{
	static int frameCount = 0;
	static float timeElapsed = 0.f;

	++frameCount;

	static float fps = 0.f, msPerFrame = 0.f;

	if (_timer->GetTotalSeconds() - timeElapsed >= 1.f)
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

void GameInstance::DisplayUserSettings()
{
	// TODO: this func is okay for now, but should be revisited later to at least remove reallocations on each frame

	ImGui::Begin("DisplaySettings");

	const auto strToChar = [](void* UserData, int Idx) -> const char*
	{
		return static_cast<std::string*>(UserData)[Idx].c_str();
	};

	{
		const auto monitorNames = DisplayOptions.GetNames();
		const char* labelMonitor = "Monitor";
		ImGui::Combo(labelMonitor, &UserSettings.MonitorIndex, strToChar, (void*)monitorNames.data(), (int)monitorNames.size());
	}

	std::vector<uint64> resolutions = DisplayOptions.GetResolutionsEncoded(UserSettings.MonitorIndex);
	UserSettings.ResolutionIndex = math::ClampIndex(UserSettings.ResolutionIndex, resolutions);

	{
		std::vector<std::string> resolutionStrs;
		resolutionStrs.reserve(resolutions.size());
		for (const auto& res : resolutions)
		{
			uint32 width, height;
			math::DecodeTwoFromOne(res, width, height);
			resolutionStrs.emplace_back(std::format("{}:{}", width, height));
		}

		const char* labelResolution = "Resolution";
		ImGui::Combo(labelResolution, &UserSettings.ResolutionIndex, strToChar, resolutionStrs.data(), (int)resolutionStrs.size());
	}

	{
		std::vector<uint64> refreshRates = DisplayOptions.GetRefreshRatesEncoded(
			UserSettings.MonitorIndex, resolutions[UserSettings.ResolutionIndex]);
		UserSettings.RefreshRateIndex = math::ClampIndex(UserSettings.RefreshRateIndex, refreshRates);

		std::vector<std::string> rRStrs;
		rRStrs.reserve(refreshRates.size());
		for (const auto& rr : refreshRates)
		{
			uint32 numerator, denominator;
			math::DecodeTwoFromOne(rr, numerator, denominator);
			rRStrs.emplace_back(std::format("{:.2f}", (float)numerator / (float)denominator));
		}

		const char* labelRR = "RefreshRate";
		ImGui::Combo(labelRR, &UserSettings.RefreshRateIndex, strToChar, rRStrs.data(), (int)rRStrs.size());
	}

	{
		const char* modeNames[] = { "Fullscreen", "Windowed", "Borderless" };
		const char* labelFullscreen = "Fullscreen";
		ImGui::SliderInt(
			labelFullscreen,
			(int*)&UserSettings.DisplayMode,
			0, (int32)EDisplayMode::Borderless,
			modeNames[(int32)UserSettings.DisplayMode],
			ImGuiSliderFlags_AlwaysClamp);
	}

	ImGui::End();
}

NAMESPACE_FRT_END
