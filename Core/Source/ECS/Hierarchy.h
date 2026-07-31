#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/ComponentPool.h"
#include "ECS/Comp_Children.h"
#include "ECS/Comp_Parent.h"


namespace frt
{
/**
 * Owns the parent/child topology and every operation that has to keep it consistent.
 *
 * The links themselves are plain data in Comp_Parent and Comp_Children, but they are
 * only ever correct as a set: attaching a child touches the child's parent pointer, both
 * of its new siblings, the new parent's head pointer and count, and the depth of the
 * entire subtree beneath it. Writing those fields directly is how a tree gets silently
 * corrupted, so the pools are private and everything goes through here.
 *
 * Depth ordering is exposed as a cached index rather than by sorting the pool. A sparse
 * set's dense order is dictated by insertion and swap-and-pop, so keeping it sorted would
 * mean re-sorting on every structural change; the index is rebuilt only when the topology
 * actually changes, which is rare.
 */
class FRT_CORE_API CHierarchy
{
public:
	/**
	 * Borrows its pools rather than owning them, so CWorld remains the single owner of
	 * every component pool and Destroy can tear an entity down uniformly. The pools must
	 * outlive the hierarchy.
	 */
	CHierarchy(TComponentPool<Comp_Parent>& InParentPool, TComponentPool<Comp_Children>& InChildrenPool)
		: ParentPool(InParentPool)
		, ChildrenPool(InChildrenPool)
	{}

	/** Returns false and changes nothing if the link would create a cycle. */
	bool SetParent(EntityId InChild, EntityId InParent);

	/** Makes InChild a root. No-op if it already is one. */
	void DetachFromParent(EntityId InChild);

	/**
	 * Unlinks InEntity and promotes its children to roots.
	 *
	 * Orphaning rather than cascading: destroying a subtree is a policy decision for the
	 * caller, and doing it implicitly here would make a single Destroy silently remove an
	 * unbounded number of entities. Use ForEachDescendant first when a cascade is wanted.
	 */
	void OnEntityDestroyed(EntityId InEntity);


	// ----- Queries -----

	bool     HasParent(EntityId InEntity) const;
	EntityId GetParent(EntityId InEntity) const;
	uint16   GetDepth(EntityId InEntity) const;

	EntityId GetFirstChild(EntityId InEntity) const;
	EntityId GetNextSibling(EntityId InEntity) const;
	uint32   GetChildCount(EntityId InEntity) const;

	/** True if InAncestor is InDescendant's parent, grandparent, and so on. */
	bool IsAncestorOf(EntityId InAncestor, EntityId InDescendant) const;

	/** Direct children only. Safe to read during; do not restructure inside the callback. */
	template <class TFunc>
	void ForEachChild(EntityId InEntity, TFunc&& InFunc) const
	{
		EntityId child = GetFirstChild(InEntity);
		while (child != InvalidEntity)
		{
			// Read the next link before invoking, so a callback that detaches the current
			// child cannot strand the walk.
			const EntityId next = GetNextSibling(child);
			InFunc(child);
			child = next;
		}
	}

	/** Whole subtree below InEntity, depth-first, excluding InEntity itself. */
	template <class TFunc>
	void ForEachDescendant(EntityId InEntity, TFunc&& InFunc) const
	{
		TArray<EntityId> stack;
		ForEachChild(InEntity, [&](EntityId InChild) { stack.Add(InChild); });

		while (!stack.IsEmpty())
		{
			const EntityId current = stack.Last();
			stack.RemoveAt<false>(static_cast<int64>(stack.Count() - 1u));

			InFunc(current);

			ForEachChild(current, [&](EntityId InChild) { stack.Add(InChild); });
		}
	}


	// ----- Depth-ordered index -----
	//
	// Covers only entities that HAVE a parent, so depth 0 (roots) is deliberately absent -
	// Sys_Transform handles roots with a separate flat pass and uses this for levels 1..N.

	/** Rebuilds the index if the topology changed since the last call. */
	void RefreshDepthOrder();

	const TArray<EntityId>& GetDepthOrdered() const { return DepthOrdered; }

	uint16 GetMaxDepth() const { return MaxDepth; }

	/** Half-open [Start, End) range into GetDepthOrdered() for one depth level. */
	void GetLevelRange(uint16 InDepth, uint32& OutStart, uint32& OutEnd) const;

	/** Bumped by any structural change. */
	uint64 GetTopologyVersion() const { return TopologyVersion; }

	uint32 GetParentedCount() const { return ParentPool.Count(); }


	// Direct pool access for systems that iterate rather than restructure.
	const TComponentPool<Comp_Parent>&   GetParentPool() const   { return ParentPool; }
	const TComponentPool<Comp_Children>& GetChildrenPool() const { return ChildrenPool; }

private:
	/** Each pending node carries its own depth, so the walk needs no level bookkeeping. */
	struct SDepthEntry
	{
		EntityId Entity;
		uint16   Depth;
	};

	void LinkToParent(EntityId InChild, EntityId InParent);
	void UnlinkFromParent(EntityId InChild);
	void PropagateDepth(EntityId InRoot, uint16 InDepth);

	TComponentPool<Comp_Parent>&   ParentPool;
	TComponentPool<Comp_Children>& ChildrenPool;

#pragma warning(push)
#pragma warning(disable: 4251)
	TArray<EntityId> DepthOrdered;
	TArray<uint32>   LevelStarts;

	/** Reused across depth propagation so restructuring does not allocate per call. */
	TArray<SDepthEntry> ScratchStack;
#pragma warning(pop)

	uint64 TopologyVersion      = 1ull;
	uint64 DepthOrderBuiltAt    = 0ull;
	uint16 MaxDepth             = 0u;
};
}
