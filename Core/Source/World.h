#pragma once

#include <vector>

#include "CoreTypes.h"
#include "Entity.h"

namespace frt
{
	class CWorld
	{
	public:
		virtual ~CWorld() = default;

		virtual void Tick(float DeltaSeconds);
		virtual void Present(float DeltaSeconds, ID3D12GraphicsCommandList* CommandList);

		memory::TMemoryHandle<CEntity> SpawnEntity();

		std::vector<memory::TMemoryHandle<CEntity>> Entities;
	};
}
