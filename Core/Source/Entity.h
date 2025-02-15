#pragma once

#include "CoreTypes.h"
#include "Graphics/Model.h"
#include "Math/Math.h"
#include "Math/Transform.h"

namespace frt
{
	class CEntity
	{
	public:
		// temp implementation
		void Tick(float DeltaSeconds);
		void Present(float DeltaSeconds, ID3D12GraphicsCommandList* CommandList);

		math::STransform Transform;

		// TODO: for now, to simplify experimenting and development of other features, the following structure is used:
		// 1 entity has 1 mesh. Later, when render, scene manager, and other things are more developed,
		// it will be easier to improve this too
		// graphics::Model Model;
		graphics::SMesh Mesh;
	};
}
