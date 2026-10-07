#pragma once

#include "Core.h"
#include "Graphics/Render/RenderView.h"


namespace frt
{
class CWorldScene;
}

namespace frt::graphics
{
class CCamera;
class CGpuScene;

struct SRenderExtractOptions
{
	bool bShowPortalMeshes = false;   // portal visualisation quads (camera rays only)
};

/**
 * Describes one world to the renderer for this frame: every drawable instance (legacy entity
 * list and ECS), lights, sky portals and the sky. Immediate mode - the GPU scene is rebuilt from
 * this every frame and diffs it against the last one itself - so the renderer never holds a
 * reference to a world, and which world is shown (the edit world, a play copy) is simply the one
 * extracted. Call once per frame, after the world's update, before CRenderer::PrepareScene.
 */
FRT_CORE_API void ExtractRenderScene (CWorldScene& World, const SRenderExtractOptions& Options, CGpuScene& Scene);

// A camera as the renderer consumes it (render space, with its projection parameters).
FRT_CORE_API SCameraView MakeCameraView (const CCamera& Camera);
}
