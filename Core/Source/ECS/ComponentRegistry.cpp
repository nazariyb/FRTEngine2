#include "ECS/ComponentRegistry.h"

#include <algorithm>
#include <cstdint>
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
		const ComponentId id = existing->Id;
		if (Registrants[id] == nullptr)
		{
			// Released with its module: the reloaded module's build of the type, whose
			// layout may have changed since. Every pool of the old one is gone.
			SComponentInfo& stored = Components[id];
			stored = InInfo;
			stored.Id = id;
			stored.Name = Names[id];
			Registrants[id] = InInfo.Name;
			return id;
		}

		// Two DISTINCT types under one name would share an id, and therefore one pool
		// reinterpreted as both - silent memory corruption. Size is a cheap proxy that
		// catches the ordinary case; names are the serialized identity, so a genuine
		// clash has to be resolved by renaming one of them.
		frt_assert(existing->Size == InInfo.Size);

		return id;
	}

	// Raise MaxComponentTypes if this ever fires; it is a real cap, not a soft one.
	frt_assert(Count < MaxComponentTypes);
	frt_assert(std::strlen(InInfo.Name) <= MaxNameLength);

	// The name is copied: InInfo.Name is a literal in the registrant's image, which may be
	// unloaded long before the registry is done with the name.
	const ComponentId id = static_cast<ComponentId>(Count);
	const size_t nameLength = (std::min)(std::strlen(InInfo.Name), static_cast<size_t>(MaxNameLength));
	std::memcpy(Names[id], InInfo.Name, nameLength);
	Names[id][nameLength] = '\0';

	SComponentInfo& stored = Components[id];
	stored = InInfo;
	stored.Id = id;
	stored.Name = Names[id];
	Registrants[id] = InInfo.Name;

	++Count;

	return id;
}

void CComponentRegistry::ReleaseModule(const void* InImageBegin, const void* InImageEnd)
{
	const auto inImage = [InImageBegin, InImageEnd](const void* InAddress)
	{
		// As integers: comparing pointers into different objects is unspecified.
		const uintptr_t address = reinterpret_cast<uintptr_t>(InAddress);
		return address >= reinterpret_cast<uintptr_t>(InImageBegin) && address < reinterpret_cast<uintptr_t>(InImageEnd);
	};

	for (uint32 i = 0u; i < Count; ++i)
	{
		// The registrant's functions come from the same instantiation as its name, so they
		// leave with it.
		if (Registrants[i] != nullptr && inImage(Registrants[i]))
		{
			SComponentInfo& stored = Components[i];
			stored.Construct = nullptr;
			stored.MoveRange = nullptr;
			stored.Destruct = nullptr;
			Registrants[i] = nullptr;
		}
	}
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
