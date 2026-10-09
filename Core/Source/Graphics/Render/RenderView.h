#pragma once

#include <DirectXMath.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "Containers/StringView.h"
#include "Graphics/Render/RenderSettings.h"
#include "Graphics/Render/RHI/GpuResources.h"
#include "Shared/FrameData.h"


namespace frt::graphics
{
class CDenoiser;
class CPathTracer;
class CPostProcess;
class CRasterPreview;
class IUpscaler;

// Camera as the renderer consumes it, already in render (DirectX) space.
struct SCameraView
{
	DirectX::XMFLOAT3 Position = {};
	DirectX::XMFLOAT3 Forward = { 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT3 Up = { 0.0f, 1.0f, 0.0f };
	float FieldOfViewY = 1.5707963f;     // radians (vertical)
	float NearZ = 0.05f;
};

// Where a view's final, display-encoded image goes.
enum class EViewTarget : uint8
{
	BackBuffer,   // straight into the swap chain (the standalone game: no copy)
	Texture,      // an owned R8G8B8A8_UNORM texture, sampled by the UI (editor viewports)
};

struct SRenderViewDesc
{
	// A constructor rather than an aggregate: MSVC 14.38 rejects (C7595) a string literal
	// initializing an FStringLiteral member of a braced aggregate built at run time, and views are
	// made as CreateView({ "Main", EViewTarget::BackBuffer }).
	constexpr SRenderViewDesc (FStringLiteral InName = "View", EViewTarget InTarget = EViewTarget::Texture) noexcept
		: Name(InName)
		, Target(InTarget)
	{}

	// A literal: the GPU profiler reads scope tags frames after they were recorded.
	FStringLiteral Name;
	EViewTarget Target;
};

/**
 * One rendered point of view and everything that persists from one of its frames to the next
 * (the renderer's counterpart of Unreal's FSceneViewState / HDRP's HDCamera):
 *   - its own instance of every view-dependent pass - path tracer targets and radiance cache,
 *     denoiser, upscaler and post-process histories, raster preview targets;
 *   - temporal state: previous camera matrices and jitter, progressive accumulation;
 *   - its size and, for texture views, its output texture.
 * Scene data (the GPU scene, TLAS, materials, textures) and GPU programs are shared by every
 * view; settings come from the renderer's SRenderProfile.
 *
 * Created and destroyed through CRenderer, which owns the storage. A view that is not rendered
 * on a frame does no GPU work, and starts its histories over when it is rendered again.
 */
class FRT_CORE_API CRenderView
{
public:
	~CRenderView ();
	CRenderView (const CRenderView&) = delete;
	CRenderView& operator= (const CRenderView&) = delete;

	FStringLiteral GetName () const { return Desc.Name; }
	EViewTarget GetTarget () const { return Desc.Target; }

	// Path tracing or raster preview. Requested only: a device without DXR 1.1, or a path tracer
	// still compiling, renders raster (CRenderer::IsTracing).
	void SetRenderMode (ERenderMode Mode) { RenderMode = Mode == ERenderMode::Hybrid ? ERenderMode::Raytracing : Mode; }
	ERenderMode GetRenderMode () const { return RenderMode; }

	uint32 GetRenderWidth () const { return RenderWidth; }
	uint32 GetRenderHeight () const { return RenderHeight; }
	uint32 GetDisplayWidth () const { return DisplayWidth; }
	uint32 GetDisplayHeight () const { return DisplayHeight; }
	uint32 GetAccumulatedSamples () const { return AccumulatedSamples; }

	// Texture views: the output as an ImGui texture id (the bindless SRV's GPU handle), 0 before
	// the first CRenderer::SetViewSize. A new id replaces it on every resize.
	uint64 GetTextureId () const { return TextureId; }

	// The next render starts every history over (camera cut, teleport).
	void ResetHistory () { bForceReset = true; }

	// Writes <Path>.png (this view, tone mapped, no UI) and optionally <Path>.pfm (linear HDR).
	void RequestCapture (const std::filesystem::path& Path, bool bHdr);

	// The view's pass instances, for the settings UI's per-view readouts.
	CPathTracer& GetPathTracer () { return *PathTracer; }
	CDenoiser& GetDenoiser () { return *Denoiser; }
	CPostProcess& GetPostProcess () { return *PostProcess; }
	IUpscaler* GetUpscaler () { return Upscaler.get(); }

private:
	friend class CRenderer;
	explicit CRenderView (const SRenderViewDesc& InDesc);

	SRenderViewDesc Desc;
	ERenderMode RenderMode = ERenderMode::Raytracing;

	std::unique_ptr<CPathTracer> PathTracer;
	std::unique_ptr<CDenoiser> Denoiser;
	std::unique_ptr<IUpscaler> Upscaler;
	std::unique_ptr<CPostProcess> PostProcess;
	std::unique_ptr<CRasterPreview> RasterPreview;

	rhi::SGpuTexture DisplayColor;           // upscaler output, display resolution, HDR
	rhi::SGpuTexture Output;                 // Texture views: post-process output, display-encoded
	rhi::SGpuTexture* LastHdrForCapture = nullptr;
	uint64 TextureId = 0u;

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

	uint64 LastRenderedFrame = ~0ull;        // renderer frame number
	uint64 SeenHistoryEpoch = 0u;            // CRenderer::HistoryEpoch at the last render
	uint64 SeenSceneEpoch = 0u;              // CRenderer::SceneEpoch at the last render

	struct SCaptureRequest
	{
		std::filesystem::path Path;
		bool bHdr = false;
	};
	std::vector<SCaptureRequest> CaptureRequests;
};
}
