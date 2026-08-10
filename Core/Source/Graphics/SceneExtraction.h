#pragma once

#include <DirectXMath.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/EntityId.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Math/Math.h"


namespace frt
{
class CWorld;
}


namespace frt::graphics
{
// Only ever referenced through a pointer here; Model.h owns the definition.
struct SRenderModel;


/**
 * Projects ECS entities into the per-frame GPU lists.
 *
 * Renderer-side on purpose. SLight and SPortal are GPU layouts whose sizes are asserted
 * against CoreTypes.hlsli, the coordinate flip is a DirectX convention, and the whole
 * shape of these functions changes if the backend does. None of that is the ECS's
 * business, so the dependency runs Graphics -> ECS and not the other way round.
 *
 * Free functions rather than systems. There is no state worth keeping between frames -
 * the caller already owns the destination array - and being a plain call rather than a
 * phase removes any question about running before or after Sys_Transform: call them once
 * the world transforms are current.
 *
 * A copy is unavoidable here and is not a missing optimization. The GPU structs are a
 * FUSION of two pools (Comp_Light has no position; that lives in Comp_WorldTransform), a
 * FILTER (disabled entries must not reach the array at all, since LightCount drives the
 * shader loop), and a CONVERSION (enum to uint32, derived area, engine space to DirectX).
 * Uploading a component pool directly would mean making the component itself the GPU
 * struct - worth it for a large instance buffer, not for a handful of lights.
 *
 * Both APPEND. The sun and sky entries come from sky settings rather than any entity, and
 * during migration the CEntity scan still contributes, so the caller controls the order:
 *
 *     Lights.Clear();
 *     Lights.Add(sun);
 *     Lights.Add(sky);
 *     // ... existing CEntity scan ...
 *     ExtractLights(World, Lights);
 */

/**
 * Appends one SLight per enabled entity holding Comp_WorldTransform + Comp_Light.
 *
 * Takes a non-const world only because pools are created on demand. Nothing is written:
 * the views request their components by const reference, so the pools' change versions
 * are left alone and downstream change detection stays meaningful.
 */
FRT_CORE_API void ExtractLights (CWorld& InWorld, TArray<SLight>& OutLights);

/** Appends one SPortal per enabled entity holding Comp_WorldTransform + Comp_Portal. */
FRT_CORE_API void ExtractPortals (CWorld& InWorld, TArray<SPortal>& OutPortals);


/** One drawable, in the form the acceleration-structure build needs. */
struct SMeshInstance
{
	/** For reverse lookup - picking in the editor, MIS against a surface light. */
	EntityId Entity = InvalidEntity;

	/** Borrowed. The Comp_RenderModel holding the strong reference outlives the frame. */
	const SRenderModel* Model = nullptr;

	/**
	 * Already in DirectX handedness and 3x4 row-major, so it drops straight into
	 * D3D12_RAYTRACING_INSTANCE_DESC::Transform - the same bytes
	 * STransform::GetRaytracingTransform produces today.
	 */
	math::TMatrix3x4<float> Transform;
};


/**
 * Appends one SMeshInstance per VISIBLE entity holding Comp_WorldTransform +
 * Comp_RenderModel with a model.
 *
 * ORDER IS STABLE while the instance set is unchanged, and that is the property the
 * acceleration structure depends on. Instances are emitted in the render-model pool's dense
 * order, and swap-and-pop is the only thing that reorders it - so as long as no
 * Comp_RenderModel is added or removed, two successive calls produce identical entries at
 * identical indices.
 *
 * That matters because everything the TLAS build derives is positional: InstanceID is the
 * array index and is visible to shaders, and each instance's hit-group offset is a running
 * sum of the section counts before it, which pins the shader-binding-table layout. Reorder
 * the array and both silently change meaning.
 *
 * So the caller decides refit-versus-rebuild from the pool's structural version:
 *
 *     const uint64 structural = World.Pool<Comp_RenderModel>().GetStructuralVersion();
 *     if (structural != LastStructural) { rebuild; LastStructural = structural; }
 *     else                             { refit transforms in place; }
 *
 * A change there means the set moved, which needs a rebuild regardless - the instance count
 * and every hit-group offset after the change are different. Visibility is a filter rather
 * than a structural change, so toggling bVisible shifts positions WITHOUT bumping that
 * version; treat a visibility change as a rebuild too, or keep hidden entities in the array
 * and mask them.
 *
 * InOrigin is subtracted from world translations before narrowing to 32-bit. Pass zero for
 * the absolute transforms the renderer uses today; pass the camera position to go
 * camera-relative, which is what a double-precision build needs to keep instances accurate
 * far from the origin.
 */
FRT_CORE_API void ExtractMeshInstances (
	CWorld& InWorld,
	const Vector3r& InOrigin,
	TArray<SMeshInstance>& OutInstances);


/**
 * Turns an acceleration-structure instance transform back into the world matrix the
 * rasteriser's object constants want.
 *
 * The instance form is the world matrix transposed with its constant (0, 0, 0, 1) column
 * dropped - see STransform::GetRaytracingTransform. This undoes both, so raster and
 * raytracing can be driven from one transform per drawable instead of two that have to be
 * kept in agreement.
 *
 * Only valid for an affine transform, which is what a scene node produces: a projective
 * last column would not survive the round trip.
 */
FRT_CORE_API DirectX::XMFLOAT4X4 ToWorldMatrix (const DirectX::XMFLOAT3X4& InInstanceTransform);
}
