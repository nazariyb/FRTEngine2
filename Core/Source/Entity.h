#pragma once

#include "Graphics/Model.h"
#include "Math/Transform.h"


namespace frt
{
class CEntity
{
public:
	// temp implementation
	void Tick (float DeltaSeconds);

	math::STransform Transform;

	// TODO: These are here temporary
	graphics::Comp_RenderModel RenderModel;
	bool bRayTraced = true; // TODO: should be per-material
};
}
