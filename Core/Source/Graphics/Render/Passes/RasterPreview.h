#pragma once

#include "Graphics/Render/Passes/RasterShadows.h"
#include "Graphics/Render/RenderContext.h"


namespace frt::graphics
{
class CGpuScene;

struct SRasterPreviewSettings
{
	uint32 DebugMode = 0u;           // gpu::RasterDebug_*
	bool bShadows = true;            // sun, emitter and portal shadow maps
	bool bEmitters = true;           // emissive instances light the scene as sphere lights
	bool bAmbientOcclusion = true;   // GTAO from the depth prepass
	float AmbientOcclusionRadius = 1.0f;   // world units
};

/**
 * Forward raster preview into HDR + motion vectors + linear view depth, so the same temporal
 * upscaler and post chain run on it. Reversed-Z depth (cleared to 0, GREATER_EQUAL). It is also
 * the renderer of GPUs without ray tracing, so all of its lighting is raster-only: shadow maps
 * (CRasterShadows), prefiltered sky lighting and, for enclosed scenes, the sky through the
 * portals plus the scene's energy-balance ambient (see Raster/RasterPreview.hlsl).
 * Frame: shadow views -> depth prepass -> GTAO (Raster/AmbientOcclusion.hlsl) -> forward
 * shading against the prepass depth (no depth writes) -> sky -> blended surfaces.
 */
class FRT_CORE_API CRasterPreview
{
public:
	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();
	void Resize (uint32 Width, uint32 Height);
	void Execute (CRenderContext& Context, const CGpuScene& Scene, float MipBias);
	void DrawSettingsUi ();

	rhi::SGpuTexture& GetColor () { return Color; }
	rhi::SGpuTexture& GetMotionVectors () { return Motion; }
	rhi::SGpuTexture& GetViewZ () { return ViewZ; }

	SRasterPreviewSettings Settings;

private:
	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	rhi::HPipeline DepthPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline DepthMaskedPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline AoPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline AoFilterPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline OpaquePipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline BlendPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline SkyPipeline = rhi::InvalidPipelineHandle;
	CRasterShadows Shadows;
	rhi::SGpuTexture Color;
	rhi::SGpuTexture Motion;
	rhi::SGpuTexture ViewZ;
	rhi::SGpuTexture Depth;
	rhi::SGpuTexture AoRaw;
	rhi::SGpuTexture Ao;
	uint32 Width = 0u;
	uint32 Height = 0u;
};
}
