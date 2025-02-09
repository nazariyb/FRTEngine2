#pragma once

#include <vector>

#include "CoreTypes.h"
#include "Entity.h"

namespace frt
{
	class CWorld
	{
	public:
		CWorld() = delete;
		explicit CWorld(memory::TMemoryHandle<graphics::Renderer> InRenderer); 
		virtual ~CWorld() = default;

		virtual void Tick(float DeltaSeconds);
		virtual void Present(float DeltaSeconds, ID3D12GraphicsCommandList* CommandList);

		void CopyConstantData();

		memory::TMemoryHandle<CEntity> SpawnEntity();

		memory::TMemoryHandle<graphics::Renderer> Renderer;
		std::vector<memory::TMemoryHandle<CEntity>> Entities;
	};
}
