#include "ECS/ComponentRegistry.h"

#include <cstring>


namespace frt
{
CComponentRegistry& CComponentRegistry::Get()
{
	// One instance, and because this function is exported from Core every module
	// resolves to the same object.
	//
	// Safe as a plain static: the registry allocates nothing, so its construction and
	// destruction are not ordered against GameInstance's CMemoryPool.
	static CComponentRegistry instance;
	return instance;
}

ComponentId CComponentRegistry::Register(const SComponentInfo& InInfo)
{
	frt_assert(InInfo.Name != nullptr);

	// Idempotent by name. Two modules registering the same type must agree on the id,
	// and a module loaded later must not append a duplicate.
	if (const SComponentInfo* existing = FindByName(InInfo.Name))
	{
		// Two DISTINCT types under one name would share an id, and therefore one pool
		// reinterpreted as both - silent memory corruption. Size is a cheap proxy that
		// catches the ordinary case; names are the serialized identity, so a genuine
		// clash has to be resolved by renaming one of them.
		frt_assert(existing->Size == InInfo.Size);

		return existing->Id;
	}

	// Raise MaxComponentTypes if this ever fires; it is a real cap, not a soft one.
	frt_assert(Count < MaxComponentTypes);

	SComponentInfo& stored = Components[Count];
	stored = InInfo;
	stored.Id = static_cast<ComponentId>(Count);

	++Count;

	return stored.Id;
}

const SComponentInfo* CComponentRegistry::Find(ComponentId InId) const
{
	if (InId >= Count)
	{
		return nullptr;
	}

	return &Components[InId];
}

const SComponentInfo* CComponentRegistry::FindByName(const char* InName) const
{
	if (InName == nullptr)
	{
		return nullptr;
	}

	for (uint32 i = 0u; i < Count; ++i)
	{
		if (std::strcmp(Components[i].Name, InName) == 0)
		{
			return &Components[i];
		}
	}

	return nullptr;
}
}
