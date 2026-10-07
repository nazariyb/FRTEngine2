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
#include "Graphics/Render/Passes/PathTracerPrograms.h"
#include "Graphics/Render/Passes/PostProcess.h"
#include "Graphics/Render/Passes/RasterPreview.h"
#include "Graphics/Render/Passes/Upscaler.h"
#include "Graphics/Render/RenderView.h"
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
	}

	ShaderCompiler.Init(paths::GetShaderRootDir());
	Pipelines.Init(Rhi, ShaderCompiler);
#ifdef RELEASE
	Pipelines.SetHotReloadEnabled(false);
#endif

	// Per view ~19 scopes (a real-time path-traced view), plus a few scene ones.
	GpuProfiler.Init(Device.Get(), CommandQueue.Get(), 256u, rhi::MaxFramesInFlight);
	RayCounters.Init(Device.Get(), rhi::MaxFramesInFlight);
	RayCounterUav = Rhi.GetBindlessHeap().AllocatePersistent();
	Rhi.WriteBufferUav(RayCounterUav, RayCounters.GetResource(), rhi::EBufferViewKind::Raw, profiler::CRayCounters::GetBufferBytes(), 0u);

	Scene = std::make_unique<CGpuScene>();
	Scene->Initialize(Rhi, Pipelines);
	PathTracerPrograms = std::make_unique<CPathTracerPrograms>();
	PathTracerPrograms->Initialize(Rhi, Pipelines);
	ReferenceResampler = CreateUpscaler(EUpscalerKind::None);
	ReferenceResampler->Initialize(Rhi, Pipelines);
	Capture = std::make_unique<CFrameCapture>();
	Capture->Initialize(Rhi);

	CreateSwapChain(false);
}

CRenderer::~CRenderer ()
{
	FlushCommandQueue();
	for (std::unique_ptr<CRenderView>& view : Views)
	{
		ShutdownView(*view);
	}
	Views.clear();
	if (Capture) { Capture->Shutdown(); }
	if (ReferenceResampler) { ReferenceResampler->Shutdown(Rhi); }
	if (PathTracerPrograms) { PathTracerPrograms->Shutdown(); }
	if (Scene) { Scene->Shutdown(); }
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
		BackBufferWidth = desc.Width;
		BackBufferHeight = desc.Height;
		for (uint32 i = 0; i < SwapChainBufferCount; ++i)
		{
			THROW_IF_FAILED(SwapChain->GetBuffer(i, IID_PPV_ARGS(&BackBuffers[i])));
			const uint32 rtv = Rhi.GetRtvHeap().Allocate();
			Device->CreateRenderTargetView(BackBuffers[i].Get(), nullptr, Rhi.GetRtvHeap().GetCpuHandle(rtv));
			BackBufferRtv[i] = rtv + 1u;
			BackBufferState[i] = D3D12_RESOURCE_STATE_PRESENT;
		}
		bFrameBuffersReady = true;
		// Only views drawing into the swap chain changed size; texture views keep their history.
		for (std::unique_ptr<CRenderView>& view : Views)
		{
			if (view->GetTarget() == EViewTarget::BackBuffer)
			{
				view->bForceReset = true;
			}
		}
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
	// A flush while the frame's command list is open would retire - and free - resources that
	// list still references: the list is not part of the signal below.
	frt_assert(!bCommandListRecording);
	++FenceValue;
	CommandQueue->Signal(Fence.Get(), FenceValue);
	WaitForFenceValue(FenceValue);
	Rhi.ProcessDeferredReleases(Fence->GetCompletedValue());
	// Releases deferred later this frame are retired by the frame's own signal (++FenceValue in
	// EndFrame), not by the value BeginFrame chose, which the signal above just consumed.
	Rhi.SetSubmissionFenceValue(FenceValue + 1u);
}

bool CRenderer::CanTrace () const
{
	return Rhi.GetCaps().RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
}

bool CRenderer::IsRaytracingReady () const { return CanTrace() && PathTracerPrograms && PathTracerPrograms->IsReady(); }

bool CRenderer::IsTracing (const CRenderView& View) const
{
	return View.GetRenderMode() == ERenderMode::Raytracing && IsRaytracingReady();
}

bool CRenderer::IsReadyToRender () const
{
	return bFrameBuffersReady && SwapChain && BackBufferWidth > 0u && BackBufferHeight > 0u;
}

void CRenderer::SetUpscalerKind (EUpscalerKind Kind)
{
	const std::unique_ptr<IUpscaler> probe = CreateUpscaler(Kind);
	if (probe && probe->IsAvailable())
	{
		Profile.Upscaler = Kind;
	}
}

void CRenderer::BeginFrame ()
{
	FrameSlot = (FrameSlot + 1u) % rhi::MaxFramesInFlight;
	WaitForFenceValue(Frames[FrameSlot].FenceValue);
	const uint64 completed = Fence->GetCompletedValue();
	Rhi.ProcessDeferredReleases(completed);
	Capture->Poll(completed);
	// Everything recorded this frame is retired by the signal EndFrame() will issue.
	Rhi.BeginFrame(FrameSlot, FenceValue + 1u);
}

void CRenderer::BeginRecording ()
{
	THROW_IF_FAILED(Frames[FrameSlot].Allocator->Reset());
	THROW_IF_FAILED(CommandList->Reset(Frames[FrameSlot].Allocator.Get(), nullptr));
	bCommandListRecording = true;
	bBackBufferDrawn = false;
	bFrameSceneChanged = false;

	GpuProfiler.BeginFrame();
	RayCounters.BeginFrame();
	RayCounters.ClearForFrame(CommandList.Get());

	CurrentBackBuffer = SwapChain->GetCurrentBackBufferIndex();
	rhi::CBarrierBatch barriers;
	barriers.Transition(BackBuffers[CurrentBackBuffer].Get(), BackBufferState[CurrentBackBuffer], D3D12_RESOURCE_STATE_RENDER_TARGET);
	BackBufferState[CurrentBackBuffer] = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers.Flush(CommandList.Get());
}

CRenderView::CRenderView (const SRenderViewDesc& InDesc)
	: Desc(InDesc)
{}

CRenderView::~CRenderView () = default;

void CRenderView::RequestCapture (const std::filesystem::path& Path, bool bHdr)
{
	CaptureRequests.push_back({ Path, bHdr });
}

CRenderView* CRenderer::CreateView (const SRenderViewDesc& Desc)
{
	// Pass instances are cheap to create: CPipelineLibrary hands every instance after the first
	// the already compiled programs, and the path tracer's state objects are shared.
	std::unique_ptr<CRenderView> view(new CRenderView(Desc));
	view->PathTracer = std::make_unique<CPathTracer>();
	view->PathTracer->Initialize(Rhi, Pipelines, *PathTracerPrograms);
	view->Denoiser = std::make_unique<CDenoiser>();
	view->Denoiser->Initialize(Rhi, Pipelines);
	view->Upscaler = CreateUpscaler(Profile.Upscaler);
	if (!view->Upscaler)
	{
		view->Upscaler = CreateUpscaler(EUpscalerKind::Temporal);
	}
	view->Upscaler->Initialize(Rhi, Pipelines);
	view->PostProcess = std::make_unique<CPostProcess>();
	view->PostProcess->Initialize(Rhi, Pipelines);
	view->RasterPreview = std::make_unique<CRasterPreview>();
	view->RasterPreview->Initialize(Rhi, Pipelines);
	view->SeenHistoryEpoch = HistoryEpoch;
	Views.push_back(std::move(view));
	return Views.back().get();
}

void CRenderer::ShutdownView (CRenderView& View)
{
	// Every pass releases through the RHI's deferred queue: nothing in flight is freed early.
	View.RasterPreview->Shutdown();
	View.PostProcess->Shutdown(Rhi);
	View.Upscaler->Shutdown(Rhi);
	View.Denoiser->Shutdown(Rhi);
	View.PathTracer->Shutdown();
	if (View.DisplayColor.IsValid()) { Rhi.Destroy(View.DisplayColor); }
	if (View.Output.IsValid()) { Rhi.Destroy(View.Output); }
	View.TextureId = 0u;
}

void CRenderer::DestroyView (CRenderView* View)
{
	if (!View)
	{
		return;
	}
	frt_assert(!bCommandListRecording);
	const auto it = std::find_if(Views.begin(), Views.end(), [View](const std::unique_ptr<CRenderView>& v) { return v.get() == View; });
	frt_assert(it != Views.end());
	if (it != Views.end())
	{
		ShutdownView(**it);
		Views.erase(it);
	}
}

void CRenderer::SetViewSize (CRenderView& View, uint32 Width, uint32 Height)
{
	frt_assert(View.GetTarget() == EViewTarget::Texture);
	Width = (std::max)(Width, 1u);
	Height = (std::max)(Height, 1u);
	if (View.Output.IsValid() && View.Output.Width == Width && View.Output.Height == Height)
	{
		return;
	}
	// The old texture may still be sampled by frames in flight - and by this frame's UI, if the
	// id was handed out before the resize. Destroy is deferred to this frame's fence (BeginFrame
	// ran before the UI), so neither the resource nor its descriptor slot is reused too early.
	if (View.Output.IsValid())
	{
		Rhi.Destroy(View.Output);
	}
	rhi::STextureDesc desc;
	desc.Width = Width;
	desc.Height = Height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // display-encoded, sampled as is by the UI
	desc.bSrv = true;
	desc.bRtv = true;
	desc.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	desc.Name = L"ViewOutput";
	View.Output = Rhi.CreateTexture(desc);
	View.TextureId = Rhi.GetBindlessHeap().GetGpuHandle(View.Output.Srv).ptr;
}

void CRenderer::UpdateResolution (CRenderView& View)
{
	uint32 displayW = BackBufferWidth;
	uint32 displayH = BackBufferHeight;
	if (View.GetTarget() == EViewTarget::Texture)
	{
		displayW = View.Output.Width;
		displayH = View.Output.Height;
	}
	const bool bReference = Profile.Frame.Mode == EPathTracingMode::Reference && View.GetRenderMode() == ERenderMode::Raytracing;
	const float scale = bReference ? 1.0f : std::clamp(Profile.Frame.RenderScale, 0.25f, 1.0f);
	const uint32 renderW = (std::max)(1u, static_cast<uint32>(std::lround(displayW * scale)));
	const uint32 renderH = (std::max)(1u, static_cast<uint32>(std::lround(displayH * scale)));
	if (renderW == View.RenderWidth && renderH == View.RenderHeight && View.DisplayColor.IsValid() &&
		View.DisplayColor.Width == displayW && View.DisplayColor.Height == displayH)
	{
		return;
	}
	View.RenderWidth = renderW;
	View.RenderHeight = renderH;
	View.DisplayWidth = displayW;
	View.DisplayHeight = displayH;

	View.PathTracer->Resize(View.RenderWidth, View.RenderHeight);
	View.RasterPreview->Resize(View.RenderWidth, View.RenderHeight);
	View.Denoiser->Resize(Rhi, View.RenderWidth, View.RenderHeight);
	View.Upscaler->Resize(Rhi, View.RenderWidth, View.RenderHeight, View.DisplayWidth, View.DisplayHeight);
	ReferenceResampler->Resize(Rhi, View.RenderWidth, View.RenderHeight, View.DisplayWidth, View.DisplayHeight);
	View.PostProcess->Resize(Rhi, View.DisplayWidth, View.DisplayHeight);

	if (View.DisplayColor.IsValid())
	{
		Rhi.Destroy(View.DisplayColor);
	}
	rhi::STextureDesc desc;
	desc.Width = View.DisplayWidth;
	desc.Height = View.DisplayHeight;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.bUav = true;
	desc.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	desc.Name = L"DisplayColor";
	View.DisplayColor = Rhi.CreateTexture(desc);
	View.bForceReset = true;
}

void CRenderer::BuildFrameConstants (CRenderView& View, const SCameraView& CameraView, gpu::SFrameConstants& Out, bool bSceneValid) const
{
	using namespace DirectX;
	Out = {};

	const XMVECTOR position = XMLoadFloat3(&CameraView.Position);
	XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&CameraView.Forward));
	const XMVECTOR up = XMLoadFloat3(&CameraView.Up);
	const XMMATRIX view = XMMatrixLookToLH(position, forward, up);

	const uint32 DisplayWidth = View.DisplayWidth;
	const uint32 DisplayHeight = View.DisplayHeight;
	const uint32 RenderWidth = View.RenderWidth;
	const uint32 RenderHeight = View.RenderHeight;
	const float aspect = static_cast<float>(DisplayWidth) / static_cast<float>((std::max)(DisplayHeight, 1u));
	const float fov = std::clamp(CameraView.FieldOfViewY, 0.1f, 3.0f);
	const float nearZ = (std::max)(CameraView.NearZ, 1e-4f);
	const XMMATRIX projNoJitter = MakeReversedInfinitePerspective(fov, aspect, nearZ);

	float jitterX = 0.0f;
	float jitterY = 0.0f;
	const bool bReference = View.GetRenderMode() == ERenderMode::Raytracing && Profile.Frame.Mode == EPathTracingMode::Reference;
	if (!bReference && View.Upscaler && View.Upscaler->GetKind() != EUpscalerKind::None)
	{
		View.Upscaler->GetJitter(FrameNumber, RenderWidth, DisplayWidth, jitterX, jitterY);
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
	if (View.bHasPrevFrame)
	{
		Out.PrevViewProjNoJitter = View.PrevViewProjNoJitter;
		Out.PrevCameraPosition = View.PrevCameraPosition;
		Out.PrevJitter = { View.PrevJitter[0], View.PrevJitter[1] };
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
	Out.AccumulationIndex = View.AccumulationFrames;
	Out.PreExposure = 1.0f;
	Out.Flags = 0u;
	if (bReference) { Out.Flags |= gpu::FrameFlag_ReferenceMode; }
	if (View.bHasPrevFrame) { Out.Flags |= gpu::FrameFlag_HistoryValid; }
	if (Profile.Frame.bCollectRayCounters)
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

uint64 CRenderer::ComputeAccumulationHash (const CRenderView& View) const
{
	uint64 hash = 1469598103934665603ull;
	hash = HashBytes(hash, &View.FrameConstants.View, sizeof(View.FrameConstants.View));
	hash = HashBytes(hash, &View.FrameConstants.ViewProjNoJitter, sizeof(View.FrameConstants.ViewProjNoJitter));
	hash = HashBytes(hash, &View.RenderWidth, sizeof(View.RenderWidth));
	hash = HashBytes(hash, &View.RenderHeight, sizeof(View.RenderHeight));
	hash = HashBytes(hash, &View.PathTracer->Settings, sizeof(SPathTracerSettings));
	hash = HashBytes(hash, &Profile.Frame.Mode, sizeof(Profile.Frame.Mode));
	return hash;
}

void CRenderer::ApplyProfile (CRenderView& View)
{
	if (View.Upscaler->GetKind() != Profile.Upscaler)
	{
		std::unique_ptr<IUpscaler> next = CreateUpscaler(Profile.Upscaler);
		if (next && next->IsAvailable())
		{
			// Shutdown only defers releases, so this needs no GPU flush.
			View.Upscaler->Shutdown(Rhi);
			View.Upscaler = std::move(next);
			View.Upscaler->Initialize(Rhi, Pipelines);
			View.RenderWidth = 0u; // force a resize
			View.bForceReset = true;
		}
	}
	View.PathTracer->Settings = Profile.PathTracer;
	View.PathTracer->RadianceCache.Settings = Profile.RadianceCache;
	View.Denoiser->Settings = Profile.Denoiser;
	View.Upscaler->ApplyProfile(Profile);
	View.PostProcess->Settings = Profile.Post;
}

void CRenderer::PrepareScene ()
{
	frt_assert(bCommandListRecording);
#ifndef RELEASE
	if (MaterialLibrary.ReloadModifiedMaterials())
	{
		ResetAccumulation();
	}
#endif
	if (Pipelines.PollHotReload())
	{
		ResetAccumulation();
	}

	// Scene passes read no camera data: neutral frame constants only give b1 a valid buffer.
	gpu::SFrameConstants sceneConstants = {};
	sceneConstants.RayCounterBuffer = sceneConstants.BlueNoiseTexture = FRT_INVALID_INDEX;
	CRenderContext context;
	context.Device = &Rhi;
	context.Pipelines = &Pipelines;
	context.CommandList = CommandList.Get();
	context.FrameNumber = FrameNumber;
	context.FrameConstantsVa = Rhi.GetUploadRing().Push(sceneConstants).Gpu;
	context.Frame = &sceneConstants;
	context.BindCompute();
	{
		FRT_GPU_SCOPE(CommandList.Get(), "Scene_Update");
		bFrameSceneChanged = Scene->Update(context);
	}

	// Before any view traces: builds the state objects on first use and after a hot reload.
	const bool bAnyTracing = std::any_of(Views.begin(), Views.end(),
		[] (const std::unique_ptr<CRenderView>& v) { return v->GetRenderMode() == ERenderMode::Raytracing; });
	if (CanTrace() && bAnyTracing)
	{
		PathTracerPrograms->Update(CommandList.Get());
	}
}

void CRenderer::RenderView (CRenderView& View, const SCameraView& Camera)
{
	frt_assert(bCommandListRecording);
	if (View.GetTarget() == EViewTarget::Texture && !View.Output.IsValid())
	{
		return; // no size yet (SetViewSize)
	}
	ApplyProfile(View);
	UpdateResolution(View);

	// A view skipped for a frame, or one created since the last reset, starts over: its
	// histories describe a camera and a scene that are no longer current.
	if (View.LastRenderedFrame != ~0ull && View.LastRenderedFrame + 1u != FrameNumber)
	{
		View.bHasPrevFrame = false;
	}
	if (View.SeenHistoryEpoch != HistoryEpoch)
	{
		View.bForceReset = true;
		View.SeenHistoryEpoch = HistoryEpoch;
	}

	GpuProfiler.SetView(View.GetName());

	CRenderContext context;
	context.Device = &Rhi;
	context.Pipelines = &Pipelines;
	context.CommandList = CommandList.Get();
	context.FrameNumber = FrameNumber;

	// Progressive accumulation bookkeeping (reference mode).
	{
		BuildFrameConstants(View, Camera, View.FrameConstants, true);
		const uint64 hash = ComputeAccumulationHash(View);
		const bool bModeChanged = View.PrevMode != Profile.Frame.Mode || View.PrevRenderMode != View.GetRenderMode();
		if (hash != View.AccumulationHash || bFrameSceneChanged || View.bForceReset || bModeChanged)
		{
			View.AccumulationHash = hash;
			View.AccumulationFrames = 0u;
		}
		else
		{
			++View.AccumulationFrames;
		}
		View.FrameConstants.AccumulationIndex = View.AccumulationFrames;
		View.AccumulatedSamples = (View.AccumulationFrames + 1u) * (std::max)(View.PathTracer->Settings.SamplesPerPixel, 1u);
	}
	context.FrameConstantsVa = Rhi.GetUploadRing().Push(View.FrameConstants).Gpu;
	context.Frame = &View.FrameConstants;
	context.SceneTlasVa = Scene->GetTlasAddress();
	context.BindCompute();

	const bool bResetHistory = View.bForceReset || !View.bHasPrevFrame || View.PrevMode != Profile.Frame.Mode || View.PrevRenderMode != View.GetRenderMode();
	if (IsTracing(View))
	{
		RecordRaytracing(View, context, bResetHistory);
	}
	else
	{
		RecordRaster(View, context, bResetHistory);
	}

	View.PrevViewProjNoJitter = View.FrameConstants.ViewProjNoJitter;
	View.PrevCameraPosition = View.FrameConstants.CameraPosition;
	View.PrevJitter[0] = View.FrameConstants.Jitter.x;
	View.PrevJitter[1] = View.FrameConstants.Jitter.y;
	View.bHasPrevFrame = true;
	View.bForceReset = false;
	View.PrevMode = Profile.Frame.Mode;
	View.PrevRenderMode = View.GetRenderMode();
	View.LastRenderedFrame = FrameNumber;

	GpuProfiler.SetView(nullptr);
}

void CRenderer::RecordRaytracing (CRenderView& View, CRenderContext& Context, bool bResetHistory)
{
	const bool bReference = Profile.Frame.Mode == EPathTracingMode::Reference;
	SPathTracerTargets& targets = View.PathTracer->GetTargets();

	const bool bTargetReached = bReference && Profile.Frame.ReferenceSampleTarget > 0u &&
		View.AccumulationFrames > 0u && View.AccumulatedSamples > Profile.Frame.ReferenceSampleTarget;
	if (!bTargetReached)
	{
		View.PathTracer->Dispatch(Context, bReference, bReference);
	}
	else if (View.AccumulationFrames > 0u)
	{
		--View.AccumulationFrames; // hold the counter at the target
	}

	rhi::SGpuTexture* hdr = &targets.Composite;
	if (bReference)
	{
		hdr = &targets.Accumulation;
	}
	else
	{
		if (Profile.Frame.bDenoiser && View.Denoiser->Settings.bEnabled)
		{
			SDenoiserInputs inputs;
			inputs.Diffuse = &targets.Diffuse;
			inputs.Specular = &targets.Specular;
			inputs.NormalRoughness = &targets.NormalRoughness;
			inputs.ViewZ = &targets.ViewZ;
			inputs.MotionVectors = &targets.MotionVectors;
			inputs.bResetHistory = bResetHistory;
			View.Denoiser->Execute(Context, inputs);
			View.PathTracer->Remodulate(Context, View.Denoiser->GetDiffuseOutput(), View.Denoiser->GetSpecularOutput(), targets.Composite);
		}
		else
		{
			View.PathTracer->Remodulate(Context, targets.Diffuse, targets.Specular, targets.Composite);
		}
	}

	SUpscalerInputs up;
	up.Color = hdr;
	up.ViewZ = &targets.ViewZ;
	up.MotionVectors = &targets.MotionVectors;
	up.JitterX = View.FrameConstants.Jitter.x;
	up.JitterY = View.FrameConstants.Jitter.y;
	up.bResetHistory = bResetHistory;
	up.Exposure = View.PostProcess->GetLastExposure();
	if (bReference)
	{
		// The accumulation is already anti-aliased and converged: resample only.
		ReferenceResampler->Execute(Context, up, View.DisplayColor);
	}
	else
	{
		View.Upscaler->Execute(Context, up, View.DisplayColor);
	}
	View.LastHdrForCapture = bReference ? &targets.Accumulation : &View.DisplayColor;

	rhi::SGpuTexture* debug = View.PathTracer->Settings.DebugMode != gpu::PathDebug_None ? &targets.Debug : nullptr;
	if (debug)
	{
		View.LastHdrForCapture = debug; // HDR captures of a debug view hold the raw linear values
	}
	RecordPost(View, Context, debug, bResetHistory);
}

void CRenderer::RecordRaster (CRenderView& View, CRenderContext& Context, bool bResetHistory)
{
	const float mipBias = View.Upscaler ? View.Upscaler->GetMipBias(View.RenderWidth, View.DisplayWidth) : 0.0f;
	View.RasterPreview->Execute(Context, *Scene, mipBias);
	Context.BindCompute();

	SUpscalerInputs up;
	up.Color = &View.RasterPreview->GetColor();
	up.ViewZ = &View.RasterPreview->GetViewZ();
	up.MotionVectors = &View.RasterPreview->GetMotionVectors();
	up.JitterX = View.FrameConstants.Jitter.x;
	up.JitterY = View.FrameConstants.Jitter.y;
	up.bResetHistory = bResetHistory;
	up.Exposure = View.PostProcess->GetLastExposure();
	View.Upscaler->Execute(Context, up, View.DisplayColor);
	View.LastHdrForCapture = &View.DisplayColor;
	RecordPost(View, Context, nullptr, bResetHistory);
}

void CRenderer::RecordPost (CRenderView& View, CRenderContext& Context, rhi::SGpuTexture* Debug, bool bResetExposure)
{
	const bool bBackBuffer = View.GetTarget() == EViewTarget::BackBuffer;
	ID3D12Resource* target = bBackBuffer ? BackBuffers[CurrentBackBuffer].Get() : View.Output.Resource.Get();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = bBackBuffer
		? Rhi.GetRtvHeap().GetCpuHandle(BackBufferRtv[CurrentBackBuffer] - 1u)
		: Rhi.GetRtvHeap().GetCpuHandle(View.Output.Rtv);
	if (!bBackBuffer)
	{
		// The back buffer is already a render target (BeginRecording); the view's texture is
		// shader-readable between frames, for the UI.
		Context.Barriers.Transition(View.Output, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Context.FlushBarriers();
	}

	SPostProcessInputs post;
	post.HdrColor = &View.DisplayColor;
	post.OutputRtv = rtv;
	post.OutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	post.OutputWidth = View.DisplayWidth;
	post.OutputHeight = View.DisplayHeight;
	post.bResetExposure = bResetExposure && View.AccumulationFrames == 0u && !View.bHasPrevFrame;
	post.DebugTexture = Debug;
	post.bDebugIsLinear = true;
	View.PostProcess->Execute(Context, post);

	// Captures see exactly what the view shows, without the UI.
	if (!View.CaptureRequests.empty())
	{
		for (const CRenderView::SCaptureRequest& request : View.CaptureRequests)
		{
			std::filesystem::path png = request.Path;
			png.replace_extension(".png");
			Capture->Record(CommandList.Get(), target, D3D12_RESOURCE_STATE_RENDER_TARGET, png);
			if (request.bHdr && View.LastHdrForCapture)
			{
				std::filesystem::path pfm = request.Path;
				pfm.replace_extension(".pfm");
				Capture->Record(CommandList.Get(), View.LastHdrForCapture->Resource.Get(), View.LastHdrForCapture->State, pfm);
			}
		}
		View.CaptureRequests.clear();
	}

	if (bBackBuffer)
	{
		bBackBufferDrawn = true;
	}
	else
	{
		Context.Barriers.Transition(View.Output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Context.FlushBarriers();
	}
}

void CRenderer::BeginUiPass ()
{
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rhi.GetRtvHeap().GetCpuHandle(BackBufferRtv[CurrentBackBuffer] - 1u);
	if (!bBackBufferDrawn)
	{
		// Nothing was rendered behind the UI (editor: every view is a texture).
		const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		CommandList->ClearRenderTargetView(rtv, clear, 0u, nullptr);
	}
	CommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(BackBufferWidth), static_cast<float>(BackBufferHeight), 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(BackBufferWidth), static_cast<LONG>(BackBufferHeight) };
	CommandList->RSSetViewports(1, &viewport);
	CommandList->RSSetScissorRects(1, &scissor);
	ID3D12DescriptorHeap* heaps[] = { Rhi.GetBindlessHeap().GetHeap() };
	CommandList->SetDescriptorHeaps(1, heaps);
}

void CRenderer::EndFrame ()
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


bool CRenderer::HasPendingCaptures () const
{
	for (const std::unique_ptr<CRenderView>& view : Views)
	{
		if (!view->CaptureRequests.empty())
		{
			return true;
		}
	}
	return Capture && Capture->HasPending();
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

void CRenderer::DrawRendererUi (CRenderView& Inspected)
{
	const rhi::SDeviceCaps& caps = Rhi.GetCaps();
	ImGui::Text("SM %s  DXR %d.%d  SER %s", ShaderModelName(caps.HighestShaderModel),
		caps.RaytracingTier / 10, caps.RaytracingTier % 10,
		Inspected.PathTracer->IsSerActive() ? (caps.bSerActuallyReorders ? "active" : "active (no-op on this GPU)") : "off");
	ImGui::Text("%s: render %ux%u -> display %ux%u", Inspected.GetName(), Inspected.RenderWidth, Inspected.RenderHeight, Inspected.DisplayWidth, Inspected.DisplayHeight);

	int mode = static_cast<int>(Inspected.GetRenderMode() == ERenderMode::Raster ? 0 : 1);
	const char* renderModes[] = { "Raster preview", "Path tracing" };
	if (ImGui::Combo("Renderer", &mode, renderModes, 2))
	{
		Inspected.SetRenderMode(mode == 0 ? ERenderMode::Raster : ERenderMode::Raytracing);
	}

	int ptMode = static_cast<int>(Profile.Frame.Mode);
	const char* ptModes[] = { "Real time (denoised)", "Reference (progressive)" };
	if (ImGui::Combo("Path tracing mode", &ptMode, ptModes, 2))
	{
		Profile.Frame.Mode = static_cast<EPathTracingMode>(ptMode);
	}

	if (Profile.Frame.Mode == EPathTracingMode::Reference)
	{
		ImGui::Text("Accumulated: %u spp", Inspected.AccumulatedSamples);
		int target = static_cast<int>(Profile.Frame.ReferenceSampleTarget);
		if (ImGui::InputInt("Stop at spp (0 = never)", &target))
		{
			Profile.Frame.ReferenceSampleTarget = static_cast<uint32>((std::max)(target, 0));
		}
	}
	else
	{
		ImGui::SliderFloat("Render scale", &Profile.Frame.RenderScale, 0.33f, 1.0f, "%.2f");
		int kind = static_cast<int>(GetUpscalerKind());
		const char* kinds[] = { "Off", "TAAU (in-house)", "DLSS (n/a)", "FSR (n/a)", "XeSS (n/a)" };
		if (ImGui::Combo("Upscaler / AA", &kind, kinds, 5))
		{
			SetUpscalerKind(static_cast<EUpscalerKind>(kind));
		}
		ImGui::Checkbox("Denoiser", &Profile.Frame.bDenoiser);
	}
	ImGui::Checkbox("Collect ray counters", &Profile.Frame.bCollectRayCounters);
	if (ImGui::Button("Reset accumulation / history"))
	{
		ResetAccumulation();
	}
	ImGui::SameLine();
	if (ImGui::Button("Capture"))
	{
		Inspected.RequestCapture(paths::GetCaptureDir() / ("capture_" + std::to_string(FrameNumber)), true);
	}

	if (ImGui::CollapsingHeader("Path tracer", ImGuiTreeNodeFlags_DefaultOpen))
	{
		Inspected.PathTracer->DrawSettingsUi(Profile.PathTracer, Profile.RadianceCache);
	}
	if (ImGui::CollapsingHeader("Denoiser"))
	{
		Inspected.Denoiser->DrawSettingsUi(Profile.Denoiser);
	}
	if (ImGui::CollapsingHeader("Upscaler / TAA"))
	{
		Inspected.Upscaler->DrawSettingsUi(Profile);
	}
	if (ImGui::CollapsingHeader("Post processing", ImGuiTreeNodeFlags_DefaultOpen))
	{
		Inspected.PostProcess->DrawSettingsUi(Profile.Post);
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
