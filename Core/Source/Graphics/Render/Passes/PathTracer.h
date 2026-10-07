#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/RenderSettings.h"
#include "Graphics/Render/Passes/PathTracerPrograms.h"
#include "Graphics/Render/Passes/RadianceCache.h"
#include "Shared/PathTracerData.h"

namespace frt::graphics
{

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
 * Host side of PathTracing/PathTracer.hlsl for one render view: the render-resolution targets,
 * the view's radiance cache and the remodulation pass that turns (denoised) demodulated signals
 * back into radiance. The programs - ray tracing state objects and pipelines - are shared by
 * every view through CPathTracerPrograms.
 */
class FRT_CORE_API CPathTracer
{
public:
	CPathTracer () = default;
	CPathTracer (const CPathTracer&) = delete;
	CPathTracer& operator= (const CPathTracer&) = delete;

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines, const CPathTracerPrograms& Programs);
	void Shutdown ();
	void Resize (uint32 Width, uint32 Height);

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

	// Edits the given settings (the renderer's profile); readouts come from this instance.
	void DrawSettingsUi (SPathTracerSettings& Edit, SRadianceCacheSettings& EditCache);

	SPathTracerSettings Settings;
	CRadianceCache RadianceCache;        // real-time mode only; updated and resolved inside Dispatch

private:
	void DestroyTargets ();

	rhi::CDevice* Device = nullptr;
	const CPathTracerPrograms* Programs = nullptr;
	SPathTracerTargets Targets;
	uint32 Width = 0u;
	uint32 Height = 0u;
};
}
