#include "UI/ImGuiLayer.h"

#if !defined(FRT_HEADLESS)

#include <cmath>
#include <system_error>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include "EnginePaths.h"
#include "Window.h"
#include "Graphics/Render/Renderer.h"
#include "Input/InputSystem.h"
#include "Math/MathUtility.h"


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
	if (!InIniPath.empty())
	{
		std::error_code ec;
		std::filesystem::create_directories(InIniPath.parent_path(), ec);
		IniPath = InIniPath.string();
		io.IniFilename = IniPath.c_str();
	}

	// UI font. The size given here becomes style.FontSizeBase; the scale factors set by
	// ApplyStyle() are applied on top of it, and 1.92 re-rasterises per size, so zoomed text
	// stays sharp. AddFontFromFileTTF() asserts on a missing file, hence the existence check -
	// a stripped content dir should degrade to the built-in font.
	const std::filesystem::path fontPath = paths::FindContentSubdir("Fonts") / "Roboto-Medium.ttf";
	std::error_code fontEc;
	if (std::filesystem::exists(fontPath, fontEc))
	{
		io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 16.0f);
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
	// The GPU must be idle (the engine flushes first): the backend releases its textures, whose
	// descriptors go back to the renderer's bindless allocator. DestroyContext writes the layout.
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	delete Impl;
	Impl = nullptr;
	bStarted = false;
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

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
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
