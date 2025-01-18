#include "World.h"

using namespace frt;

void CWorld::Tick(float DeltaSeconds)
{
}

void CWorld::Present(float DeltaSeconds, ID3D12GraphicsCommandList* CommandList)
{
	for (auto& entity : Entities)
	{
		entity->Present(DeltaSeconds, CommandList);
	}
}

memory::TMemoryHandle<CEntity> CWorld::SpawnEntity()
{
	auto newEntity = memory::New<CEntity>();
	Entities.push_back(newEntity);
	return newEntity;
}
