#pragma once

#include <type_traits>

#include "Core.h"
#include "CoreTypes.h"


namespace frt
{
using ComponentId = uint16;

inline constexpr ComponentId InvalidComponentId = ~ComponentId(0);


/**
 * Everything the world needs to handle a component type without knowing it statically.
 *
 * Serialization and replication hooks are declared here rather than bolted on later
 * because retrofitting them means revisiting every registration site. They may be null
 * until the corresponding system exists.
 */
struct SComponentInfo
{
	/** Compact, runtime-assigned. Storage and queries only - never serialized. */
	ComponentId  Id = InvalidComponentId;

	/** Stable across builds and runs. This is what a saved scene references. */
	const char*  Name = nullptr;

	uint32       Size = 0u;
	uint32       Alignment = 0u;

	/**
	 * Whether the type can be relocated with a raw copy. Pools currently require this
	 * (see TComponentPool's static_assert); the flag is recorded so the check can move
	 * to runtime once non-trivial components are supported through MoveRange.
	 */
	bool         bTriviallyRelocatable = false;

	/** Null until a component needs non-trivial construction / relocation / teardown. */
	void (*Construct)(void* InDest, uint32 InCount) = nullptr;
	void (*MoveRange)(void* InDest, void* InSource, uint32 InCount) = nullptr;
	void (*Destruct)(void* InDest, uint32 InCount) = nullptr;
};


/**
 * Specialize through FRT_DECLARE_COMPONENT to give a type a stable serialized name.
 * Registration is deliberately explicit: an implicit typeid-derived name would change
 * with the compiler and silently invalidate saved scenes.
 */
template <class T>
struct TComponentTraits
{
	static constexpr bool bDefined = false;
};


/**
 * The single source of component ids for the whole process.
 *
 * This exists because Core is a DLL. A counter inside a function template gets one
 * instance per module, so the same component type would be assigned different ids in
 * Core and in Demo - identical pool indices meaning different types, corrupting silently
 * rather than failing. Registration goes through this one exported instance and is
 * idempotent by Name, so whichever module asks first wins and everyone agrees.
 *
 * Storage is a fixed array and the registry never allocates. That is not a shortcut - it
 * is the only option that works. The registry's lifetime falls outside the engine pool's
 * window at BOTH ends: a component can be registered before GameInstance constructs its
 * CMemoryPool, and the registry is destroyed after that pool has been released. So it can
 * neither allocate at the start nor free at the end. Reaching for a second allocator
 * would only move the problem; not allocating removes it, and leaves the TLSF arena the
 * single owner of all engine memory.
 *
 * The cap is a real limit, not a guess to be quietly raised - exceeding it asserts.
 */
class FRT_CORE_API CComponentRegistry
{
public:
	/** Ids are uint16, but the practical count is dozens. ~16 KiB of static data. */
	static constexpr uint32 MaxComponentTypes = 256u;

	static CComponentRegistry& Get();

	/** Returns the existing id if Name is already registered. */
	ComponentId Register(const SComponentInfo& InInfo);

	const SComponentInfo* Find(ComponentId InId) const;
	const SComponentInfo* FindByName(const char* InName) const;

	uint32 GetCount() const { return Count; }

private:
	CComponentRegistry() = default;

	SComponentInfo Components[MaxComponentTypes] = {};
	uint32         Count = 0u;
};


namespace detail
{
template <class T>
SComponentInfo MakeComponentInfo()
{
	static_assert(TComponentTraits<T>::bDefined,
		"Component type is missing FRT_DECLARE_COMPONENT - it needs a stable name for serialization");

	SComponentInfo info;
	info.Name                  = TComponentTraits<T>::Name;
	info.Size                  = static_cast<uint32>(sizeof(T));
	info.Alignment             = static_cast<uint32>(alignof(T));
	info.bTriviallyRelocatable = std::is_trivially_copyable_v<T>;

	if constexpr (!std::is_trivially_default_constructible_v<T>)
	{
		info.Construct = [](void* InDest, uint32 InCount)
		{
			T* dest = static_cast<T*>(InDest);
			for (uint32 i = 0u; i < InCount; ++i)
			{
				new (dest + i) T();
			}
		};
	}

	if constexpr (!std::is_trivially_copyable_v<T>)
	{
		info.MoveRange = [](void* InDest, void* InSource, uint32 InCount)
		{
			T* dest = static_cast<T*>(InDest);
			T* source = static_cast<T*>(InSource);
			for (uint32 i = 0u; i < InCount; ++i)
			{
				new (dest + i) T(std::move(source[i]));
				source[i].~T();
			}
		};
	}

	if constexpr (!std::is_trivially_destructible_v<T>)
	{
		info.Destruct = [](void* InDest, uint32 InCount)
		{
			T* dest = static_cast<T*>(InDest);
			for (uint32 i = 0u; i < InCount; ++i)
			{
				dest[i].~T();
			}
		};
	}

	return info;
}
}


/**
 * Resolved once per type per module. The local static differs between Core and Demo,
 * but both route through the one registry, so both end up holding the same id.
 */
template <class T>
ComponentId GetComponentId()
{
	static const ComponentId id = CComponentRegistry::Get().Register(detail::MakeComponentInfo<T>());
	return id;
}
}


#define FRT_DECLARE_COMPONENT(ComponentType)\
	template <>\
	struct frt::TComponentTraits<ComponentType>\
	{\
		static constexpr bool bDefined = true;\
		static constexpr const char* Name = #ComponentType;\
	}

#define FRT_DECLARE_COMPONENT_NAMED(ComponentType, StableName)\
	template <>\
	struct frt::TComponentTraits<ComponentType>\
	{\
		static constexpr bool bDefined = true;\
		static constexpr const char* Name = StableName;\
	}
