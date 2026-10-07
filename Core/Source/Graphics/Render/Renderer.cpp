#include "Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "EnginePaths.h"
#include "Exception.h"
#include "Window.h"
#include "Graphics/Render/Material.h"
#include "Graphics/Render/Passes/Denoiser.h"
#include "Graphics/Render/Passes/FrameCapture.h"
#include "Graphics/Render/Passes/PathTracer.h"
#include "Graphics/Render/Passes/PostProcess.h"
#include "Graphics/Render/Passes/RasterPreview.h"
#include "Graphics/Render/Passes/Upscaler.h"
#include "Graphics/Render/Scene/GpuScene.h"
#include "Profiler/Profiler.h"


namespace frt::graphics
{
namespace
{
// Adapters in high-performance order, requiring DXR tier 1.1 (inline ray queries). WARP is
// skipped: it reports ray tracing but is useless for it.
ComPtr<IDXGIAdapter1> PickAdapter (IDXGIFactory6* Factory)
{
	ComPtr<IDXGIAdapter1> fallback;
	for (UINT index = 0; ; ++index)
	{
		ComPtr<IDXGIAdapter1> adapter;
		if (Factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
		{
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			continue;
		}
		ComPtr<ID3D12Device> probe;
		if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe))))
		{
			continue;
		}
		if (!fallback)
		{
			fallback = adapter;
		}
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
		if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) &&
			options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1)
		{
			return adapter;
		}
	}
	return fallback;
}

rhi::SDeviceCaps QueryCaps (ID3D12Device5* Device)
{
	rhi::SDeviceCaps caps;
	for (const D3D_SHADER_MODEL model : { D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6 })
	{
		D3D12_FEATURE_DATA_SHADER_MODEL sm = { model };
		if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
		{
			caps.HighestShaderModel = sm.HighestShaderModel;
			break;
		}
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
	{
		caps.ResourceBindingTier = options.ResourceBindingTier;
		caps.bTypedUavLoadAdditionalFormats = options.TypedUAVLoadAdditionalFormats;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))))
	{
		caps.WaveLaneCountMin = options1.WaveLaneCountMin;
		caps.WaveLaneCountMax = options1.WaveLaneCountMax;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4))))
	{
		caps.bNative16BitOps = options4.Native16BitShaderOpsSupported;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
	{
		caps.RaytracingTier = options5.RaytracingTier;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9 = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9))))
	{
		caps.bInt64Atomics = options9.AtomicInt64OnTypedResourceSupported;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
	{
		caps.bEnhancedBarriers = options12.EnhancedBarriersSupported;
	}
	caps.bOpacityMicromaps = caps.RaytracingTier >= D3D12_RAYTRACING_TIER_1_2;
	caps.bShaderExecutionReordering = caps.HighestShaderModel >= D3D_SHADER_MODEL_6_9 && caps.RaytracingTier >= D3D12_RAYTRACING_TIER_1_2;
	D3D12_FEATURE_DATA_D3D12_OPTIONS22 options22 = {};
	if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22, &options22, sizeof(options22))))
	{
		caps.bSerActuallyReorders = options22.ShaderExecutionReorderingActuallyReorders;
	}
	return caps;
}

const char* ShaderModelName (D3D_SHADER_MODEL Model)
{
	switch (Model)
	{
		case D3D_SHADER_MODEL_6_9: return "6.9";
		case D3D_SHADER_MODEL_6_8: return "6.8";
		case D3D_SHADER_MODEL_6_7: return "6.7";
		case D3D_SHADER_MODEL_6_6: return "6.6";
		default: return "<6.6";
	}
}

void ReportDeviceRemoved (ID3D12Device5* Device)
{
	const HRESULT reason = Device->GetDeviceRemovedReason();
	std::fprintf(stderr, "[renderer] DEVICE REMOVED: 0x%08lx\n", reason);
	ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
	if (SUCCEEDED(Device->QueryInterface(IID_PPV_ARGS(&dred))))
	{
		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
		if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
		{
			for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
			{
				const uint32 completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
				if (completed < node->BreadcrumbCount)
				{
					std::fprintf(stderr, "  [dred] list '%ls' stopped at op %u / %u\n",
						node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?", completed, node->BreadcrumbCount);
				}
			}
		}
		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA)
		{
			std::fprintf(stderr, "  [dred] page fault at VA 0x%llx\n", static_cast<unsigned long long>(pageFault.PageFaultVA));
		}
	}
	std::fflush(stderr);
}

// Reversed-Z infinite perspective (row-vector convention): NDC z = Near / viewZ.
DirectX::XMMATRIX MakeReversedInfinitePerspective (float FovY, float Aspect, float Near)
{
	const float f = 1.0f / std::tan(0.5f * FovY);
	return DirectX::XMMATRIX(
		f / Aspect, 0.0f, 0.0f, 0.0f,
		0.0f, f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, Near, 0.0f);
}

uint64 HashBytes (uint64 Hash, const void* Data, size_t Size)
{
	const uint8* bytes = static_cast<const uint8*>(Data);
	for (size_t i = 0; i < Size; ++i)
	{
		Hash ^= bytes[i];
		Hash *= 1099511628211ull;
	}
	return Hash;
}
}


CRenderer::CRenderer (CWindow* InWindow)
	: Window(InWindow)
{
	CreateDevice();

	Rhi.Init(Device.Get(), CommandQueue.Get(), QueryCaps(Device.Get()));
	const rhi::SDeviceCaps& caps = Rhi.GetCaps();
	{
		DXGI_ADAPTER_DESC1 desc = {};
		Adapter->GetDesc1(&desc);
		std::fprintf(stdout, "[renderer] %ls | SM %s | DXR tier %d.%d | SER %s%s | bindless tier %d\n",
			desc.Description, ShaderModelName(caps.HighestShaderModel),
			caps.RaytracingTier / 10, caps.RaytracingTier % 10,
			caps.bShaderExecutionReordering ? "yes" : "no",
			caps.bShaderExecutionReordering ? (caps.bSerActuallyReorders ? " (reorders)" : " (no-op)") : "",
			static_cast<int>(caps.ResourceBindingTier));
		std::fflush(stdout);
	}
	if (caps.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
	{
		std::fprintf(stderr, "[renderer] DXR tier 1.1 is required for the path tracer; raster preview only.\n");
		RenderMode = ERenderMode::Raster;
	}

	ShaderCompiler.Init(paths::GetShaderRootDir());
	Pipelines.Init(Rhi, ShaderCompiler);
#ifdef RELEASE
	Pipelines.SetHotReloadEnabled(false);
#endif

	GpuProfiler.Init(Device.Get(), CommandQueue.Get(), 96u, rhi::MaxFramesInFlight);
	RayCounters.Init(Device.Get(), rhi::MaxFramesInFlight);
	RayCounterUav = Rhi.GetBindlessHeap().AllocatePersistent();
	Rhi.WriteBufferUav(RayCounterUav, RayCounters.GetResource(), rhi::EBufferViewKind::Raw, profiler::CRayCounters::GetBufferBytes(), 0u);

	Scene = std::make_unique<CGpuScene>();
	Scene->Initialize(Rhi, Pipelines);
	PathTracer = std::make_unique<CPathTracer>();
	PathTracer->Initialize(Rhi, Pipelines);
	Denoiser = std::make_unique<CDenoiser>();
	Denoiser->Initialize(Rhi, Pipelines);
	Upscaler = CreateUpscaler(EUpscalerKind::Temporal);
	Upscaler->Initialize(Rhi, Pipelines);
	ReferenceResampler = CreateUpscaler(EUpscalerKind::None);
	ReferenceResampler->Initialize(Rhi, Pipelines);
	PostProcess = std::make_unique<CPostProcess>();
	PostProcess->Initialize(Rhi, Pipelines);
	RasterPreview = std::make_unique<CRasterPreview>();
	RasterPreview->Initialize(Rhi, Pipelines);
	Capture = std::make_unique<CFrameCapture>();
	Capture->Initialize(Rhi);

	CreateSwapChain(false);
}

CRenderer::~CRenderer ()
{
	FlushCommandQueue();
	if (Capture) { Capture->Shutdown(); }
	if (RasterPreview) { RasterPreview->Shutdown(); }
	if (PostProcess) { PostProcess->Shutdown(Rhi); }
	if (Upscaler) { Upscaler->Shutdown(Rhi); }
	if (ReferenceResampler) { ReferenceResampler->Shutdown(Rhi); }
	if (Denoiser) { Denoiser->Shutdown(Rhi); }
	if (PathTracer) { PathTracer->Shutdown(); }
	if (Scene) { Scene->Shutdown(); }
	if (DisplayColor.IsValid()) { Rhi.Destroy(DisplayColor); }
	for (uint32 i = 0; i < SwapChainBufferCount; ++i)
	{
		BackBuffers[i].Reset();
	}
	FlushCommandQueue();
	Pipelines.Shutdown();
	GpuProfiler.Shutdown();
	RayCounters.Shutdown();
	Rhi.Shutdown();
	if (FenceEvent)
	{
		CloseHandle(FenceEvent);
	}
}

void CRenderer::CreateDevice ()
{
#if defined(DEBUG) || defined(_DEBUG)
	{
		ComPtr<ID3D12Debug1> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			char gbv[8] = {};
			if (GetEnvironmentVariableA("FRT_GBV", gbv, sizeof(gbv)) > 0 && gbv[0] == '1')
			{
				debug->SetEnableGPUBasedValidation(TRUE);
			}
		}
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
		{
			dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		}
	}
	const UINT factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#else
	const UINT factoryFlags = 0u;
#endif

	THROW_IF_FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&Factory)));
	Adapter = PickAdapter(Factory.Get());
	frt_assert(Adapter);

	HRESULT hr = D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&Device));
	if (FAILED(hr))
	{
		THROW_IF_FAILED(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&Device)));
	}
	Device->SetName(L"FRT Device");

	BOOL allowTearing = FALSE;
	if (SUCCEEDED(Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
	{
		bTearingSupported = allowTearing == TRUE;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	THROW_IF_FAILED(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CommandQueue)));
	CommandQueue->SetName(L"FRT Direct Queue");

	for (uint32 i = 0; i < rhi::MaxFramesInFlight; ++i)
	{
		THROW_IF_FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Frames[i].Allocator)));
	}
	THROW_IF_FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Frames[0].Allocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));
	CommandList->SetName(L"FRT Frame");
	CommandList->Close();

	THROW_IF_FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
	FenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
}

void CRenderer::CreateSwapChain (bool bFullscreen)
{
	for (uint32 i = 0; i < SwapChainBufferCount; ++i)
	{
		BackBuffers[i].Reset();
	}
	SwapChain.Reset();

	const Vector2f size = Window->GetWindowSize();
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = static_cast<UINT>(size.x);
	desc.Height = static_cast<UINT>(size.y);
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = SwapChainBufferCount;
	desc.Scaling = DXGI_SCALING_NONE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (bTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen = {};
	fullscreen.RefreshRate = DisplayRefreshRate;
	fullscreen.Windowed = !bFullscreen;

	ComPtr<IDXGISwapChain1> swapChain1;
	THROW_IF_FAILED(Factory->CreateSwapChainForHwnd(CommandQueue.Get(), Window->GetHandle(), &desc, &fullscreen, nullptr, &swapChain1));
	THROW_IF_FAILED(swapChain1.As(&SwapChain));
	// Alt+Enter is handled by the engine's display settings.
	Factory->MakeWindowAssociation(Window->GetHandle(), DXGI_MWA_NO_ALT_ENTER);
}

void CRenderer::Resize (bool bNewFullscreenState)
{
	if (bResizing)
	{
		return;
	}
	bResizing = true;
	FlushCommandQueue();

	bFrameBuffersReady = false;
	for (uint32 i = 0; i < SwapChainBufferCount; ++i)
	{
		BackBuffers[i].Reset();
		if (BackBufferRtv[i] != 0u)
		{
			Rhi.GetRtvHeap().Free(BackBufferRtv[i] - 1u);
			BackBufferRtv[i] = 0u;
		}
	}

	BOOL bWasFullscreenRaw = FALSE;
	if (SwapChain)
	{
		SwapChain->GetFullscreenState(&bWasFullscreenRaw, nullptr);
	}
	const bool bWasFullscreen = bWasFullscreenRaw != FALSE;
	if (SwapChain && bWasFullscreen && !bNewFullscreenState)
	{
		SwapChain->SetFullscreenState(FALSE, nullptr);
	}

	const Vector2f size = Window->GetWindowSize();
	if (size.x > 0.0f && size.y > 0.0f)
	{
		if (!SwapChain || bWasFullscreen != bNewFullscreenState)
		{
			CreateSwapChain(bNewFullscreenState);
		}
		else
		{
			THROW_IF_FAILED(SwapChain->ResizeBuffers(
				SwapChainBufferCount, static_cast<UINT>(size.x), static_cast<UINT>(size.y), DXGI_FORMAT_R8G8B8A8_UNORM,
				DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (bTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u)));
		}
		if (bWasFullscreen && bNewFullscreenState)
		{
			DXGI_MODE_DESC mode = {};
			mode.Width = static_cast<UINT>(size.x);
			mode.Height = static_cast<UINT>(size.y);
			mode.RefreshRate = DisplayRefreshRate;
			mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			SwapChain->ResizeTarget(&mode);
		}

		DXGI_SWAP_CHAIN_DESC1 desc = {};
		THROW_IF_FAILED(SwapChain->GetDesc1(&desc));
		DisplayWidth = desc.Width;
		DisplayHeight = desc.Height;
		for (uint32 i = 0; i < SwapChainBufferCount; ++i)
		{
			THROW_IF_FAILED(SwapChain->GetBuffer(i, IID_PPV_ARGS(&BackBuffers[i])));
			const uint32 rtv = Rhi.GetRtvHeap().Allocate();
			Device->CreateRenderTargetView(BackBuffers[i].Get(), nullptr, Rhi.GetRtvHeap().GetCpuHandle(rtv));
			BackBufferRtv[i] = rtv + 1u;
			BackBufferState[i] = D3D12_RESOURCE_STATE_PRESENT;
		}
		bFrameBuffersReady = true;
		bForceReset = true;
	}
	else if (SwapChain)
	{
		// Minimised: drop the swap chain until a real size comes back.
		SwapChain->SetFullscreenState(FALSE, nullptr);
		SwapChain.Reset();
	}
	bResizing = false;
}

IDXGIAdapter1* CRenderer::GetAdapter () { return Adapter.Get(); }
ID3D12Device5* CRenderer::GetDevice () { return Device.Get(); }
ID3D12CommandQueue* CRenderer::GetCommandQueue () { return CommandQueue.Get(); }
ID3D12GraphicsCommandList4* CRenderer::GetCommandList () { return CommandList.Get(); }
CMaterialLibrary& CRenderer::GetMaterialLibrary () { return MaterialLibrary; }

void CRenderer::InitializeRendering ()
{
	// Everything is created lazily by the GPU scene during the first frames.
}

void CRenderer::WaitForFenceValue (uint64 Value)
{
	if (Value == 0u || Fence->GetCompletedValue() >= Value)
	{
		return;
	}
	THROW_IF_FAILED(Fence->SetEventOnCompletion(Value, FenceEvent));
	WaitForSingleObject(FenceEvent, INFINITE);
}

void CRenderer::FlushCommandQueue ()
{
	if (!CommandQueue || !Fence)
	{
		return;
	}
	++FenceValue;
	CommandQueue->Signal(Fence.Get(), FenceValue);
	WaitForFenceValue(FenceValue);
	Rhi.ProcessDeferredReleases(Fence->GetCompletedValue());
}

void CRenderer::SetRenderMode (ERenderMode Mode)
{
	// Hybrid is not a separate pipeline any more: it maps to the path tracer.
	RenderMode = Mode == ERenderMode::Hybrid ? ERenderMode::Raytracing : Mode;
	if (RenderMode == ERenderMode::Raytracing && Rhi.GetCaps().RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
	{
		RenderMode = ERenderMode::Raster;
	}
}

bool CRenderer::IsRaytracingReady () const { return PathTracer && PathTracer->IsReady(); }
bool CRenderer::ShouldRenderRaster () const { return RenderMode == ERenderMode::Raster || !IsRaytracingReady(); }
bool CRenderer::ShouldRenderRaytracing () const { return RenderMode == ERenderMode::Raytracing && IsRaytracingReady(); }

bool CRenderer::IsReadyToRender () const
{
	return bFrameBuffersReady && SwapChain && DisplayWidth > 0u && DisplayHeight > 0u;
}

EUpscalerKind CRenderer::GetUpscalerKind () const
{
	return Upscaler ? Upscaler->GetKind() : EUpscalerKind::None;
}

void CRenderer::SetUpscalerKind (EUpscalerKind Kind)
{
	if (Upscaler && Upscaler->GetKind() == Kind)
	{
		return;
	}
	std::unique_ptr<IUpscaler> next = CreateUpscaler(Kind);
	if (!next || !next->IsAvailable())
	{
		return;
	}
	FlushCommandQueue();
	if (Upscaler)
	{
		Upscaler->Shutdown(Rhi);
	}
	Upscaler = std::move(next);
	Upscaler->Initialize(Rhi, Pipelines);
	RenderWidth = 0u; // force resize
	bForceReset = true;
}

void CRenderer::Tick ()
{
	FrameSlot = (FrameSlot + 1u) % rhi::MaxFramesInFlight;
	WaitForFenceValue(Frames[FrameSlot].FenceValue);
	const uint64 completed = Fence->GetCompletedValue();
	Rhi.ProcessDeferredReleases(completed);
	Capture->Poll(completed);
	// Everything recorded this frame is retired by the signal Draw() will issue.
	Rhi.BeginFrame(FrameSlot, FenceValue + 1u);
}

void CRenderer::StartFrame ()
{
	THROW_IF_FAILED(Frames[FrameSlot].Allocator->Reset());
	THROW_IF_FAILED(CommandList->Reset(Frames[FrameSlot].Allocator.Get(), nullptr));
	bCommandListRecording = true;

	GpuProfiler.BeginFrame();
	RayCounters.BeginFrame();
	RayCounters.ClearForFrame(CommandList.Get());

	CurrentBackBuffer = SwapChain->GetCurrentBackBufferIndex();
	rhi::CBarrierBatch barriers;
	barriers.Transition(BackBuffers[CurrentBackBuffer].Get(), BackBufferState[CurrentBackBuffer], D3D12_RESOURCE_STATE_RENDER_TARGET);
	BackBufferState[CurrentBackBuffer] = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers.Flush(CommandList.Get());
}

void CRenderer::UpdateResolution ()
{
	const bool bReference = Settings.Mode == EPathTracingMode::Reference && RenderMode == ERenderMode::Raytracing;
	const float scale = bReference ? 1.0f : std::clamp(Settings.RenderScale, 0.25f, 1.0f);
	const uint32 renderW = (std::max)(1u, static_cast<uint32>(std::lround(DisplayWidth * scale)));
	const uint32 renderH = (std::max)(1u, static_cast<uint32>(std::lround(DisplayHeight * scale)));
	if (renderW == RenderWidth && renderH == RenderHeight && DisplayColor.IsValid() &&
		DisplayColor.Width == DisplayWidth && DisplayColor.Height == DisplayHeight)
	{
		return;
	}
	RenderWidth = renderW;
	RenderHeight = renderH;

	PathTracer->Resize(RenderWidth, RenderHeight);
	RasterPreview->Resize(RenderWidth, RenderHeight);
	Denoiser->Resize(Rhi, RenderWidth, RenderHeight);
	Upscaler->Resize(Rhi, RenderWidth, RenderHeight, DisplayWidth, DisplayHeight);
	ReferenceResampler->Resize(Rhi, RenderWidth, RenderHeight, DisplayWidth, DisplayHeight);
	PostProcess->Resize(Rhi, DisplayWidth, DisplayHeight);

	if (DisplayColor.IsValid())
	{
		Rhi.Destroy(DisplayColor);
	}
	rhi::STextureDesc desc;
	desc.Width = DisplayWidth;
	desc.Height = DisplayHeight;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.bUav = true;
	desc.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	desc.Name = L"DisplayColor";
	DisplayColor = Rhi.CreateTexture(desc);
	bForceReset = true;
}

void CRenderer::BuildFrameConstants (gpu::SFrameConstants& Out, bool bSceneValid)
{
	using namespace DirectX;
	Out = {};

	const XMVECTOR position = XMLoadFloat3(&CameraView.Position);
	XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&CameraView.Forward));
	const XMVECTOR up = XMLoadFloat3(&CameraView.Up);
	const XMMATRIX view = XMMatrixLookToLH(position, forward, up);

	const float aspect = static_cast<float>(DisplayWidth) / static_cast<float>((std::max)(DisplayHeight, 1u));
	const float fov = std::clamp(Settings.FieldOfViewY, 0.1f, 3.0f);
	const float nearZ = (std::max)(Settings.NearZ, 1e-4f);
	const XMMATRIX projNoJitter = MakeReversedInfinitePerspective(fov, aspect, nearZ);

	float jitterX = 0.0f;
	float jitterY = 0.0f;
	const bool bReference = RenderMode == ERenderMode::Raytracing && Settings.Mode == EPathTracingMode::Reference;
	if (!bReference && Upscaler && Upscaler->GetKind() != EUpscalerKind::None)
	{
		Upscaler->GetJitter(FrameNumber, RenderWidth, DisplayWidth, jitterX, jitterY);
	}
	XMMATRIX proj = projNoJitter;
	// Pixel k must sample the unjittered image at k + 0.5 + Jitter (the convention of the path
	// tracer's primary rays, the denoiser and the TAA). Offsetting NDC by +2J/size moves the
	// projected image by +J pixels, so pixel k would see k + 0.5 - J: hence the minus (x) / plus
	// (y, NDC up) here. With the opposite sign every raster frame was misregistered by 2 J render
	// pixels in the TAA (up to 2 display px per frame at RenderScale 0.5: combed, stair-stepped
	// edges) and depth edges failed its reprojection test.
	proj.r[2] = XMVectorAdd(proj.r[2], XMVectorSet(-2.0f * jitterX / RenderWidth, 2.0f * jitterY / RenderHeight, 0.0f, 0.0f));

	const XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	const XMMATRIX viewProjNoJitter = XMMatrixMultiply(view, projNoJitter);

	XMStoreFloat4x4(&Out.View, view);
	XMStoreFloat4x4(&Out.InvView, XMMatrixInverse(nullptr, view));
	XMStoreFloat4x4(&Out.Proj, proj);
	XMStoreFloat4x4(&Out.InvViewProjNoJitter, XMMatrixInverse(nullptr, viewProjNoJitter));
	XMStoreFloat4x4(&Out.ViewProj, viewProj);
	XMStoreFloat4x4(&Out.InvViewProj, XMMatrixInverse(nullptr, viewProj));
	XMStoreFloat4x4(&Out.ViewProjNoJitter, viewProjNoJitter);
	if (bHasPrevFrame)
	{
		Out.PrevViewProjNoJitter = PrevViewProjNoJitter;
		Out.PrevCameraPosition = PrevCameraPosition;
		Out.PrevJitter = { PrevJitter[0], PrevJitter[1] };
	}
	else
	{
		Out.PrevViewProjNoJitter = Out.ViewProjNoJitter;
		Out.PrevCameraPosition = CameraView.Position;
		Out.PrevJitter = { jitterX, jitterY };
	}

	Out.CameraPosition = CameraView.Position;
	XMStoreFloat3(&Out.CameraForward, forward);
	Out.NearZ = nearZ;
	Out.FarZ = 1e6f;
	Out.TanHalfFovY = std::tan(0.5f * fov);
	Out.RenderSize = { static_cast<float>(RenderWidth), static_cast<float>(RenderHeight) };
	Out.InvRenderSize = { 1.0f / RenderWidth, 1.0f / RenderHeight };
	Out.DisplaySize = { static_cast<float>(DisplayWidth), static_cast<float>(DisplayHeight) };
	Out.InvDisplaySize = { 1.0f / DisplayWidth, 1.0f / DisplayHeight };
	Out.Jitter = { jitterX, jitterY };
	Out.AspectRatio = aspect;
	Out.PixelSpreadAngle = std::atan(2.0f * Out.TanHalfFovY / static_cast<float>(RenderHeight));
	Out.FrameIndex = static_cast<uint32>(FrameNumber);
	Out.AccumulationIndex = AccumulationFrames;
	Out.PreExposure = 1.0f;
	Out.Flags = 0u;
	if (bReference) { Out.Flags |= gpu::FrameFlag_ReferenceMode; }
	if (bHasPrevFrame) { Out.Flags |= gpu::FrameFlag_HistoryValid; }
	if (Settings.bCollectRayCounters)
	{
		Out.Flags |= gpu::FrameFlag_CountersEnabled;
		Out.RayCounterBuffer = RayCounterUav;
	}
	else
	{
		Out.RayCounterBuffer = FRT_INVALID_INDEX;
	}
	Out.BlueNoiseTexture = FRT_INVALID_INDEX;
	Out.InstanceBuffer = Out.GeometryBuffer = Out.MaterialBuffer = Out.LightBuffer = FRT_INVALID_INDEX;
	Out.EmissiveTriangleBuffer = Out.LightAliasTable = Out.SkyEnvironmentMap = Out.SkyLuminancePyramid = FRT_INVALID_INDEX;
	Out.PortalBuffer = Out.EnergyCompensationLut = FRT_INVALID_INDEX;
	Out.SunLightIndex = Out.SkyLightIndex = FRT_INVALID_INDEX;
	if (bSceneValid)
	{
		Scene->FillFrameConstants(Out);
	}
}

uint64 CRenderer::ComputeAccumulationHash () const
{
	uint64 hash = 1469598103934665603ull;
	hash = HashBytes(hash, &FrameConstants.View, sizeof(FrameConstants.View));
	hash = HashBytes(hash, &FrameConstants.ViewProjNoJitter, sizeof(FrameConstants.ViewProjNoJitter));
	hash = HashBytes(hash, &RenderWidth, sizeof(RenderWidth));
	hash = HashBytes(hash, &RenderHeight, sizeof(RenderHeight));
	hash = HashBytes(hash, &PathTracer->Settings, sizeof(SPathTracerSettings));
	hash = HashBytes(hash, &Settings.Mode, sizeof(Settings.Mode));
	return hash;
}

void CRenderer::PrepareCurrentPass ()
{
	if (!IsReadyToRender())
	{
		return;
	}
	UpdateResolution();

#ifndef RELEASE
	if (MaterialLibrary.ReloadModifiedMaterials())
	{
		bForceReset = true;
	}
#endif
	if (Pipelines.PollHotReload())
	{
		bForceReset = true;
	}

	CRenderContext context;
	context.Device = &Rhi;
	context.Pipelines = &Pipelines;
	context.CommandList = CommandList.Get();
	context.FrameNumber = FrameNumber;

	// Provisional constants (camera only) so scene passes have a valid b1; the final version
	// with this frame's scene table indices is uploaded right after the scene update.
	BuildFrameConstants(FrameConstants, false);
	context.FrameConstantsVa = Rhi.GetUploadRing().Push(FrameConstants).Gpu;
	context.Frame = &FrameConstants;
	context.BindCompute();

	bool bSceneChanged = false;
	{
		FRT_GPU_SCOPE(CommandList.Get(), "Scene_Update");
		bSceneChanged = Scene->Update(context);
	}

	// Progressive accumulation bookkeeping (reference mode).
	{
		BuildFrameConstants(FrameConstants, true);
		const uint64 hash = ComputeAccumulationHash();
		const bool bModeChanged = PrevMode != Settings.Mode || PrevRenderMode != RenderMode;
		if (hash != AccumulationHash || bSceneChanged || bForceReset || bModeChanged)
		{
			AccumulationHash = hash;
			AccumulationFrames = 0u;
		}
		else
		{
			++AccumulationFrames;
		}
		FrameConstants.AccumulationIndex = AccumulationFrames;
		AccumulatedSamples = (AccumulationFrames + 1u) * (std::max)(PathTracer->Settings.SamplesPerPixel, 1u);
	}
	context.FrameConstantsVa = Rhi.GetUploadRing().Push(FrameConstants).Gpu;
	context.SceneTlasVa = Scene->GetTlasAddress();
	context.BindCompute();

	const bool bResetHistory = bForceReset || !bHasPrevFrame || PrevMode != Settings.Mode || PrevRenderMode != RenderMode;
	if (RenderMode == ERenderMode::Raytracing)
	{
		PathTracer->UpdatePipelines(CommandList.Get());
	}
	if (ShouldRenderRaytracing())
	{
		RecordRaytracing(context, bResetHistory);
	}
	else
	{
		RecordRaster(context, bResetHistory);
	}

	PrevViewProjNoJitter = FrameConstants.ViewProjNoJitter;
	PrevCameraPosition = FrameConstants.CameraPosition;
	PrevJitter[0] = FrameConstants.Jitter.x;
	PrevJitter[1] = FrameConstants.Jitter.y;
	bHasPrevFrame = true;
	bForceReset = false;
	PrevMode = Settings.Mode;
	PrevRenderMode = RenderMode;
}

void CRenderer::RecordRaytracing (CRenderContext& Context, bool bResetHistory)
{
	const bool bReference = Settings.Mode == EPathTracingMode::Reference;
	SPathTracerTargets& targets = PathTracer->GetTargets();

	const bool bTargetReached = bReference && Settings.ReferenceSampleTarget > 0u &&
		AccumulationFrames > 0u && AccumulatedSamples > Settings.ReferenceSampleTarget;
	if (!bTargetReached)
	{
		PathTracer->Dispatch(Context, bReference, bReference);
	}
	else if (AccumulationFrames > 0u)
	{
		--AccumulationFrames; // hold the counter at the target
	}

	rhi::SGpuTexture* hdr = &targets.Composite;
	if (bReference)
	{
		hdr = &targets.Accumulation;
	}
	else
	{
		if (Settings.bDenoiser && Denoiser->Settings.bEnabled)
		{
			SDenoiserInputs inputs;
			inputs.Diffuse = &targets.Diffuse;
			inputs.Specular = &targets.Specular;
			inputs.NormalRoughness = &targets.NormalRoughness;
			inputs.ViewZ = &targets.ViewZ;
			inputs.MotionVectors = &targets.MotionVectors;
			inputs.bResetHistory = bResetHistory;
			Denoiser->Execute(Context, inputs);
			PathTracer->Remodulate(Context, Denoiser->GetDiffuseOutput(), Denoiser->GetSpecularOutput(), targets.Composite);
		}
		else
		{
			PathTracer->Remodulate(Context, targets.Diffuse, targets.Specular, targets.Composite);
		}
	}

	SUpscalerInputs up;
	up.Color = hdr;
	up.ViewZ = &targets.ViewZ;
	up.MotionVectors = &targets.MotionVectors;
	up.JitterX = FrameConstants.Jitter.x;
	up.JitterY = FrameConstants.Jitter.y;
	up.bResetHistory = bResetHistory;
	up.Exposure = PostProcess->GetLastExposure();
	if (bReference)
	{
		// The accumulation is already anti-aliased and converged: resample only.
		ReferenceResampler->Execute(Context, up, DisplayColor);
	}
	else
	{
		Upscaler->Execute(Context, up, DisplayColor);
	}
	LastHdrForCapture = bReference ? &targets.Accumulation : &DisplayColor;

	rhi::SGpuTexture* debug = PathTracer->Settings.DebugMode != gpu::PathDebug_None ? &targets.Debug : nullptr;
	if (debug)
	{
		LastHdrForCapture = debug; // HDR captures of a debug view hold the raw linear values
	}
	RecordPostAndPresentTarget(Context, DisplayColor, debug, bResetHistory);
}

void CRenderer::RecordRaster (CRenderContext& Context, bool bResetHistory)
{
	const float mipBias = Upscaler ? Upscaler->GetMipBias(RenderWidth, DisplayWidth) : 0.0f;
	RasterPreview->Execute(Context, *Scene, mipBias);
	Context.BindCompute();

	SUpscalerInputs up;
	up.Color = &RasterPreview->GetColor();
	up.ViewZ = &RasterPreview->GetViewZ();
	up.MotionVectors = &RasterPreview->GetMotionVectors();
	up.JitterX = FrameConstants.Jitter.x;
	up.JitterY = FrameConstants.Jitter.y;
	up.bResetHistory = bResetHistory;
	up.Exposure = PostProcess->GetLastExposure();
	Upscaler->Execute(Context, up, DisplayColor);
	LastHdrForCapture = &DisplayColor;
	RecordPostAndPresentTarget(Context, DisplayColor, nullptr, bResetHistory);
}

void CRenderer::RecordPostAndPresentTarget (CRenderContext& Context, rhi::SGpuTexture& Color, rhi::SGpuTexture* Debug, bool bResetExposure)
{
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rhi.GetRtvHeap().GetCpuHandle(BackBufferRtv[CurrentBackBuffer] - 1u);

	SPostProcessInputs post;
	post.HdrColor = &Color;
	post.OutputRtv = rtv;
	post.OutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	post.OutputWidth = DisplayWidth;
	post.OutputHeight = DisplayHeight;
	post.bResetExposure = bResetExposure && AccumulationFrames == 0u && !bHasPrevFrame;
	post.DebugTexture = Debug;
	post.bDebugIsLinear = true;
	PostProcess->Execute(Context, post);

	// Captures see exactly what the user sees minus the UI.
	if (!CaptureRequests.empty())
	{
		for (const SCaptureRequest& request : CaptureRequests)
		{
			std::filesystem::path png = request.Path;
			png.replace_extension(".png");
			Capture->Record(CommandList.Get(), BackBuffers[CurrentBackBuffer].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, png);
			if (request.bHdr && LastHdrForCapture)
			{
				std::filesystem::path pfm = request.Path;
				pfm.replace_extension(".pfm");
				Capture->Record(CommandList.Get(), LastHdrForCapture->Resource.Get(), LastHdrForCapture->State, pfm);
			}
		}
		CaptureRequests.clear();
	}

	// Leave the back buffer bound for the UI.
	CommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(DisplayWidth), static_cast<float>(DisplayHeight), 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(DisplayWidth), static_cast<LONG>(DisplayHeight) };
	CommandList->RSSetViewports(1, &viewport);
	CommandList->RSSetScissorRects(1, &scissor);
	ID3D12DescriptorHeap* heaps[] = { Rhi.GetBindlessHeap().GetHeap() };
	CommandList->SetDescriptorHeaps(1, heaps);
}

void CRenderer::RebindAfterUi ()
{
}

void CRenderer::Draw ()
{
	rhi::CBarrierBatch barriers;
	barriers.Transition(BackBuffers[CurrentBackBuffer].Get(), BackBufferState[CurrentBackBuffer], D3D12_RESOURCE_STATE_PRESENT);
	BackBufferState[CurrentBackBuffer] = D3D12_RESOURCE_STATE_PRESENT;
	barriers.Flush(CommandList.Get());

	GpuProfiler.ResolveFrame(CommandList.Get());
	RayCounters.ResolveFrame(CommandList.Get());

	THROW_IF_FAILED(CommandList->Close());
	bCommandListRecording = false;
	ID3D12CommandList* lists[] = { CommandList.Get() };
	CommandQueue->ExecuteCommandLists(1, lists);

	BOOL bFullscreen = FALSE;
	SwapChain->GetFullscreenState(&bFullscreen, nullptr);
	const bool bTear = !bVSyncEnabled && bTearingSupported && !bFullscreen;
	const HRESULT hr = SwapChain->Present(bVSyncEnabled ? 1u : 0u, bTear ? DXGI_PRESENT_ALLOW_TEARING : 0u);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
	{
		ReportDeviceRemoved(Device.Get());
		THROW_IF_FAILED(hr);
	}

	++FenceValue;
	CommandQueue->Signal(Fence.Get(), FenceValue);
	Frames[FrameSlot].FenceValue = FenceValue;
	++FrameNumber;
}

void CRenderer::RequestCapture (const std::filesystem::path& Path, bool bHdr)
{
	CaptureRequests.push_back({ Path, bHdr });
}

bool CRenderer::HasPendingCaptures () const
{
	return !CaptureRequests.empty() || (Capture && Capture->HasPending());
}

void CRenderer::AllocateImGuiDescriptor (D3D12_CPU_DESCRIPTOR_HANDLE* OutCpu, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpu)
{
	const uint32 index = Rhi.GetBindlessHeap().AllocatePersistent();
	if (OutCpu) { *OutCpu = Rhi.GetBindlessHeap().GetCpuHandle(index); }
	if (OutGpu) { *OutGpu = Rhi.GetBindlessHeap().GetGpuHandle(index); }
}

void CRenderer::FreeImGuiDescriptor (D3D12_CPU_DESCRIPTOR_HANDLE Cpu, D3D12_GPU_DESCRIPTOR_HANDLE Gpu)
{
	(void)Cpu;
	const D3D12_GPU_DESCRIPTOR_HANDLE start = Rhi.GetBindlessHeap().GetGpuHandle(0u);
	const UINT increment = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const uint32 index = static_cast<uint32>((Gpu.ptr - start.ptr) / increment);
	Rhi.DeferFreeDescriptor(index);
}

void CRenderer::DrawRendererUi ()
{
	const rhi::SDeviceCaps& caps = Rhi.GetCaps();
	ImGui::Text("SM %s  DXR %d.%d  SER %s", ShaderModelName(caps.HighestShaderModel),
		caps.RaytracingTier / 10, caps.RaytracingTier % 10,
		PathTracer->IsSerActive() ? (caps.bSerActuallyReorders ? "active" : "active (no-op on this GPU)") : "off");
	ImGui::Text("Render %ux%u -> display %ux%u", RenderWidth, RenderHeight, DisplayWidth, DisplayHeight);

	int mode = static_cast<int>(RenderMode == ERenderMode::Raster ? 0 : 1);
	const char* renderModes[] = { "Raster preview", "Path tracing" };
	if (ImGui::Combo("Renderer", &mode, renderModes, 2))
	{
		SetRenderMode(mode == 0 ? ERenderMode::Raster : ERenderMode::Raytracing);
	}

	int ptMode = static_cast<int>(Settings.Mode);
	const char* ptModes[] = { "Real time (denoised)", "Reference (progressive)" };
	if (ImGui::Combo("Path tracing mode", &ptMode, ptModes, 2))
	{
		Settings.Mode = static_cast<EPathTracingMode>(ptMode);
	}

	if (Settings.Mode == EPathTracingMode::Reference)
	{
		ImGui::Text("Accumulated: %u spp", AccumulatedSamples);
		int target = static_cast<int>(Settings.ReferenceSampleTarget);
		if (ImGui::InputInt("Stop at spp (0 = never)", &target))
		{
			Settings.ReferenceSampleTarget = static_cast<uint32>((std::max)(target, 0));
		}
	}
	else
	{
		ImGui::SliderFloat("Render scale", &Settings.RenderScale, 0.33f, 1.0f, "%.2f");
		int kind = static_cast<int>(GetUpscalerKind());
		const char* kinds[] = { "Off", "TAAU (in-house)", "DLSS (n/a)", "FSR (n/a)", "XeSS (n/a)" };
		if (ImGui::Combo("Upscaler / AA", &kind, kinds, 5))
		{
			SetUpscalerKind(static_cast<EUpscalerKind>(kind));
		}
		ImGui::Checkbox("Denoiser", &Settings.bDenoiser);
	}
	float fovDeg = Settings.FieldOfViewY * 57.29578f;
	if (ImGui::SliderFloat("Vertical FOV", &fovDeg, 20.0f, 120.0f, "%.1f deg"))
	{
		Settings.FieldOfViewY = fovDeg / 57.29578f;
	}
	ImGui::Checkbox("Collect ray counters", &Settings.bCollectRayCounters);
	if (ImGui::Button("Reset accumulation / history"))
	{
		ResetAccumulation();
	}
	ImGui::SameLine();
	if (ImGui::Button("Capture"))
	{
		RequestCapture(paths::GetCaptureDir() / ("capture_" + std::to_string(FrameNumber)), true);
	}

	if (ImGui::CollapsingHeader("Path tracer", ImGuiTreeNodeFlags_DefaultOpen))
	{
		PathTracer->DrawSettingsUi();
	}
	if (ImGui::CollapsingHeader("Denoiser"))
	{
		Denoiser->DrawSettingsUi();
	}
	if (ImGui::CollapsingHeader("Upscaler / TAA"))
	{
		Upscaler->DrawSettingsUi();
	}
	if (ImGui::CollapsingHeader("Post processing", ImGuiTreeNodeFlags_DefaultOpen))
	{
		PostProcess->DrawSettingsUi();
	}
	if (ImGui::CollapsingHeader("Scene"))
	{
		const SSceneStats& s = Scene->GetStats();
		ImGui::Text("Instances %u  models %u  triangles %llu", s.Instances, s.Models, static_cast<unsigned long long>(s.Triangles));
		ImGui::Text("Materials %u  lights %u  emissive triangles %u", s.Materials, s.AnalyticLights, s.EmissiveTriangles);
		ImGui::Text("BLAS %.1f MB (%u compacted)", s.BlasBytes / (1024.0 * 1024.0), s.BlasCompacted);
		ImGui::Text("Textures %u resident (%.1f MB), %u streaming", s.TexturesResident, s.TextureBytes / (1024.0 * 1024.0), s.TexturesPending);
		ImGui::Text("Bindless descriptors %u / %u", Rhi.GetBindlessHeap().GetPersistentInUse(), Rhi.GetBindlessHeap().GetPersistentCapacity());
	}
	const std::vector<std::string>& errors = Pipelines.GetErrors();
	if (!errors.empty() && ImGui::CollapsingHeader("Shader errors", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (const std::string& e : errors)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", e.c_str());
		}
	}
}
}
