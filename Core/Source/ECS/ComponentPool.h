#pragma once

#include <type_traits>
#include <utility>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/EntityId.h"
#include "Memory/Memory.h"


namespace frt
{
/**
 * Type-erased pool interface, so the world can hold a heterogeneous list of pools and
 * still destroy an entity's components without knowing their types.
 */
// Not FRT_CORE_API: a pure interface with no data members and no out-of-line
// definitions has nothing for Core to export, and marking it dllimport would ask the
// linker for an implicit constructor and destructor that are never emitted.
class IComponentPool
{
public:
	virtual ~IComponentPool() = default;

	virtual ComponentId GetComponentId() const = 0;
	virtual uint32      Count() const = 0;
	virtual bool        Contains(EntityId InEntity) const = 0;

	/** No-op if the entity does not have this component. */
	virtual void        Remove(EntityId InEntity) = 0;

	virtual EntityId    EntityAt(uint32 InDenseIndex) const = 0;
};


/**
 * Sparse set storing one component type.
 *
 *   Sparse : entity index -> dense index   (paged, mostly holes)
 *   Dense  : dense index  -> entity        (packed)
 *   Data   : dense index  -> component     (packed, parallel to Dense)
 *
 * Lookup is Data[Sparse[index]] - two dependent loads. Iteration is a linear walk of
 * Data. Removal is swap-and-pop, which is O(1) but REORDERS the dense arrays: any
 * removal invalidates every component pointer into this pool. Only handles are stable.
 *
 * The sparse array is paged because it is indexed by entity index and would otherwise
 * have to span the whole id space - 1M entries x 4 bytes per component type, nearly all
 * of it holes. Pages are allocated only for id ranges actually in use.
 */
template <class T>
class TComponentPool final : public IComponentPool
{
public:
	// Non-trivial components are supported: TArray::ReAlloc move-constructs rather than
	// relocating raw bytes whenever the element type is not trivially copyable, chosen at
	// compile time so POD components pay nothing for it.
	//
	// Supported is not the same as advisable. A component holding a std::string costs an
	// allocation per entity and cannot be memcpy'd to the GPU or over a socket. Keep hot
	// components trivially copyable; reserve the rest for cold metadata like Comp_Name.
	static_assert(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>,
		"Component must be move-constructible and move-assignable: pool growth relocates by "
		"moving, and swap-and-pop removal move-assigns the tail element into the hole.");

	/** Entities per sparse page. 4096 x 4 bytes = one 16 KiB page. */
	static constexpr uint32 SparsePageSize = 4096u;

	/** Dense entries per change-tracking block. */
	static constexpr uint32 DirtyBlockSize = 64u;

	static constexpr uint32 InvalidDenseIndex = ~0u;


	// Qualified: the member GetComponentId() below would otherwise shadow the free
	// template and be parsed as a member template.
	TComponentPool()
		: ComponentIdValue(frt::GetComponentId<T>())
	{}

	~TComponentPool() override
	{
		FreeSparsePages();
	}

	TComponentPool(const TComponentPool&) = delete;
	TComponentPool& operator=(const TComponentPool&) = delete;


	// ----- IComponentPool -----

	ComponentId GetComponentId() const override { return ComponentIdValue; }
	uint32      Count() const override          { return Dense.Count(); }

	bool Contains(EntityId InEntity) const override
	{
		return FindDenseIndex(InEntity) != InvalidDenseIndex;
	}

	EntityId EntityAt(uint32 InDenseIndex) const override
	{
		frt_assert(InDenseIndex < Dense.Count());
		return Dense[InDenseIndex];
	}

	void Remove(EntityId InEntity) override
	{
		const uint32 denseIndex = FindDenseIndex(InEntity);
		if (denseIndex == InvalidDenseIndex)
		{
			return;
		}

		const uint32 lastIndex = Dense.Count() - 1u;

		// Swap-and-pop: move the last element into the hole, then fix the moved
		// entity's sparse entry so it still points at where its data now lives.
		if (denseIndex != lastIndex)
		{
			Dense[denseIndex] = Dense[lastIndex];
			Data[denseIndex]  = std::move(Data[lastIndex]);
			SetSparse(Dense[denseIndex], denseIndex);

			// The moved entity's data changed location, so both blocks are stale.
			MarkDirty(denseIndex);
		}

		SetSparse(InEntity, InvalidDenseIndex);
		MarkDirty(lastIndex);

		Dense.RemoveAt<false>(static_cast<int64>(lastIndex));
		Data.RemoveAt<false>(static_cast<int64>(lastIndex));

		++Version;
		++StructuralVersion;
	}


	// ----- Typed access -----

	/** Overwrites the existing component if the entity already has one. */
	template <class... TArgs>
	T& Add(EntityId InEntity, TArgs&&... InArgs)
	{
		frt_assert(InEntity.IsValid());

		const uint32 existing = FindDenseIndex(InEntity);
		if (existing != InvalidDenseIndex)
		{
			Data[existing] = T{ std::forward<TArgs>(InArgs)... };
			MarkDirty(existing);
			++Version;
			return Data[existing];
		}

		const uint32 denseIndex = Dense.Count();

		Dense.Add(InEntity);
		Data.Add(T{ std::forward<TArgs>(InArgs)... });
		SetSparse(InEntity, denseIndex);

		MarkDirty(denseIndex);
		++Version;
		++StructuralVersion;

		return Data[denseIndex];
	}

	/** Asserts the entity has the component. Bumps the version - this is a write. */
	T& Get(EntityId InEntity)
	{
		const uint32 denseIndex = FindDenseIndex(InEntity);
		frt_assert(denseIndex != InvalidDenseIndex);

		MarkDirty(denseIndex);
		++Version;

		return Data[denseIndex];
	}

	/** Read-only, so it does not mark anything dirty. */
	const T& Get(EntityId InEntity) const
	{
		const uint32 denseIndex = FindDenseIndex(InEntity);
		frt_assert(denseIndex != InvalidDenseIndex);

		return Data[denseIndex];
	}

	T* TryGet(EntityId InEntity)
	{
		const uint32 denseIndex = FindDenseIndex(InEntity);
		if (denseIndex == InvalidDenseIndex)
		{
			return nullptr;
		}

		MarkDirty(denseIndex);
		++Version;

		return &Data[denseIndex];
	}

	const T* TryGet(EntityId InEntity) const
	{
		const uint32 denseIndex = FindDenseIndex(InEntity);
		return denseIndex == InvalidDenseIndex ? nullptr : &Data[denseIndex];
	}


	// ----- Dense iteration -----
	// The const overloads are the ones to reach for in read-only systems: they leave
	// the change-tracking state alone.

	T*       GetData()                { return Data.GetData(); }
	const T* GetData() const          { return Data.GetData(); }

	const EntityId* GetEntities() const { return Dense.GetData(); }

	T&       AtDense(uint32 InIndex)       { MarkDirty(InIndex); ++Version; return Data[InIndex]; }
	const T& AtDense(uint32 InIndex) const { return Data[InIndex]; }


	// ----- Change tracking -----
	//
	// Conservative by construction. Swap-and-pop moves an entity between blocks, and
	// both the source and destination block are marked, so a block is never wrongly
	// reported clean - only wrongly reported dirty, which costs recomputation and
	// nothing else.

	uint64 GetVersion() const { return Version; }

	/**
	 * Bumped only when the SET of entities changes - an insert or a removal - and never by
	 * writing to a component that is already present.
	 *
	 * This is what makes dense order usable as a stable ordering. Swap-and-pop is the only
	 * thing that reorders the dense arrays, so while this value holds steady, iteration
	 * yields the same entities in the same positions every time. A consumer that has built
	 * something order-dependent - a TLAS with per-instance shader-table offsets, say - can
	 * refit against it and only rebuild when this changes.
	 */
	uint64 GetStructuralVersion() const { return StructuralVersion; }

	uint32 GetBlockCount() const
	{
		return (Dense.Count() + DirtyBlockSize - 1u) / DirtyBlockSize;
	}

	static uint32 BlockOf(uint32 InDenseIndex) { return InDenseIndex / DirtyBlockSize; }

	uint64 GetBlockVersion(uint32 InBlock) const
	{
		return InBlock < BlockVersions.Count() ? BlockVersions[InBlock] : 0ull;
	}

	bool IsBlockDirtySince(uint32 InBlock, uint64 InSinceVersion) const
	{
		return GetBlockVersion(InBlock) > InSinceVersion;
	}

	bool IsDirtySince(EntityId InEntity, uint64 InSinceVersion) const
	{
		const uint32 denseIndex = FindDenseIndex(InEntity);
		if (denseIndex == InvalidDenseIndex)
		{
			return false;
		}

		return IsBlockDirtySince(BlockOf(denseIndex), InSinceVersion);
	}

	void Clear()
	{
		Dense.Clear();
		Data.Clear();
		BlockVersions.Clear();
		FreeSparsePages();
		++Version;
		++StructuralVersion;
	}

private:
	uint32 FindDenseIndex(EntityId InEntity) const
	{
		if (!InEntity.IsValid())
		{
			return InvalidDenseIndex;
		}

		const uint32 index    = InEntity.GetIndex();
		const uint32 pageIdx  = index / SparsePageSize;
		const uint32 pageSlot = index % SparsePageSize;

		if (pageIdx >= SparsePages.Count() || SparsePages[pageIdx] == nullptr)
		{
			return InvalidDenseIndex;
		}

		const uint32 denseIndex = SparsePages[pageIdx][pageSlot];
		if (denseIndex >= Dense.Count())
		{
			return InvalidDenseIndex;
		}

		// The round-trip is what makes this sound: the sparse slot may hold a stale
		// index from a previous occupant of this entity index, or from a generation
		// that has since been recycled.
		return Dense[denseIndex] == InEntity ? denseIndex : InvalidDenseIndex;
	}

	void SetSparse(EntityId InEntity, uint32 InDenseIndex)
	{
		const uint32 index    = InEntity.GetIndex();
		const uint32 pageIdx  = index / SparsePageSize;
		const uint32 pageSlot = index % SparsePageSize;

		while (SparsePages.Count() <= pageIdx)
		{
			SparsePages.Add(nullptr);
		}

		if (SparsePages[pageIdx] == nullptr)
		{
			uint32* page = static_cast<uint32*>(
				memory::NewUnmanaged(sizeof(uint32) * SparsePageSize));

			for (uint32 i = 0u; i < SparsePageSize; ++i)
			{
				page[i] = InvalidDenseIndex;
			}

			SparsePages[pageIdx] = page;
		}

		SparsePages[pageIdx][pageSlot] = InDenseIndex;
	}

	void MarkDirty(uint32 InDenseIndex)
	{
		const uint32 block = BlockOf(InDenseIndex);

		while (BlockVersions.Count() <= block)
		{
			BlockVersions.Add(0ull);
		}

		BlockVersions[block] = Version + 1ull;
	}

	void FreeSparsePages()
	{
		for (uint32 i = 0u; i < SparsePages.Count(); ++i)
		{
			if (SparsePages[i] != nullptr)
			{
				memory::DestroyUnmanaged(SparsePages[i]);
				SparsePages[i] = nullptr;
			}
		}

		SparsePages.Clear();
	}


	ComponentId      ComponentIdValue = InvalidComponentId;

	TArray<uint32*>  SparsePages;
	TArray<EntityId> Dense;
	TArray<T>        Data;

	uint64           Version = 0ull;
	uint64           StructuralVersion = 0ull;
	TArray<uint64>   BlockVersions;
};
}
