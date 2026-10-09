#include "UI/ImGuiLayer.h"

#if !defined(FRT_HEADLESS)

#include <cmath>
#include <optional>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include "EnginePaths.h"
#include "Window.h"
#include "Containers/String.h"
#include "Graphics/Render/Renderer.h"
#include "Input/InputSystem.h"
#include "Math/MathUtility.h"
#include "Platform/Platform.h"


namespace frt
{
namespace
{
constexpr float ZoomMin = 0.4f;
constexpr float ZoomMax = 3.0f;
constexpr float ZoomStepPerNotch = 1.1f;
}

struct CImGuiLayer::SImpl
{
	ImGui_ImplDX12_InitInfo Dx12InitInfo;
	ImGuiStyle UnscaledStyle;     // reference style at scale 1.0, untinted
};

void CImGuiLayer::StartUp (CWindow& InWindow, graphics::CRenderer& Renderer, const std::filesystem::path& InIniPath)
{
	frt_assert(!bStarted);
	Window = &InWindow;
	Impl = new SImpl();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IniPath = InIniPath;
	bIniLoaded = false;
	if (!IniPath.empty())
	{
		// Read and written by this layer (BeginFrame, SaveIniFile).
		io.IniFilename = nullptr;
	}

	// UI font. The size given here becomes style.FontSizeBase; the scale factors set by
	// ApplyStyle() are applied on top of it, and 1.92 re-rasterises per size, so zoomed text
	// stays sharp. Read through the UTF-16 path; a stripped content dir degrades to the
	// built-in font.
	const std::filesystem::path fontPath = paths::GetEngineContentDir() / "Fonts" / "Roboto-Medium.ttf";
	if (std::optional<std::vector<uint8>> fontBytes = platform::ReadFileBytes(fontPath))
	{
		FontData = std::move(*fontBytes);
		ImFontConfig config;
		config.FontDataOwnedByAtlas = false;
		// What AddFontFromFileTTF would name it (debug windows show it).
		FString::FromPath(fontPath.filename()).CopyTo(config.Name, static_cast<uint32>(sizeof(config.Name)));
		io.Fonts->AddFontFromMemoryTTF(FontData.data(), static_cast<int>(FontData.size()), 16.0f, &config);
	}
	else
	{
		io.Fonts->AddFontDefault();
	}

	Impl->UnscaledStyle = ImGui::GetStyle();

	ImGui_ImplWin32_Init(Window->GetHandle());

	ImGui_ImplDX12_InitInfo& info = Impl->Dx12InitInfo;
	info = ImGui_ImplDX12_InitInfo();
	info.Device = Renderer.GetDevice();
	info.CommandQueue = Renderer.GetCommandQueue();
	info.NumFramesInFlight = graphics::rhi::MaxFramesInFlight;
	info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	info.DSVFormat = DXGI_FORMAT_UNKNOWN;
	info.UserData = &Renderer;
	info.SrvDescriptorHeap = Renderer.GetBindlessHeap();
	info.SrvDescriptorAllocFn =
		[] (ImGui_ImplDX12_InitInfo* InitInfo, D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
		{
			static_cast<graphics::CRenderer*>(InitInfo->UserData)->AllocateImGuiDescriptor(OutCpuHandle, OutGpuHandle);
		};
	info.SrvDescriptorFreeFn =
		[] (ImGui_ImplDX12_InitInfo* InitInfo, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
		{
			static_cast<graphics::CRenderer*>(InitInfo->UserData)->FreeImGuiDescriptor(CpuHandle, GpuHandle);
		};
	ImGui_ImplDX12_Init(&info);
	bStarted = true;
}

void CImGuiLayer::ShutDown ()
{
	if (!bStarted)
	{
		return;
	}
	// The layout as it is now, as DestroyContext writes it when ImGui owns the file: a change
	// made in the last seconds has not been saved yet.
	if (bIniLoaded)
	{
		SaveIniFile();
	}
	// The GPU must be idle (the engine flushes first): the backend releases its textures, whose
	// descriptors go back to the renderer's bindless allocator.
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	FontData = std::vector<uint8>();
	delete Impl;
	Impl = nullptr;
	bStarted = false;
}

void CImGuiLayer::DisableIniFile ()
{
	IniPath.clear();
	ImGui::GetIO().IniFilename = nullptr;
}

void CImGuiLayer::SaveIniFile ()
{
	ImGui::GetIO().WantSaveIniSettings = false;
	if (IniPath.empty())
	{
		return;
	}
	size_t size = 0u;
	const char* text = ImGui::SaveIniSettingsToMemory(&size);
	platform::WriteFileText(IniPath, FStringView(text, size));
}

void CImGuiLayer::ApplyStyle (float DpiScale)
{
	const float wantedScale = Zoom * DpiScale;
	const bool bTintChanged = Tint[0] != TintApplied[0] || Tint[1] != TintApplied[1] || Tint[2] != TintApplied[2];
	if (wantedScale == ScaleApplied && !bTintChanged)
	{
		return;
	}

	ImGuiStyle& style = ImGui::GetStyle();
	style = Impl->UnscaledStyle;
	if (Tint[0] != 1.0f || Tint[1] != 1.0f || Tint[2] != 1.0f)
	{
		for (const ImGuiCol col : { ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg, ImGuiCol_MenuBarBg,
			ImGuiCol_TitleBg, ImGuiCol_TitleBgActive, ImGuiCol_TitleBgCollapsed, ImGuiCol_Tab, ImGuiCol_TabHovered,
			ImGuiCol_TabSelected, ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected, ImGuiCol_DockingEmptyBg })
		{
			ImVec4& c = style.Colors[col];
			c.x *= Tint[0];
			c.y *= Tint[1];
			c.z *= Tint[2];
		}
	}
	style.ScaleAllSizes(wantedScale);
	style.FontScaleMain = wantedScale;

	ScaleApplied = wantedScale;
	TintApplied[0] = Tint[0];
	TintApplied[1] = Tint[1];
	TintApplied[2] = Tint[2];
}

void CImGuiLayer::BeginFrame (const input::CInputSystem& Input)
{
	// Ctrl+Wheel zooms the whole UI. ImGui ignores the wheel while Ctrl is held (UpdateMouseWheel
	// bails unless io.FontAllowUserScaling), so no window scrolls underneath.
	const bool bCtrlDown = Input.IsKeyDown(input::KeyCode::LeftCtrl) || Input.IsKeyDown(input::KeyCode::RightCtrl);
	if (bCtrlDown)
	{
		const float wheel = Input.GetMouseWheelDelta();
		if (wheel != 0.0f)
		{
			Zoom = math::Clamp(Zoom * std::pow(ZoomStepPerNotch, wheel), ZoomMin, ZoomMax);
		}
	}

	// The monitor scale is re-queried every frame, so moving the window to a differently scaled
	// monitor needs no extra plumbing. Before NewFrame, which resolves font size and metrics.
	ApplyStyle(ImGui_ImplWin32_GetDpiScaleForHwnd(Window->GetHandle()));

	// The layout is read before the first frame, as ImGui reads its own file: until then the
	// file can still be turned off (DisableIniFile).
	if (!bIniLoaded)
	{
		if (!IniPath.empty())
		{
			if (const std::optional<FString> text = platform::ReadFileText(IniPath))
			{
				ImGui::LoadIniSettingsFromMemory(text->GetData(), text->Len());
			}
		}
		bIniLoaded = true;
	}

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// NewFrame asks for a save a few seconds after the layout last changed (io.IniSavingRate).
	if (ImGui::GetIO().WantSaveIniSettings)
	{
		SaveIniFile();
	}
}

void CImGuiLayer::Render (graphics::CRenderer& Renderer)
{
	ImGui::Render();
	ID3D12DescriptorHeap* heaps[] = { Renderer.GetBindlessHeap() };
	Renderer.GetCommandList()->SetDescriptorHeaps(_countof(heaps), heaps);
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), Renderer.GetCommandList());
}

void CImGuiLayer::EndFrameWithoutRender ()
{
	ImGui::EndFrame();
}

void CImGuiLayer::SetTint (float R, float G, float B)
{
	Tint[0] = R;
	Tint[1] = G;
	Tint[2] = B;
}
}

#endif // !FRT_HEADLESS
