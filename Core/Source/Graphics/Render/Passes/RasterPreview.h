#pragma once

#include "Graphics/Render/RenderContext.h"


namespace frt::graphics
{
class CGpuScene;

/**
 * Forward raster preview into HDR + motion vectors + linear view depth, so the same temporal
 * upscaler and post chain run on it. Reversed-Z depth (cleared to 0, GREATER_EQUAL).
 */
class FRT_CORE_API CRasterPreview
{
public:
	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();
	void Resize (uint32 Width, uint32 Height);
	void Execute (CRenderContext& Context, const CGpuScene& Scene, float MipBias);

	rhi::SGpuTexture& GetColor () { return Color; }
	rhi::SGpuTexture& GetMotionVectors () { return Motion; }
	rhi::SGpuTexture& GetViewZ () { return ViewZ; }

private:
	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	rhi::HPipeline OpaquePipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline BlendPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline SkyPipeline = rhi::InvalidPipelineHandle;
	rhi::SGpuTexture Color;
	rhi::SGpuTexture Motion;
	rhi::SGpuTexture ViewZ;
	rhi::SGpuTexture Depth;
	uint32 Width = 0u;
	uint32 Height = 0u;
};
}
