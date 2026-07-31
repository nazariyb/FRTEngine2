#include "ECS/Hierarchy.h"


namespace frt
{
bool CHierarchy::HasParent(EntityId InEntity) const
{
	const Comp_Parent* parent = ParentPool.TryGet(InEntity);
	return parent != nullptr && parent->Parent != InvalidEntity;
}

EntityId CHierarchy::GetParent(EntityId InEntity) const
{
	const Comp_Parent* parent = ParentPool.TryGet(InEntity);
	return parent != nullptr ? parent->Parent : InvalidEntity;
}

uint16 CHierarchy::GetDepth(EntityId InEntity) const
{
	const Comp_Parent* parent = ParentPool.TryGet(InEntity);
	return parent != nullptr ? parent->Depth : 0u;
}

EntityId CHierarchy::GetFirstChild(EntityId InEntity) const
{
	const Comp_Children* children = ChildrenPool.TryGet(InEntity);
	return children != nullptr ? children->FirstChild : InvalidEntity;
}

EntityId CHierarchy::GetNextSibling(EntityId InEntity) const
{
	const Comp_Parent* parent = ParentPool.TryGet(InEntity);
	return parent != nullptr ? parent->NextSibling : InvalidEntity;
}

uint32 CHierarchy::GetChildCount(EntityId InEntity) const
{
	const Comp_Children* children = ChildrenPool.TryGet(InEntity);
	return children != nullptr ? children->Count : 0u;
}

bool CHierarchy::IsAncestorOf(EntityId InAncestor, EntityId InDescendant) const
{
	if (InAncestor == InvalidEntity || InDescendant == InvalidEntity)
	{
		return false;
	}

	EntityId current = GetParent(InDescendant);

	// Bounded by the number of parented entities: even if the links were somehow already
	// cyclic, this terminates rather than hanging.
	uint32 guard = ParentPool.Count() + 1u;

	while (current != InvalidEntity && guard-- > 0u)
	{
		if (current == InAncestor)
		{
			return true;
		}

		current = GetParent(current);
	}

	return false;
}


bool CHierarchy::SetParent(EntityId InChild, EntityId InParent)
{
	if (!InChild.IsValid())
	{
		return false;
	}

	// An entity cannot parent itself, and cannot be parented to its own descendant -
	// either would produce a cycle, and depth propagation would then never terminate.
	if (InChild == InParent)
	{
		return false;
	}

	if (InParent != InvalidEntity && IsAncestorOf(InChild, InParent))
	{
		return false;
	}

	// Already correct - nothing to do, and in particular no topology bump.
	if (InParent == InvalidEntity)
	{
		if (!ParentPool.Contains(InChild))
		{
			return true;
		}
	}
	else if (GetParent(InChild) == InParent)
	{
		return true;
	}

	UnlinkFromParent(InChild);

	if (InParent == InvalidEntity)
	{
		ParentPool.Remove(InChild);
		PropagateDepth(InChild, 0u);
		++TopologyVersion;
		return true;
	}

	LinkToParent(InChild, InParent);
	PropagateDepth(InChild, static_cast<uint16>(GetDepth(InParent) + 1u));

	++TopologyVersion;
	return true;
}

void CHierarchy::DetachFromParent(EntityId InChild)
{
	if (!ParentPool.Contains(InChild))
	{
		return;
	}

	UnlinkFromParent(InChild);
	ParentPool.Remove(InChild);
	PropagateDepth(InChild, 0u);

	++TopologyVersion;
}

void CHierarchy::OnEntityDestroyed(EntityId InEntity)
{
	UnlinkFromParent(InEntity);
	ParentPool.Remove(InEntity);

	// Promote children to roots. Collect first: making a child a root unlinks it, which
	// mutates the very list being walked.
	TArray<EntityId> orphans;
	ForEachChild(InEntity, [&](EntityId InChild) { orphans.Add(InChild); });

	for (uint32 i = 0u; i < orphans.Count(); ++i)
	{
		UnlinkFromParent(orphans[i]);
		ParentPool.Remove(orphans[i]);
		PropagateDepth(orphans[i], 0u);
	}

	ChildrenPool.Remove(InEntity);

	++TopologyVersion;
}


void CHierarchy::LinkToParent(EntityId InChild, EntityId InParent)
{
	Comp_Parent& childLink = ParentPool.Contains(InChild)
		? ParentPool.Get(InChild)
		: ParentPool.Add(InChild);

	Comp_Children& parentLink = ChildrenPool.Contains(InParent)
		? ChildrenPool.Get(InParent)
		: ChildrenPool.Add(InParent);

	// Insert at the head: O(1), and child order within a parent is not meaningful.
	const EntityId oldFirst = parentLink.FirstChild;

	childLink.Parent      = InParent;
	childLink.PrevSibling = InvalidEntity;
	childLink.NextSibling = oldFirst;

	if (oldFirst != InvalidEntity)
	{
		ParentPool.Get(oldFirst).PrevSibling = InChild;
	}

	parentLink.FirstChild = InChild;
	++parentLink.Count;
}

void CHierarchy::UnlinkFromParent(EntityId InChild)
{
	Comp_Parent* childLink = ParentPool.TryGet(InChild);
	if (childLink == nullptr || childLink->Parent == InvalidEntity)
	{
		return;
	}

	const EntityId parent = childLink->Parent;
	const EntityId prev   = childLink->PrevSibling;
	const EntityId next   = childLink->NextSibling;

	if (prev != InvalidEntity)
	{
		ParentPool.Get(prev).NextSibling = next;
	}

	if (next != InvalidEntity)
	{
		ParentPool.Get(next).PrevSibling = prev;
	}

	if (Comp_Children* parentLink = ChildrenPool.TryGet(parent))
	{
		// Only the head pointer needs fixing, and only when this was the head.
		if (parentLink->FirstChild == InChild)
		{
			parentLink->FirstChild = next;
		}

		frt_assert(parentLink->Count > 0u);
		--parentLink->Count;

		// Drop the component entirely once the last child leaves, so "has children"
		// stays equivalent to "is in the pool".
		if (parentLink->Count == 0u)
		{
			ChildrenPool.Remove(parent);
		}
	}

	childLink->Parent      = InvalidEntity;
	childLink->PrevSibling = InvalidEntity;
	childLink->NextSibling = InvalidEntity;
}

void CHierarchy::PropagateDepth(EntityId InRoot, uint16 InDepth)
{
	// Iterative rather than recursive: a deep chain would otherwise put its whole length
	// on the call stack, and nothing bounds how deep a user-built hierarchy goes.
	//
	// Each entry carries its own depth. Deriving depth from position in the traversal
	// instead only works for a breadth-first queue - with a stack the pop order is not
	// level order, so a node and its children interleave.
	ScratchStack.Clear();

	if (Comp_Parent* rootLink = ParentPool.TryGet(InRoot))
	{
		rootLink->Depth = InDepth;
	}

	const uint16 childDepth = static_cast<uint16>(InDepth + 1u);
	ForEachChild(InRoot, [&](EntityId InChild)
	{
		ScratchStack.Add(SDepthEntry{ InChild, childDepth });
	});

	while (!ScratchStack.IsEmpty())
	{
		const SDepthEntry current = ScratchStack.Last();
		ScratchStack.RemoveAt<false>(static_cast<int64>(ScratchStack.Count() - 1u));

		if (Comp_Parent* link = ParentPool.TryGet(current.Entity))
		{
			link->Depth = current.Depth;
		}

		const uint16 nextDepth = static_cast<uint16>(current.Depth + 1u);
		ForEachChild(current.Entity, [&](EntityId InChild)
		{
			ScratchStack.Add(SDepthEntry{ InChild, nextDepth });
		});
	}
}


void CHierarchy::RefreshDepthOrder()
{
	if (DepthOrderBuiltAt == TopologyVersion)
	{
		return;
	}

	DepthOrdered.Clear();
	LevelStarts.Clear();
	MaxDepth = 0u;

	const uint32 count = ParentPool.Count();
	if (count == 0u)
	{
		DepthOrderBuiltAt = TopologyVersion;
		return;
	}

	// Const reference deliberately: AtDense's mutable overload bumps the pool's change
	// version, and rebuilding an index is a read, not a write. Using it here would mark
	// every transform dirty each time the topology changed.
	const TComponentPool<Comp_Parent>& parents = ParentPool;

	// Counting sort by depth. Depths are small, dense and bounded, so this beats a
	// comparison sort and keeps the rebuild linear.
	for (uint32 i = 0u; i < count; ++i)
	{
		const uint16 depth = parents.AtDense(i).Depth;
		if (depth > MaxDepth)
		{
			MaxDepth = depth;
		}
	}

	TArray<uint32> counts;
	counts.SetSize(static_cast<uint32>(MaxDepth) + 2u, 0u);

	for (uint32 i = 0u; i < count; ++i)
	{
		++counts[parents.AtDense(i).Depth];
	}

	LevelStarts.SetSize(static_cast<uint32>(MaxDepth) + 2u, 0u);

	uint32 running = 0u;
	for (uint32 depth = 0u; depth <= static_cast<uint32>(MaxDepth) + 1u; ++depth)
	{
		LevelStarts[depth] = running;
		running += counts[depth];
	}

	DepthOrdered.SetSize(count, InvalidEntity);

	TArray<uint32> cursors = LevelStarts;
	for (uint32 i = 0u; i < count; ++i)
	{
		const uint16 depth = parents.AtDense(i).Depth;
		DepthOrdered[cursors[depth]++] = parents.EntityAt(i);
	}

	DepthOrderBuiltAt = TopologyVersion;
}

void CHierarchy::GetLevelRange(uint16 InDepth, uint32& OutStart, uint32& OutEnd) const
{
	if (InDepth + 1u >= LevelStarts.Count())
	{
		OutStart = 0u;
		OutEnd = 0u;
		return;
	}

	OutStart = LevelStarts[InDepth];
	OutEnd = LevelStarts[InDepth + 1u];
}
}
