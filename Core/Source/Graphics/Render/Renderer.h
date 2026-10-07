#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Event.h"
#include "GraphicsCoreTypes.h"
#include "MaterialLibrary.h"
#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/RenderSettings.h"
#include "Graphics/Render/RenderView.h"
#include "Graphics/Render/RHI/Device.h"
#include "Graphics/Render/RHI/Pipelines.h"
#include "Graphics/Render/RHI/ShaderCompiler.h"
#include "Profiler/GpuProfiler.h"
#include "Profiler/RayCounters.h"

namespace frt
{
class CWindow;
}

namespace frt::graphics
{
class CGpuScene;
class CPathTracerPrograms;
class IUpscaler;
class CFrameCapture;

using Microsoft::WRL::ComPtr;

/**
 * The device, the swap chain and the frame: everything shared by the views it renders.
 *
 * Shared, owned here: the D3D12 device and queue, the swap chain, frame slots and fences, the
 * RHI (descriptor heaps, upload ring, deferred releases), the shader compiler and pipeline
 * library, materials, the GPU scene (one per renderer: only the active world is described),
 * the path tracer's programs, the GPU profiler, ray counters and frame captures.
 * Per view (CRenderView): pass instances with their histories, targets, camera state.
 *
 * Frame, in this order:
 *   BeginFrame        wait for the frame slot, retire deferred releases. Before the UI, so
 *                     whatever the UI creates or destroys belongs to this frame.
 *   (UI: SetViewSize, view texture ids handed to ImGui)
 *   BeginRecording    open the command list
 *   PrepareScene      hot reload, GPU scene upload + acceleration structures (once per frame)
 *   RenderView        once per visible view: trace / rasterise, denoise, upscale, post
 *   BeginUiPass       back buffer bound for the UI (cleared when no view drew into it)
 *   EndFrame          submit, present, signal
 */
class FRT_CORE_API CRenderer
{
public:
	CRenderer () = delete;
	explicit CRenderer (CWindow* Window);
	~CRenderer ();
	CRenderer (const CRenderer&) = delete;
	CRenderer& operator= (const CRenderer&) = delete;

	void Resize (bool bNewFullscreenState);

	IDXGIAdapter1* GetAdapter ();
	ID3D12Device5* GetDevice ();
	ID3D12CommandQueue* GetCommandQueue ();
	ID3D12GraphicsCommandList4* GetCommandList ();
	rhi::CDevice& GetRhi () { return Rhi; }
	const rhi::SDeviceCaps& GetCaps () const { return Rhi.GetCaps(); }

	void InitializeRendering ();
	void FlushCommandQueue ();
	// Drops a frame whose recording an exception cut short: the open command list is closed
	// without being executed. No-op between frames. For shutdown paths, before the flush.
	void AbandonFrame ();

	// ---- frame (see the class comment) ----
	void BeginFrame ();
	void BeginRecording ();
	void PrepareScene ();
	void RenderView (CRenderView& View, const SCameraView& Camera);
	void BeginUiPass ();
	void EndFrame ();

	// ---- views ----
	CRenderView* CreateView (const SRenderViewDesc& Desc);
	// GPU resources are released once the frames that used them are done.
	void DestroyView (CRenderView* View);
	// Texture views, during the UI (after BeginFrame): a new output texture of this size, whose
	// id GetTextureId() returns right away; the frame's RenderView fills it. No-op if unchanged.
	void SetViewSize (CRenderView& View, uint32 Width, uint32 Height);
	// Every view starts its histories and accumulation over (scene edits, hot reload, Play/Stop).
	void ResetAccumulation () { ++HistoryEpoch; }

	// The device supports the path tracer (DXR 1.1 inline ray queries).
	bool CanTrace () const;
	bool IsRaytracingReady () const;
	// What a view actually renders this frame: its mode, unless the path tracer is unavailable.
	bool IsTracing (const CRenderView& View) const;
	bool IsReadyToRender () const;

	uint32 GetBackBufferWidth () const { return BackBufferWidth; }
	uint32 GetBackBufferHeight () const { return BackBufferHeight; }

	// Captures requested on any view and not yet written to disk.
	bool HasPendingCaptures () const;
	// <Path>.png of the whole window this frame - views and UI - as presented.
	void RequestBackBufferCapture (const std::filesystem::path& Path) { BackBufferCaptures.push_back(Path); }

	// Ray counters are one buffer per frame: only this view writes them (null: every view).
	void SetCountersView (const CRenderView* View) { CountersView = View; }

	CMaterialLibrary& GetMaterialLibrary ();
	CGpuScene& GetScene () { return *Scene; }
	EUpscalerKind GetUpscalerKind () const { return Profile.Upscaler; }
	// Writes Profile.Upscaler if that kind is available; views switch on their next render.
	void SetUpscalerKind (EUpscalerKind Kind);
	rhi::CPipelineLibrary& GetPipelines () { return Pipelines; }

	profiler::CGpuProfiler& GetGpuProfiler () { return GpuProfiler; }
	const profiler::CGpuProfiler& GetGpuProfiler () const { return GpuProfiler; }
	profiler::CRayCounters& GetRayCounters () { return RayCounters; }
	const profiler::CRayCounters& GetRayCounters () const { return RayCounters; }

	// ImGui allocates its font atlas and user textures out of the bindless heap.
	void AllocateImGuiDescriptor (D3D12_CPU_DESCRIPTOR_HANDLE* OutCpu, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpu);
	void FreeImGuiDescriptor (D3D12_CPU_DESCRIPTOR_HANDLE Cpu, D3D12_GPU_DESCRIPTOR_HANDLE Gpu);
	ID3D12DescriptorHeap* GetBindlessHeap () { return Rhi.GetBindlessHeap().GetHeap(); }

	// Edits Profile; per-view readouts (accumulation, histograms, history) come from Inspected.
	void DrawRendererUi (CRenderView& Inspected);

	// Every setting the renderer applies, shared by all views (see RenderSettings.h). Written by
	// the settings UI, launch configuration and profiling sessions.
	SRenderProfile Profile;
	bool bVSyncEnabled = true;
	DXGI_RATIONAL DisplayRefreshRate = { 0u, 1u };

private:
	void CreateDevice ();
	void CreateSwapChain (bool bFullscreen);
	void WaitForFenceValue (uint64 Value);
	void ApplyProfile (CRenderView& View);
	void UpdateResolution (CRenderView& View);
	void BuildFrameConstants (CRenderView& View, const SCameraView& Camera, gpu::SFrameConstants& Out, bool bSceneValid) const;
	uint64 ComputeAccumulationHash (const CRenderView& View) const;
	void RecordRaytracing (CRenderView& View, CRenderContext& Context, bool bResetHistory);
	void RecordRaster (CRenderView& View, CRenderContext& Context, bool bResetHistory);
	void RecordPost (CRenderView& View, CRenderContext& Context, rhi::SGpuTexture* Debug, bool bResetExposure);
	void ShutdownView (CRenderView& View);

	CWindow* Window = nullptr;

	ComPtr<IDXGIFactory6> Factory;
	ComPtr<IDXGIAdapter1> Adapter;
	ComPtr<ID3D12Device5> Device;
	ComPtr<ID3D12CommandQueue> CommandQueue;
	ComPtr<ID3D12GraphicsCommandList4> CommandList;
	ComPtr<IDXGISwapChain3> SwapChain;
	bool bTearingSupported = false;

	static constexpr uint32 SwapChainBufferCount = 3u;
	ComPtr<ID3D12Resource> BackBuffers[SwapChainBufferCount];
	uint32 BackBufferRtv[SwapChainBufferCount] = {};
	D3D12_RESOURCE_STATES BackBufferState[SwapChainBufferCount] = {};
	uint32 CurrentBackBuffer = 0u;
	bool bFrameBuffersReady = false;
	bool bResizing = false;
	bool bCommandListRecording = false;

	struct SFrameSlot
	{
		ComPtr<ID3D12CommandAllocator> Allocator;
		uint64 FenceValue = 0u;
	};
	SFrameSlot Frames[rhi::MaxFramesInFlight];
	uint32 FrameSlot = 0u;
	uint64 FrameNumber = 0u;

	ComPtr<ID3D12Fence> Fence;
	uint64 FenceValue = 0u;
	HANDLE FenceEvent = nullptr;

	rhi::CDevice Rhi;
	rhi::CShaderCompiler ShaderCompiler;
	rhi::CPipelineLibrary Pipelines;
	CMaterialLibrary MaterialLibrary;
	profiler::CGpuProfiler GpuProfiler;
	profiler::CRayCounters RayCounters;
	uint32 RayCounterUav = rhi::InvalidDescriptorIndex;

	std::unique_ptr<CGpuScene> Scene;
	std::unique_ptr<CPathTracerPrograms> PathTracerPrograms;   // shared by every view's CPathTracer
	std::unique_ptr<IUpscaler> ReferenceResampler;             // reference mode: plain resample (stateless)
	std::unique_ptr<CFrameCapture> Capture;

	std::vector<std::unique_ptr<CRenderView>> Views;
	uint64 HistoryEpoch = 1u;              // views reset when theirs differs (ResetAccumulation)
	uint64 SceneEpoch = 1u;                // bumped when PrepareScene finds the content changed:
	                                       // a view's accumulation restarts when it last saw another
	bool bBackBufferDrawn = false;         // a back-buffer view drew this frame: the UI composes over it
	const CRenderView* CountersView = nullptr;
	std::vector<std::filesystem::path> BackBufferCaptures;

	uint32 BackBufferWidth = 0u;
	uint32 BackBufferHeight = 0u;
};
}
