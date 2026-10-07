#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/Passes/RadianceCache.h"
#include "Shared/PathTracerData.h"


namespace frt::graphics
{
struct SPathTracerSettings
{
	uint32 SamplesPerPixel = 1u;
	uint32 MaxBounces = 4u;
	uint32 RussianRouletteDepth = 3u;
	uint32 RisCandidates = 4u;
	uint32 RisCandidatesSecondary = 1u;  // at bounce vertices: indirect noise is dominated by BSDF sampling there
	float IndirectClamp = 20.0f;         // real time only (reference mode never clamps); 0 = off
	float MinRoughness = 0.0f;
	bool bShaderExecutionReordering = false; // measured net-negative here (+1.8 ms on RTX 4080): one uber shading path in raygen leaves little divergence to sort, and the reorder spills the path state
	bool bInlineRayQueries = true;        // camera/bounce rays via RayQuery: -11% vs TraceRay (no RT-stack spill of the path state)
	bool bNormalMaps = true;
	bool bPathRegularization = false;
	bool bPortalGuiding = true;
	bool bPortalPreFilter = false;       // legacy thesis estimator (biased if portals are incomplete)
	bool bPrimarySurfaceReplacement = true; // real time: delta-mirror pixels give the denoiser/TAA the reflected surface's guide
	bool bDisableNee = false;            // validation: BSDF sampling only
	bool bDisableBsdfMis = false;        // validation: NEE only for sampleable emitters
	bool bWhiteFurnace = false;          // validation: albedo 1, environment 1, no lights
	uint32 DebugMode = gpu::PathDebug_None;
};

// Render-resolution targets written by the path tracer each frame.
struct SPathTracerTargets
{
	rhi::SGpuTexture Diffuse;           // RGBA16F demodulated diffuse + hit distance
	rhi::SGpuTexture Specular;          // RGBA16F demodulated specular + hit distance
	rhi::SGpuTexture NormalRoughness;   // RGBA16F
	rhi::SGpuTexture ViewZ;             // R32F
	rhi::SGpuTexture MotionVectors;     // RG16F
	rhi::SGpuTexture DiffuseAlbedo;     // RGBA8 (UNORM) demodulation factors
	rhi::SGpuTexture SpecularAlbedo;    // RGBA16F
	rhi::SGpuTexture Emission;          // RGBA16F primary emission / sky
	rhi::SGpuTexture Composite;         // RGBA16F radiance (before denoising), also remodulation target
	rhi::SGpuTexture Accumulation;      // RGBA32F reference accumulation (rgb mean, a = samples)
	rhi::SGpuTexture Debug;             // RGBA16F debug view
};

/**
 * Host side of PathTracing/PathTracer.hlsl: ray tracing state objects (a Shader Execution
 * Reordering build on SM 6.9 devices and a plain SM 6.8 build), the shader table, the
 * render-resolution targets and the remodulation pass that turns (denoised) demodulated
 * signals back into radiance.
 */
class FRT_CORE_API CPathTracer
{
public:
	CPathTracer () = default;
	CPathTracer (const CPathTracer&) = delete;
	CPathTracer& operator= (const CPathTracer&) = delete;

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();
	void Resize (uint32 Width, uint32 Height);

	// (Re)builds the state objects whose DXIL library changed (startup, hot reload). Must run
	// before IsReady() can report true; Dispatch() also calls it.
	void UpdatePipelines (ID3D12GraphicsCommandList* CommandList);

	// Traces the frame. bReference selects progressive accumulation (and full-mixture BSDF
	// weights); otherwise the demodulated lobe split for the denoiser is produced, after the
	// radiance cache update + resolve that the real-time paths then query.
	void Dispatch (CRenderContext& Context, bool bReference, bool bAccumulate);

	// Composite = DiffuseIn * DiffuseAlbedo + SpecularIn * SpecularAlbedo + Emission.
	void Remodulate (CRenderContext& Context, rhi::SGpuTexture& DiffuseIn, rhi::SGpuTexture& SpecularIn, rhi::SGpuTexture& Output);

	SPathTracerTargets& GetTargets () { return Targets; }
	bool IsReady () const;
	bool IsSerActive () const;
	uint32 GetWidth () const { return Width; }
	uint32 GetHeight () const { return Height; }

	void DrawSettingsUi ();

	SPathTracerSettings Settings;
	CRadianceCache RadianceCache;        // real-time mode only; updated and resolved inside Dispatch

private:
	struct SPipelineVariant
	{
		rhi::HShaderLibrary Library = rhi::InvalidPipelineHandle;
		uint32 BuiltVersion = 0u;
		rhi::ComPtr<ID3D12StateObject> StateObject;
		rhi::SGpuBuffer ShaderTable;
		D3D12_DISPATCH_RAYS_DESC DispatchTemplate = {};
		bool bValid = false;
	};

	bool BuildVariant (SPipelineVariant& Variant, const wchar_t* Name, ID3D12GraphicsCommandList* CommandList);
	void DestroyTargets ();

	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	SPipelineVariant SerVariant;
	SPipelineVariant BaseVariant;
	rhi::HPipeline RemodulatePipeline = rhi::InvalidPipelineHandle;
	SPathTracerTargets Targets;
	uint32 Width = 0u;
	uint32 Height = 0u;
};
}
