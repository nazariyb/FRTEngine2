#pragma once

#include <utility>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/ComponentPool.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/EntityId.h"
#include "ECS/Hierarchy.h"
#include "ECS/View.h"
#include "Memory/Memory.h"


namespace frt
{
/**
 * Owns entity identity and every component pool.
 *
 * Entity records hold the generation for each index. Destroying an entity bumps its
 * generation and returns the index to a free list, so a stale handle fails validation
 * rather than aliasing whatever entity got the slot next.
 *
 * Pools are created on demand, indexed by ComponentId, and owned here so that destroying
 * an entity can strip all of its components without knowing their types.
 */
class FRT_CORE_API CWorld
{
public:
	CWorld();
	~CWorld();

	CWorld(const CWorld&) = delete;
	CWorld& operator=(const CWorld&) = delete;


	// ----- Entities -----

	EntityId Spawn();
	void     Destroy(EntityId InEntity);

	bool     IsAlive(EntityId InEntity) const;
	uint32   GetAliveCount() const { return AliveCount; }

	/** Highest index ever handed out - the span the sparse pages have to cover. */
	uint32   GetRecordCount() const { return Records.Count(); }


	// ----- Components -----

	template <class T, class... TArgs>
	T& Add(EntityId InEntity, TArgs&&... InArgs)
	{
		frt_assert(IsAlive(InEntity));
		return Pool<T>().Add(InEntity, std::forward<TArgs>(InArgs)...);
	}

	template <class T>
	void Remove(EntityId InEntity)
	{
		Pool<T>().Remove(InEntity);
	}

	template <class T>
	bool Has(EntityId InEntity) const
	{
		const TComponentPool<T>* pool = FindPool<T>();
		return pool != nullptr && pool->Contains(InEntity);
	}

	template <class T>       T& Get(EntityId InEntity)       { return Pool<T>().Get(InEntity); }
	template <class T> const T& Get(EntityId InEntity) const { return ConstPool<T>().Get(InEntity); }

	template <class T> T* TryGet(EntityId InEntity)
	{
		TComponentPool<T>* pool = FindPool<T>();
		return pool != nullptr ? pool->TryGet(InEntity) : nullptr;
	}

	template <class T> const T* TryGet(EntityId InEntity) const
	{
		const TComponentPool<T>* pool = FindPool<T>();
		return pool != nullptr ? pool->TryGet(InEntity) : nullptr;
	}


	// ----- Pools -----

	/** Creates the pool if this is the first use of T. */
	template <class T>
	TComponentPool<T>& Pool()
	{
		const ComponentId id = GetComponentId<T>();

		while (Pools.Count() <= id)
		{
			Pools.Add(nullptr);
		}

		if (Pools[id] == nullptr)
		{
			void* storage = memory::NewUnmanaged(sizeof(TComponentPool<T>));
			frt_assert(storage != nullptr);
			Pools[id] = new (storage) TComponentPool<T>();
		}

		return *static_cast<TComponentPool<T>*>(Pools[id]);
	}

	/** Asserts the pool exists - use FindPool when it might not. */
	template <class T>
	const TComponentPool<T>& ConstPool() const
	{
		const TComponentPool<T>* pool = FindPool<T>();
		frt_assert(pool != nullptr);
		return *pool;
	}

	template <class T>
	TComponentPool<T>* FindPool()
	{
		const ComponentId id = GetComponentId<T>();
		return id < Pools.Count() ? static_cast<TComponentPool<T>*>(Pools[id]) : nullptr;
	}

	template <class T>
	const TComponentPool<T>* FindPool() const
	{
		const ComponentId id = GetComponentId<T>();
		return id < Pools.Count() ? static_cast<const TComponentPool<T>*>(Pools[id]) : nullptr;
	}


	// ----- Queries -----

	template <class... TComponents>
	TView<TComponents...> View()
	{
		return TView<TComponents...>(this, Pool<std::remove_const_t<TComponents>>()...);
	}


	// ----- Hierarchy -----

	CHierarchy&       GetHierarchy()       { return Hierarchy; }
	const CHierarchy& GetHierarchy() const { return Hierarchy; }

private:
	struct SEntityRecord
	{
		uint32 Generation = 0u;
		bool   bAlive     = false;
	};

#pragma warning(push)
#pragma warning(disable: 4251)
	TArray<SEntityRecord>   Records;
	TArray<uint32>          FreeIndices;
	TArray<IComponentPool*> Pools;
#pragma warning(pop)

	uint32 AliveCount = 0u;

	// Declared after Pools so it is destroyed first, and constructed from pools that the
	// constructor creates up front rather than on demand.
	CHierarchy Hierarchy;
};


// Defined here rather than in View.h because it needs CWorld to be complete: the
// exclusion pools are looked up by type rather than passed in by the caller.
template <class... TComponents>
template <class... TExcludes>
TView<TComponents...> TView<TComponents...>::Exclude() const
{
	frt_assert(SourceWorld != nullptr);

	TView<TComponents...> result = *this;

	(result.Excluded.Add(static_cast<const IComponentPool*>(
		&SourceWorld->Pool<std::remove_const_t<TExcludes>>())), ...);

	return result;
}
}
