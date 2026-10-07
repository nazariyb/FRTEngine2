#include "ECS/World.h"


namespace frt
{
CWorld::CWorld()
	// The hierarchy borrows these two, so they must exist before it is constructed
	// rather than being created lazily on first use like every other pool.
	: Hierarchy(Pool<Comp_Parent>(), Pool<Comp_Children>())
{
}

CWorld::~CWorld()
{
	for (uint32 i = 0u; i < Pools.Count(); ++i)
	{
		if (Pools[i] == nullptr)
		{
			continue;
		}

		// Virtual destructor, then release the storage: NewUnmanaged / DestroyUnmanaged
		// only move raw bytes and never run a destructor.
		Pools[i]->~IComponentPool();
		memory::DestroyUnmanaged(Pools[i]);
		Pools[i] = nullptr;
	}
}

void CWorld::CopyFrom(const CWorld& InSource)
{
	frt_assert(AliveCount == 0u && Records.IsEmpty());

	Records = InSource.Records;
	FreeIndices = InSource.FreeIndices;
	AliveCount = InSource.AliveCount;

	for (uint32 id = 0u; id < InSource.Pools.Count(); ++id)
	{
		const IComponentPool* source = InSource.Pools[id];
		if (source == nullptr)
		{
			continue;
		}
		while (Pools.Count() <= id)
		{
			Pools.Add(nullptr);
		}
		// Copied into the existing pool when there is one: the hierarchy holds references to
		// the parent and children pools.
		if (Pools[id] == nullptr)
		{
			Pools[id] = source->CreateEmpty();
		}
		Pools[id]->CopyFrom(*source);
	}

	Hierarchy.InvalidateCaches();
}

EntityId CWorld::Spawn()
{
	++AliveCount;

	// Recycle a retired index if one is available. The record's generation was already
	// bumped on destruction, so the handle differs from the one that occupied it before.
	if (!FreeIndices.IsEmpty())
	{
		const uint32 index = FreeIndices.Last();
		FreeIndices.RemoveAt<false>(static_cast<int64>(FreeIndices.Count() - 1u));

		Records[index].bAlive = true;
		return EntityId(index, Records[index].Generation);
	}

	frt_assert(Records.Count() <= EntityId::MaxIndex);

	const uint32 index = Records.Count();
	SEntityRecord& record = Records.Add();
	record.Generation = 0u;
	record.bAlive = true;

	return EntityId(index, record.Generation);
}

void CWorld::Destroy(EntityId InEntity)
{
	if (!IsAlive(InEntity))
	{
		return;
	}

	const uint32 index = InEntity.GetIndex();

	// Hierarchy first: it has to unlink siblings and re-root children while the parent
	// and child components are still present.
	Hierarchy.OnEntityDestroyed(InEntity);

	for (uint32 i = 0u; i < Pools.Count(); ++i)
	{
		if (Pools[i] != nullptr)
		{
			Pools[i]->Remove(InEntity);
		}
	}

	// Bump the generation so every outstanding handle to this index stops validating.
	// Wrapping is silent by construction - see EntityId's note on the 12-bit budget.
	Records[index].Generation = (Records[index].Generation + 1u) & EntityId::GenerationMask;
	Records[index].bAlive = false;

	FreeIndices.Add(index);
	--AliveCount;
}

bool CWorld::IsAlive(EntityId InEntity) const
{
	if (!InEntity.IsValid())
	{
		return false;
	}

	const uint32 index = InEntity.GetIndex();
	if (index >= Records.Count())
	{
		return false;
	}

	return Records[index].bAlive && Records[index].Generation == InEntity.GetGeneration();
}
}
