#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Math/Math.h"


namespace frt
{
class CWorldScene;
}
namespace frt::graphics
{
class CCamera;
}


namespace frt::profiler
{
// Per-portal snapshot for the profiling CSV. ScreenCoverage is the analytical
// projected+clipped quad area / (W*H) for the session camera (occlusion-agnostic).
struct SPortalInfo
{
	bool   bEllipse        = false;
	float  WorldArea       = 0.0f;
	float  ScreenCoverage  = 0.0f; // [0,1]

	// TEMP debug — corner0 clip-space + NDC, to diagnose zero-coverage.
	float  DbgClipW0 = 0.0f;
	float  DbgNdc0X  = 0.0f;
	float  DbgNdc0Y  = 0.0f;
	int    DbgClipCount = 0;  // verts surviving near-plane clip
	int    DbgNdcCount  = 0;  // verts surviving viewport clip
};

// Everything about a scene that can impact / be impacted by portal pre-filtering.
// Captured once per profiling session (camera is fixed during a session).
struct SSceneDescriptor
{
	uint32 RenderWidth  = 0u;
	uint32 RenderHeight = 0u;

	uint32 EntityCount   = 0u;
	uint64 TriangleCount = 0u;

	// Explicit Comp_Light entities by kind. Sun (Directional) + Sky are always present
	// in the NEE list on top of these; tracked separately.
	uint32 PointLightCount       = 0u;
	uint32 DirectionalLightCount = 0u;
	uint32 AreaQuadLightCount    = 0u;

	uint32 PortalCount        = 0u;
	float  PortalCoverageTotal = 0.0f; // sum of per-portal screen coverage (may exceed 1 if overlapping)
	TArray<SPortalInfo> Portals;

	// Camera (session-fixed) + sky for reproducibility.
	Vector3f CameraPosition = {};
	Vector3f CameraRotation = {};
	graphics::SSkyConstants Sky;
};

FRT_CORE_API SSceneDescriptor CaptureSceneDescriptor (
	const CWorldScene& World,
	const graphics::CCamera& Camera,
	const graphics::SSkyConstants& Sky,
	uint32 RenderWidth,
	uint32 RenderHeight);
}
