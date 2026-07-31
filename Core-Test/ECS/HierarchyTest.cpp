// A dropped sibling link corrupts silently and only surfaces systems later, so every
// structural operation here is checked against a full re-walk rather than a spot check.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include "ECS/CoreComponents.h"
#include "ECS/Hierarchy.h"
#include "ECS/World.h"

using namespace frt;

namespace
{
EntityId E (uint32 InIndex)
{
	return EntityId(InIndex, 0u);
}

/**
 * Asserts the child list of InParent is internally consistent: forward and backward
 * walks agree, Count matches the walk, every child points back, and the head has no
 * PrevSibling.
 */
void CheckListIntegrity (const CHierarchy& InH, EntityId InParent)
{
	uint32 forward = 0u;
	EntityId last = InvalidEntity;

	InH.ForEachChild(InParent, [&](EntityId InChild)
	{
		++forward;
		EXPECT_EQ(InH.GetParent(InChild), InParent) << "child does not point back at parent";
		last = InChild;
	});

	EXPECT_EQ(forward, InH.GetChildCount(InParent)) << "Count disagrees with the forward walk";

	uint32 backward = 0u;
	EntityId cursor = last;
	while (cursor != InvalidEntity)
	{
		++backward;
		const Comp_Parent* link = InH.GetParentPool().TryGet(cursor);
		cursor = link != nullptr ? link->PrevSibling : InvalidEntity;
	}

	EXPECT_EQ(backward, forward) << "backward walk disagrees with the forward walk";

	const EntityId head = InH.GetFirstChild(InParent);
	if (head != InvalidEntity)
	{
		const Comp_Parent* link = InH.GetParentPool().TryGet(head);
		ASSERT_NE(link, nullptr);
		EXPECT_EQ(link->PrevSibling, InvalidEntity) << "head of the list has a PrevSibling";
	}
}
}


TEST(Hierarchy, BasicAttach)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	EXPECT_TRUE(h.SetParent(E(1), E(0)));
	EXPECT_EQ(h.GetParent(E(1)), E(0));
	EXPECT_EQ(h.GetDepth(E(1)), 1u);
	EXPECT_EQ(h.GetChildCount(E(0)), 1u);
	EXPECT_EQ(h.GetFirstChild(E(0)), E(1));
	EXPECT_FALSE(h.HasParent(E(0)));
	EXPECT_EQ(h.GetDepth(E(0)), 0u);

	CheckListIntegrity(h, E(0));
}

TEST(Hierarchy, RemovalFromHeadMiddleAndTail)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	for (uint32 i = 1u; i <= 5u; ++i)
	{
		h.SetParent(E(i), E(0));
	}

	ASSERT_EQ(h.GetChildCount(E(0)), 5u);
	CheckListIntegrity(h, E(0));

	// Children are inserted at the head, so the list reads 5,4,3,2,1.
	h.DetachFromParent(E(5)); // head
	EXPECT_EQ(h.GetChildCount(E(0)), 4u);
	CheckListIntegrity(h, E(0));

	h.DetachFromParent(E(3)); // middle
	EXPECT_EQ(h.GetChildCount(E(0)), 3u);
	CheckListIntegrity(h, E(0));

	h.DetachFromParent(E(1)); // tail
	EXPECT_EQ(h.GetChildCount(E(0)), 2u);
	CheckListIntegrity(h, E(0));

	EXPECT_FALSE(h.HasParent(E(5)));
	EXPECT_EQ(h.GetDepth(E(5)), 0u);
}

TEST(Hierarchy, ChildComponentDroppedWhenLastChildLeaves)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(0));
	h.DetachFromParent(E(1));
	h.DetachFromParent(E(2));

	EXPECT_EQ(h.GetChildCount(E(0)), 0u);
	EXPECT_EQ(h.GetFirstChild(E(0)), InvalidEntity);

	// "Has children" must stay equivalent to "is in the pool".
	EXPECT_FALSE(h.GetChildrenPool().Contains(E(0)));
}

TEST(Hierarchy, DepthPropagatesThroughDeepChain)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	for (uint32 i = 1u; i <= 20u; ++i)
	{
		h.SetParent(E(i), E(i - 1u));
	}

	EXPECT_EQ(h.GetDepth(E(20)), 20u);

	h.RefreshDepthOrder();
	EXPECT_EQ(h.GetMaxDepth(), 20u);
}

TEST(Hierarchy, ReparentShiftsWholeSubtree)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	for (uint32 i = 1u; i <= 20u; ++i)
	{
		h.SetParent(E(i), E(i - 1u));
	}

	ASSERT_TRUE(h.SetParent(E(10), E(100)));

	EXPECT_EQ(h.GetDepth(E(10)), 1u)  << "moved node";
	EXPECT_EQ(h.GetDepth(E(11)), 2u)  << "its child followed";
	EXPECT_EQ(h.GetDepth(E(20)), 11u) << "whole subtree followed";
	EXPECT_EQ(h.GetDepth(E(9)),  9u)  << "nodes above are untouched";

	h.DetachFromParent(E(10));
	EXPECT_EQ(h.GetDepth(E(10)), 0u);
	EXPECT_EQ(h.GetDepth(E(20)), 10u);
}

TEST(Hierarchy, RejectsCycles)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(1));
	h.SetParent(E(3), E(2));

	// Any of these would make depth propagation non-terminating.
	EXPECT_FALSE(h.SetParent(E(0), E(0))) << "self-parent";
	EXPECT_FALSE(h.SetParent(E(0), E(3))) << "ancestor under its own descendant";
	EXPECT_FALSE(h.SetParent(E(1), E(3))) << "mid-chain cycle";

	EXPECT_TRUE(h.SetParent(E(0), E(9))) << "unrelated parent is still allowed";
	EXPECT_EQ(h.GetDepth(E(3)), 4u) << "depths intact after rejections";
}

TEST(Hierarchy, AncestorQueryIsDirectional)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(1));
	h.SetParent(E(3), E(2));

	EXPECT_TRUE(h.IsAncestorOf(E(0), E(3)));
	EXPECT_TRUE(h.IsAncestorOf(E(2), E(3)));
	EXPECT_FALSE(h.IsAncestorOf(E(3), E(0)));
	EXPECT_FALSE(h.IsAncestorOf(E(3), E(3)));
}

TEST(Hierarchy, DestroyOrphansChildrenRatherThanCascading)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(1));
	h.SetParent(E(3), E(1));
	h.SetParent(E(4), E(1));

	h.OnEntityDestroyed(E(1));

	EXPECT_EQ(h.GetChildCount(E(0)), 0u) << "removed from its own parent";
	EXPECT_FALSE(h.HasParent(E(2)));
	EXPECT_FALSE(h.HasParent(E(3)));
	EXPECT_FALSE(h.HasParent(E(4)));
	EXPECT_EQ(h.GetDepth(E(2)), 0u);
	EXPECT_FALSE(h.GetChildrenPool().Contains(E(1)));

	CheckListIntegrity(h, E(0));
}

TEST(Hierarchy, DestroyRebasesGrandchildren)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(1));
	h.SetParent(E(3), E(2));

	h.OnEntityDestroyed(E(1));

	EXPECT_EQ(h.GetDepth(E(2)), 0u) << "orphan becomes a root";
	EXPECT_EQ(h.GetDepth(E(3)), 1u) << "grandchild rebased under it";
}

TEST(Hierarchy, ReparentBetweenLiveParents)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(10));
	h.SetParent(E(2), E(10));
	h.SetParent(E(3), E(20));

	h.SetParent(E(1), E(20));

	EXPECT_EQ(h.GetChildCount(E(10)), 1u);
	EXPECT_EQ(h.GetChildCount(E(20)), 2u);
	EXPECT_EQ(h.GetParent(E(1)), E(20));

	CheckListIntegrity(h, E(10));
	CheckListIntegrity(h, E(20));
}

TEST(Hierarchy, RedundantSetParentIsANoOp)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(20));
	h.SetParent(E(2), E(20));

	const uint64 version = h.GetTopologyVersion();

	EXPECT_TRUE(h.SetParent(E(1), E(20)));
	EXPECT_EQ(h.GetTopologyVersion(), version) << "no topology bump for a no-op";
	EXPECT_EQ(h.GetChildCount(E(20)), 2u) << "no duplicate link";

	CheckListIntegrity(h, E(20));
}

TEST(Hierarchy, DepthOrderedIndexPartitionsByLevel)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	//        0
	//     1     2
	//    3 4   5
	//   6
	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(0));
	h.SetParent(E(3), E(1));
	h.SetParent(E(4), E(1));
	h.SetParent(E(5), E(2));
	h.SetParent(E(6), E(3));

	h.RefreshDepthOrder();

	ASSERT_EQ(h.GetMaxDepth(), 3u);
	ASSERT_EQ(h.GetDepthOrdered().Count(), 6u);

	uint32 covered = 0u;
	uint32 previousEnd = 0u;

	for (uint16 depth = 1u; depth <= h.GetMaxDepth(); ++depth)
	{
		uint32 start = 0u, end = 0u;
		h.GetLevelRange(depth, start, end);

		EXPECT_EQ(start, previousEnd) << "level ranges must be contiguous";
		previousEnd = end;
		covered += end - start;

		for (uint32 i = start; i < end; ++i)
		{
			EXPECT_EQ(h.GetDepth(h.GetDepthOrdered()[i]), depth)
				<< "entity is outside the range for its own depth";
		}
	}

	EXPECT_EQ(covered, 6u) << "levels cover every entity exactly once";
}

TEST(Hierarchy, DepthOrderRebuildsOnlyWhenTopologyChanges)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.RefreshDepthOrder();

	const uint64 version = h.GetTopologyVersion();
	h.RefreshDepthOrder();
	EXPECT_EQ(h.GetTopologyVersion(), version) << "refreshing alone is not a change";

	h.SetParent(E(2), E(1));
	h.RefreshDepthOrder();
	EXPECT_EQ(h.GetMaxDepth(), 2u) << "index picked up the new level";
}

TEST(Hierarchy, DescendantWalkIsScoped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	h.SetParent(E(1), E(0));
	h.SetParent(E(2), E(0));
	h.SetParent(E(3), E(1));
	h.SetParent(E(4), E(3));

	uint32 fromRoot = 0u;
	h.ForEachDescendant(E(0), [&](EntityId) { ++fromRoot; });
	EXPECT_EQ(fromRoot, 4u);

	uint32 fromMid = 0u;
	h.ForEachDescendant(E(1), [&](EntityId) { ++fromMid; });
	EXPECT_EQ(fromMid, 2u);

	uint32 fromLeaf = 0u;
	h.ForEachDescendant(E(4), [&](EntityId) { ++fromLeaf; });
	EXPECT_EQ(fromLeaf, 0u);
}

TEST(Hierarchy, RandomizedReparentingStaysConsistent)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	constexpr uint32 entityCount = 200u;

	uint32 seed = 12345u;
	auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };

	for (uint32 iteration = 0u; iteration < 4000u; ++iteration)
	{
		const uint32 child = 1u + (next() % (entityCount - 1u));
		const uint32 parent = next() % entityCount;

		if ((next() & 7u) == 0u)
		{
			h.DetachFromParent(E(child));
		}
		else
		{
			h.SetParent(E(child), E(parent)); // may legitimately be rejected as a cycle
		}
	}

	for (uint32 i = 0u; i < entityCount; ++i)
	{
		SCOPED_TRACE(testing::Message() << "entity " << i);

		CheckListIntegrity(h, E(i));
		EXPECT_FALSE(h.IsAncestorOf(E(i), E(i))) << "entity became its own ancestor";

		// Stored depth must equal the measured distance to the root.
		uint16 walked = 0u;
		EntityId cursor = h.GetParent(E(i));
		while (cursor != InvalidEntity)
		{
			++walked;
			cursor = h.GetParent(cursor);
		}

		EXPECT_EQ(walked, h.GetDepth(E(i))) << "stored depth drifted from the real distance";
	}

	h.RefreshDepthOrder();
	EXPECT_EQ(h.GetDepthOrdered().Count(), h.GetParentedCount());
}
