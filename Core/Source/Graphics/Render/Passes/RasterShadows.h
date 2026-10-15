#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Shared/RasterData.h"


namespace frt::graphics
{
class CGpuScene;

// True when the box is entirely outside one clip plane of ViewProj (row-vector convention,
// reversed Z: 0 <= z <= w): the draw cannot touch the view.
FRT_CORE_API bool IsBoxOutsideView (const DirectX::XMMATRIX& ViewProj, const DirectX::XMFLOAT3& Min, const DirectX::XMFLOAT3& Max);

/**
 * Shadow maps of the raster preview, the renderer that has to work without ray tracing:
 *   - the sun: one orthographic view fitted to the scene bounds. Its projection depends on the
 *     sun direction and the scene only, never on the camera, so shadows do not swim or shimmer
 *     when the view moves (no cascade snapping needed at this scene scale);
 *   - cube maps, six tiles of one depth atlas each: the emitter sphere lights
 *     (CGpuScene::GetRasterEmitters), then the portals of an enclosed scene, whose sky light is
 *     shadowed like an area light's.
 * All views are reversed Z (cleared to 0, GREATER_EQUAL). Receivers filter them with PCF and
 * bias themselves (normal offset), see Raster/RasterPreview.hlsl.
 */
class FRT_CORE_API CRasterShadows
{
public:
	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();
	// Writes the emitters (up to MaxEmitters), the views and the shadow-map indices into
	// Constants and renders the views (all of them unshadowed when !bShadows). Reads the portals
	// the caller put into Constants.
	void Execute (CRenderContext& Context, const CGpuScene& Scene, bool bShadows, uint32 MaxEmitters, gpu::SRasterConstants& Constants);

	static constexpr uint32 SunShadowSize = 2048u;
	static constexpr uint32 CubeTileSize = 512u;

private:
	void RenderView (CRenderContext& Context, const CGpuScene& Scene, uint32 ViewIndex, const D3D12_VIEWPORT& Viewport,
		const DirectX::XMMATRIX& ViewProj, uint32 SkipInstance);

	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	rhi::HPipeline OpaquePipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline MaskedPipeline = rhi::InvalidPipelineHandle;
	rhi::SGpuTexture SunShadow;
	rhi::SGpuTexture LocalShadow;     // 6 x RasterMaxCubeShadows tiles of CubeTileSize
};
}
