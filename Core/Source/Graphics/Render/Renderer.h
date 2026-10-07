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
class CPathTracer;
class CPathTracerPrograms;
class CDenoiser;
class IUpscaler;
class CPostProcess;
class CRasterPreview;
class CFrameCapture;

using Microsoft::WRL::ComPtr;

// Camera as the renderer consumes it, already in render (DirectX) space.
struct SCameraView
{
	DirectX::XMFLOAT3 Position = {};
	DirectX::XMFLOAT3 Forward = { 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT3 Up = { 0.0f, 1.0f, 0.0f };
};

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

	// ---- frame lifecycle ----
	// Tick: wait until the next frame slot is free on the GPU, then recycle its resources.
	void Tick ();
	// StartFrame: open the command list for this frame.
	void StartFrame ();
	// PrepareCurrentPass: scene upload, tracing/rasterisation, denoise, upscale, post -> back
	// buffer (left bound as render target for the UI).
	void PrepareCurrentPass ();
	// Draw: back buffer to present, submit, present, signal.
	void Draw ();

	// Writes Profile.RenderMode. The mode rendered is Raster whenever the device cannot trace.
	void SetRenderMode (ERenderMode Mode);
	ERenderMode GetRenderMode () const;
	bool IsRaytracingReady () const;
	bool ShouldRenderRaster () const;
	bool ShouldRenderRaytracing () const;
	bool IsReadyToRender () const;

	void SetCameraView (const SCameraView& View) { CameraView = View; }
	void ResetAccumulation () { bForceReset = true; }
	uint32 GetAccumulatedSamples () const { return AccumulatedSamples; }
	uint32 GetRenderWidth () const { return RenderWidth; }
	uint32 GetRenderHeight () const { return RenderHeight; }
	uint32 GetDisplayWidth () const { return DisplayWidth; }
	uint32 GetDisplayHeight () const { return DisplayHeight; }

	// Writes <Path>.png (display, tone mapped, no UI) and optionally <Path>.pfm (linear HDR).
	void RequestCapture (const std::filesystem::path& Path, bool bHdr);
	bool HasPendingCaptures () const;

	CMaterialLibrary& GetMaterialLibrary ();
	CGpuScene& GetScene () { return *Scene; }
	CPathTracer& GetPathTracer () { return *PathTracer; }
	CDenoiser& GetDenoiser () { return *Denoiser; }
	CPostProcess& GetPostProcess () { return *PostProcess; }
	IUpscaler* GetUpscaler () { return Upscaler.get(); }
	EUpscalerKind GetUpscalerKind () const;
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
	void RebindAfterUi ();

	void DrawRendererUi ();

	// Every setting the renderer applies, shared by all views (see RenderSettings.h). Written by
	// the settings UI, launch configuration and profiling sessions.
	SRenderProfile Profile;
	bool bVSyncEnabled = true;
	DXGI_RATIONAL DisplayRefreshRate = { 0u, 1u };

	frt::CEvent<> OnShaderDescriptorHeapRebuild; // kept for API compatibility; never fires

private:
	void CreateDevice ();
	void CreateSwapChain (bool bFullscreen);
	void WaitForFenceValue (uint64 Value);
	void UpdateResolution ();
	void BuildFrameConstants (gpu::SFrameConstants& Out, bool bSceneValid);
	uint64 ComputeAccumulationHash () const;
	void RecordRaytracing (CRenderContext& Context, bool bResetHistory);
	void RecordRaster (CRenderContext& Context, bool bResetHistory);
	void ApplyProfileToPasses ();
	void RecordPostAndPresentTarget (CRenderContext& Context, rhi::SGpuTexture& DisplayColor, rhi::SGpuTexture* Debug, bool bResetExposure);

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
	std::unique_ptr<CPathTracer> PathTracer;
	std::unique_ptr<CDenoiser> Denoiser;
	std::unique_ptr<IUpscaler> Upscaler;
	std::unique_ptr<IUpscaler> ReferenceResampler;     // reference mode: plain resample
	std::unique_ptr<CPostProcess> PostProcess;
	std::unique_ptr<CRasterPreview> RasterPreview;
	std::unique_ptr<CFrameCapture> Capture;

	rhi::SGpuTexture DisplayColor;           // upscaler output, display resolution
	rhi::SGpuTexture* LastHdrForCapture = nullptr;

	SCameraView CameraView;
	gpu::SFrameConstants FrameConstants = {};
	DirectX::XMFLOAT4X4 PrevViewProjNoJitter = {};
	DirectX::XMFLOAT3 PrevCameraPosition = {};
	float PrevJitter[2] = {};
	bool bHasPrevFrame = false;

	uint32 RenderWidth = 0u;
	uint32 RenderHeight = 0u;
	uint32 DisplayWidth = 0u;
	uint32 DisplayHeight = 0u;

	uint64 AccumulationHash = 0u;
	uint32 AccumulatedSamples = 0u;
	uint32 AccumulationFrames = 0u;
	bool bForceReset = true;
	EPathTracingMode PrevMode = EPathTracingMode::RealTime;
	ERenderMode PrevRenderMode = ERenderMode::Raytracing;

	struct SCaptureRequest
	{
		std::filesystem::path Path;
		bool bHdr = false;
	};
	std::vector<SCaptureRequest> CaptureRequests;
};
}
