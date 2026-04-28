#pragma once

#include "Graphics/Comp_Light.h"
#include "Graphics/Model.h"
#include "Math/Transform.h"


namespace frt
{
class CEntity
{
public:
	// temp implementation
	void Tick (float DeltaSeconds);

	const math::STransform& GetTransform () const { return Transform; }
	math::STransform Transform;
	Vector3f RotationSpeed = Vector3f::ZeroVector; // TODO: also temp

	// TODO: These are here temporary
	const memory::TRefShared<graphics::Comp_RenderModel>& GetRenderModel () const { return RenderModel; }
	memory::TRefShared<graphics::Comp_RenderModel> RenderModel;
	bool bRayTraced = true; // TODO: should be per-material

	// Optional light component. Light collector pushes an SLight entry per frame when set.
	memory::TRefShared<graphics::Comp_Light> Light;
};
}

