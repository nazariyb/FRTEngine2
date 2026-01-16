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
#if !defined(FRT_HEADLESS)
	void Present (float DeltaSeconds, ID3D12GraphicsCommandList* CommandList);
#endif

	math::STransform Transform;

	graphics::Comp_RenderModel RenderModel;
};
}
