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
		graphics::Model Model;
	};
}
