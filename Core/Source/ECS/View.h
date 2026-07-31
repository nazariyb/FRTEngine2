#pragma once

#include <tuple>
#include <type_traits>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/ComponentPool.h"
#include "ECS/EntityId.h"


namespace frt
{
class CWorld;


/**
 * Iterates every entity holding all of TComponents, minus any excluded types.
 *
 * Access is declared by constness of the template arguments, not by wrapper types:
 *
 *     for (auto [id, xf, vel] : World.View<Comp_LocalTransform, const Comp_LinearVelocity>())
 *
 * writes the first and reads the second. That is free, cannot drift out of sync with the
 * loop body, and is what the pool's change tracking keys off - a const argument reaches
 * the pool's const accessor and leaves its version alone, so a read-only system does not
 * dirty everything it touches.
 *
 * Iteration drives off the smallest pool and does a sparse lookup for the rest, so cost
 * scales with the rarest component rather than the most common one. Owning groups, which
 * turn the whole thing into a linear walk, come later; this interface does not change
 * when they do.
 *
 * A view holds raw pool pointers. Any structural change - adding or removing a component,
 * destroying an entity - invalidates it, exactly as it invalidates component references.
 * Queue those through a command buffer instead.
 */
template <class... TComponents>
class TView
{
	static_assert(sizeof...(TComponents) > 0u, "A view needs at least one component type");

	template <class T>
	using TPoolFor = TComponentPool<std::remove_const_t<T>>;

	static constexpr uint32 ComponentCount = sizeof...(TComponents);

public:
	TView(CWorld* InWorld, TPoolFor<TComponents>&... InPools)
		: Pools(&InPools...)
		, Generic{ static_cast<const IComponentPool*>(&InPools)... }
		, SourceWorld(InWorld)
	{
		// Drive off the rarest component: it bounds how many entities are ever visited.
		uint32 bestCount = ~0u;
		for (uint32 i = 0u; i < ComponentCount; ++i)
		{
			const uint32 count = Generic[i]->Count();
			if (count < bestCount)
			{
				bestCount = count;
				DriverIndex = i;
			}
		}
	}

	/**
	 * Skips entities holding any of TExcludes. Chainable.
	 *
	 * Returns BY VALUE, deliberately. Returning a reference makes the natural spelling
	 *
	 *     for (auto [id, x] : World.View<A>().Exclude<B>())
	 *
	 * dangle: the range initializer is then an lvalue reference, so C++20 lifetime
	 * extension does not reach the View<A>() temporary, which dies at the end of the
	 * full expression and leaves iteration reading freed memory. C++23 fixed this
	 * (P2718R0); until then a prvalue return is what makes the obvious code correct.
	 * The copy is a handful of pointers, made once per loop.
	 *
	 * Defined in World.h, where CWorld is complete.
	 */
	template <class... TExcludes>
	TView Exclude() const;

	class CIterator
	{
	public:
		CIterator(const TView* InView, uint32 InIndex)
			: View(InView)
			, Index(InIndex)
		{
			SkipToMatch();
		}

		bool operator!=(const CIterator& Rhs) const { return Index != Rhs.Index; }

		CIterator& operator++()
		{
			++Index;
			SkipToMatch();
			return *this;
		}

		std::tuple<EntityId, TComponents&...> operator*() const
		{
			return View->Fetch(View->Driver()->EntityAt(Index));
		}

	private:
		void SkipToMatch()
		{
			const uint32 count = View->Driver()->Count();
			while (Index < count && !View->Matches(View->Driver()->EntityAt(Index)))
			{
				++Index;
			}
		}

		const TView* View = nullptr;
		uint32       Index = 0u;
	};

	CIterator begin() const { return CIterator(this, 0u); }
	CIterator end() const   { return CIterator(this, Driver()->Count()); }

	/** Upper bound only: the driver pool's size, before intersection and exclusion. */
	uint32 GetDriverCount() const { return Driver()->Count(); }

private:
	const IComponentPool* Driver() const { return Generic[DriverIndex]; }

	bool Matches(EntityId InEntity) const
	{
		for (uint32 i = 0u; i < ComponentCount; ++i)
		{
			if (i != DriverIndex && !Generic[i]->Contains(InEntity))
			{
				return false;
			}
		}

		for (uint32 i = 0u; i < Excluded.Count(); ++i)
		{
			if (Excluded[i]->Contains(InEntity))
			{
				return false;
			}
		}

		return true;
	}

	template <class T>
	static T& FetchOne(TPoolFor<T>* InPool, EntityId InEntity)
	{
		// Routing const arguments to the pool's const accessor is what keeps a read-only
		// view from bumping the change version.
		if constexpr (std::is_const_v<T>)
		{
			return static_cast<const TPoolFor<T>*>(InPool)->Get(InEntity);
		}
		else
		{
			return InPool->Get(InEntity);
		}
	}

	std::tuple<EntityId, TComponents&...> Fetch(EntityId InEntity) const
	{
		return std::apply([&](auto*... InPools)
		{
			return std::tuple<EntityId, TComponents&...>(
				InEntity, FetchOne<TComponents>(InPools, InEntity)...);
		}, Pools);
	}

	/** Typed, for building the yielded tuple. */
	std::tuple<TPoolFor<TComponents>*...> Pools;

	/** Type-erased, for the driver / intersection logic, which needs no types. */
	const IComponentPool* Generic[ComponentCount];

#pragma warning(push)
#pragma warning(disable: 4251)
	TArray<const IComponentPool*> Excluded;
#pragma warning(pop)

	CWorld* SourceWorld = nullptr;
	uint32  DriverIndex = 0u;
};
}
