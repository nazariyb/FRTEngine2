#include <gtest/gtest.h>

#include "TestCommon.h"

#include "ECS/CoreComponents.h"
#include "ECS/Sys_Transform.h"
#include "ECS/World.h"

using namespace frt;

namespace
{
constexpr double Tolerance = 1e-5;

/** Spawns an entity carrying both transform components, positioned at InTranslation. */
EntityId SpawnTransformed (CWorld& InWorld, const Vector3r& InTranslation)
{
	const EntityId e = InWorld.Spawn();
	InWorld.Add<Comp_LocalTransform>(e).SetTranslation(InTranslation);
	InWorld.Add<Comp_WorldTransform>(e);
	return e;
}

void ExpectTranslation (const CWorld& InWorld, EntityId InEntity, Real InX, Real InY, Real InZ)
{
	const Comp_WorldTransform& xf = InWorld.Get<Comp_WorldTransform>(InEntity);

	EXPECT_NEAR(xf.Translation.x, InX, Tolerance);
	EXPECT_NEAR(xf.Translation.y, InY, Tolerance);
	EXPECT_NEAR(xf.Translation.z, InZ, Tolerance);
}
}


// ---------------------------------------------------------------------------------
// Roots
// ---------------------------------------------------------------------------------

TEST(SysTransform, RootWorldTransformMirrorsLocal)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	const EntityId e = SpawnTransformed(world, Vector3r(1, 2, 3));

	system.Run();

	ExpectTranslation(world, e, Real(1), Real(2), Real(3));
}

TEST(SysTransform, EntityWithoutWorldTransformIsSkipped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	const EntityId withBoth = SpawnTransformed(world, Vector3r(1, 0, 0));

	const EntityId localOnly = world.Spawn();
	world.Add<Comp_LocalTransform>(localOnly).SetTranslation(Vector3r(9, 0, 0));

	system.Run();

	EXPECT_EQ(system.GetLastUpdatedCount(), 1u);
	EXPECT_FALSE(world.Has<Comp_WorldTransform>(localOnly));
	ExpectTranslation(world, withBoth, Real(1), Real(0), Real(0));
}

TEST(SysTransform, EmptyWorldIsHarmless)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	system.Run();

	EXPECT_EQ(system.GetLastUpdatedCount(), 0u);
}


// ---------------------------------------------------------------------------------
// Hierarchy composition
// ---------------------------------------------------------------------------------

TEST(SysTransform, ChildInheritsParentTranslation)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	const EntityId parent = SpawnTransformed(world, Vector3r(10, 0, 0));
	const EntityId child = SpawnTransformed(world, Vector3r(0, 5, 0));

	ASSERT_TRUE(world.GetHierarchy().SetParent(child, parent));

	system.Run();

	ExpectTranslation(world, parent, Real(10), Real(0), Real(0));
	ExpectTranslation(world, child, Real(10), Real(5), Real(0));
}

TEST(SysTransform, ParentRotationAppliesToChildOffset)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	const EntityId parent = SpawnTransformed(world, Vector3r(10, 0, 0));
	world.Get<Comp_LocalTransform>(parent).SetRotation(
		Vector3r(0, static_cast<Real>(math::PI_OVER_TWO), 0));

	const EntityId child = SpawnTransformed(world, Vector3r(0, 0, 5));

	ASSERT_TRUE(world.GetHierarchy().SetParent(child, parent));

	system.Run();

	// A 90 degree yaw maps +Z onto +X, so the child lands at 10 + 5 on X.
	ExpectTranslation(world, child, Real(15), Real(0), Real(0));
}

TEST(SysTransform, ThreeLevelChainAccumulates)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	const EntityId a = SpawnTransformed(world, Vector3r(1, 0, 0));
	const EntityId b = SpawnTransformed(world, Vector3r(2, 0, 0));
	const EntityId c = SpawnTransformed(world, Vector3r(4, 0, 0));

	ASSERT_TRUE(h.SetParent(b, a));
	ASSERT_TRUE(h.SetParent(c, b));

	system.Run();

	ExpectTranslation(world, a, Real(1), Real(0), Real(0));
	ExpectTranslation(world, b, Real(3), Real(0), Real(0));
	ExpectTranslation(world, c, Real(7), Real(0), Real(0));
}

TEST(SysTransform, ParentScaleScalesChildOffset)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	const EntityId parent = SpawnTransformed(world, Vector3r(0, 0, 0));
	world.Get<Comp_LocalTransform>(parent).SetScale(Real(2));

	const EntityId child = SpawnTransformed(world, Vector3r(3, 0, 0));
	ASSERT_TRUE(world.GetHierarchy().SetParent(child, parent));

	system.Run();

	ExpectTranslation(world, child, Real(6), Real(0), Real(0));
}

/**
 * The ordering guarantee, tested where it actually bites: children are created BEFORE
 * their parents, so pool insertion order is the reverse of the required evaluation order.
 * Only the depth-ordered walk gets this right.
 */
TEST(SysTransform, ChildrenCreatedBeforeParentsStillResolveCorrectly)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	const EntityId leaf = SpawnTransformed(world, Vector3r(1, 0, 0));
	const EntityId mid  = SpawnTransformed(world, Vector3r(10, 0, 0));
	const EntityId root = SpawnTransformed(world, Vector3r(100, 0, 0));

	ASSERT_TRUE(h.SetParent(leaf, mid));
	ASSERT_TRUE(h.SetParent(mid, root));

	system.Run();

	ExpectTranslation(world, root, Real(100), Real(0), Real(0));
	ExpectTranslation(world, mid,  Real(110), Real(0), Real(0));
	ExpectTranslation(world, leaf, Real(111), Real(0), Real(0));
}

TEST(SysTransform, ParentWithoutWorldTransformTreatsChildAsRoot)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	// A parent carrying no transform of its own cannot contribute one.
	const EntityId parent = world.Spawn();
	const EntityId child = SpawnTransformed(world, Vector3r(5, 0, 0));

	ASSERT_TRUE(world.GetHierarchy().SetParent(child, parent));

	system.Run();

	ExpectTranslation(world, child, Real(5), Real(0), Real(0));
}


// ---------------------------------------------------------------------------------
// Reacting to change
// ---------------------------------------------------------------------------------

TEST(SysTransform, MovingAParentMovesItsWholeSubtree)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	const EntityId root = SpawnTransformed(world, Vector3r(0, 0, 0));
	const EntityId mid = SpawnTransformed(world, Vector3r(1, 0, 0));
	const EntityId leaf = SpawnTransformed(world, Vector3r(1, 0, 0));

	ASSERT_TRUE(h.SetParent(mid, root));
	ASSERT_TRUE(h.SetParent(leaf, mid));

	system.Run();
	ExpectTranslation(world, leaf, Real(2), Real(0), Real(0));

	world.Get<Comp_LocalTransform>(root).SetTranslation(Vector3r(100, 0, 0));
	system.Run();

	ExpectTranslation(world, root, Real(100), Real(0), Real(0));
	ExpectTranslation(world, mid,  Real(101), Real(0), Real(0));
	ExpectTranslation(world, leaf, Real(102), Real(0), Real(0));
}

TEST(SysTransform, ReparentingRecomputesWithoutTouchingLocalTransforms)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	const EntityId parentA = SpawnTransformed(world, Vector3r(10, 0, 0));
	const EntityId parentB = SpawnTransformed(world, Vector3r(50, 0, 0));
	const EntityId child = SpawnTransformed(world, Vector3r(1, 0, 0));

	ASSERT_TRUE(h.SetParent(child, parentA));
	system.Run();
	ExpectTranslation(world, child, Real(11), Real(0), Real(0));

	// Only the topology changed - no local transform was written. If the system keyed
	// solely off local-transform dirtiness it would leave the child stale here.
	ASSERT_TRUE(h.SetParent(child, parentB));
	system.Run();

	ExpectTranslation(world, child, Real(51), Real(0), Real(0));
}

TEST(SysTransform, DetachingToRootDropsTheParentContribution)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	const EntityId parent = SpawnTransformed(world, Vector3r(10, 0, 0));
	const EntityId child = SpawnTransformed(world, Vector3r(1, 0, 0));

	ASSERT_TRUE(h.SetParent(child, parent));
	system.Run();
	ExpectTranslation(world, child, Real(11), Real(0), Real(0));

	h.DetachFromParent(child);
	system.Run();

	ExpectTranslation(world, child, Real(1), Real(0), Real(0));
}

TEST(SysTransform, OrphanedGrandchildRebasesOnDestroy)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	const EntityId root = SpawnTransformed(world, Vector3r(10, 0, 0));
	const EntityId mid = SpawnTransformed(world, Vector3r(20, 0, 0));
	const EntityId leaf = SpawnTransformed(world, Vector3r(1, 0, 0));

	ASSERT_TRUE(h.SetParent(mid, root));
	ASSERT_TRUE(h.SetParent(leaf, mid));

	system.Run();
	ExpectTranslation(world, leaf, Real(31), Real(0), Real(0));

	// Destroy orphans rather than cascading, so the leaf survives as a root.
	world.Destroy(mid);
	system.Run();

	ExpectTranslation(world, leaf, Real(1), Real(0), Real(0));
}


// ---------------------------------------------------------------------------------
// Change skipping
// ---------------------------------------------------------------------------------

TEST(SysTransformSkipping, FirstRunUpdatesEverything)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	for (uint32 i = 0u; i < 10u; ++i)
	{
		SpawnTransformed(world, Vector3r(static_cast<Real>(i), 0, 0));
	}

	system.Run();

	EXPECT_EQ(system.GetLastUpdatedCount(), 10u);
}

TEST(SysTransformSkipping, SecondRunWithNoChangesDoesNothing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	for (uint32 i = 0u; i < 10u; ++i)
	{
		SpawnTransformed(world, Vector3r(static_cast<Real>(i), 0, 0));
	}

	system.Run();
	ASSERT_EQ(system.GetLastUpdatedCount(), 10u);

	system.Run();
	EXPECT_EQ(system.GetLastUpdatedCount(), 0u);
}

TEST(SysTransformSkipping, TouchingOneEntityDoesNotRecomputeEverything)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	constexpr uint32 entityCount = 512u;
	constexpr uint32 blockSize = TComponentPool<Comp_LocalTransform>::DirtyBlockSize;

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < entityCount; ++i)
	{
		entities.Add(SpawnTransformed(world, Vector3r(static_cast<Real>(i), 0, 0)));
	}

	system.Run();
	ASSERT_EQ(system.GetLastUpdatedCount(), entityCount);

	world.Get<Comp_LocalTransform>(entities[0]).SetTranslation(Vector3r(999, 0, 0));
	system.Run();

	// Dirtiness is tracked per block, so this is deliberately a bound rather than an
	// exact count - the skip may do redundant work, but must never do all of it.
	EXPECT_GE(system.GetLastUpdatedCount(), 1u);
	EXPECT_LE(system.GetLastUpdatedCount(), blockSize);

	ExpectTranslation(world, entities[0], Real(999), Real(0), Real(0));
}

TEST(SysTransformSkipping, InvalidateForcesAFullRecompute)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	for (uint32 i = 0u; i < 10u; ++i)
	{
		SpawnTransformed(world, Vector3r(static_cast<Real>(i), 0, 0));
	}

	system.Run();
	system.Run();
	ASSERT_EQ(system.GetLastUpdatedCount(), 0u);

	system.Invalidate();
	system.Run();

	EXPECT_EQ(system.GetLastUpdatedCount(), 10u);
}

TEST(SysTransformSkipping, TopologyChangeForcesAFullRecompute)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < 10u; ++i)
	{
		entities.Add(SpawnTransformed(world, Vector3r(static_cast<Real>(i), 0, 0)));
	}

	system.Run();
	system.Run();
	ASSERT_EQ(system.GetLastUpdatedCount(), 0u);

	ASSERT_TRUE(world.GetHierarchy().SetParent(entities[1], entities[0]));
	system.Run();

	EXPECT_EQ(system.GetLastUpdatedCount(), 10u);
}

TEST(SysTransformSkipping, ReadOnlyRunLeavesWorldPoolVersionAlone)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);

	for (uint32 i = 0u; i < 10u; ++i)
	{
		SpawnTransformed(world, Vector3r(static_cast<Real>(i), 0, 0));
	}

	system.Run();

	const uint64 mark = world.Pool<Comp_WorldTransform>().GetVersion();
	system.Run();

	// Downstream systems key off this version to decide what to re-upload. A pass that
	// wrote nothing must leave it untouched, or every frame looks fully dirty.
	ASSERT_EQ(system.GetLastUpdatedCount(), 0u);
	EXPECT_EQ(world.Pool<Comp_WorldTransform>().GetVersion(), mark);
}


// ---------------------------------------------------------------------------------
// Cross-check against a brute-force reference
// ---------------------------------------------------------------------------------

/**
 * Builds a randomized forest, runs the system, then recomputes every world transform the
 * naive way - walk each entity's ancestor chain to a root and compose downwards - and
 * compares. This is what catches an ordering bug that the small hand-built cases miss.
 */
TEST(SysTransform, MatchesBruteForceOnRandomForest)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform system(world);
	CHierarchy& h = world.GetHierarchy();

	constexpr uint32 entityCount = 300u;

	uint32 seed = 987654321u;
	auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < entityCount; ++i)
	{
		const EntityId e = SpawnTransformed(world,
			Vector3r(static_cast<Real>(next() % 20u), static_cast<Real>(next() % 20u), static_cast<Real>(next() % 20u)));

		world.Get<Comp_LocalTransform>(e).SetRotation(
			Vector3r(static_cast<Real>((next() % 100u)) / Real(50), Real(0), Real(0)));

		entities.Add(e);
	}

	// Parent each entity to an earlier one at random, leaving some as roots. Attaching
	// only backwards keeps it acyclic by construction.
	for (uint32 i = 1u; i < entityCount; ++i)
	{
		if ((next() & 3u) == 0u)
		{
			continue; // stays a root
		}

		h.SetParent(entities[i], entities[next() % i]);
	}

	system.Run();

	for (uint32 i = 0u; i < entityCount; ++i)
	{
		SCOPED_TRACE(testing::Message() << "entity index " << i);

		// Collect the ancestor chain, root-most last.
		TArray<EntityId> chain;
		EntityId cursor = entities[i];
		while (cursor != InvalidEntity)
		{
			chain.Add(cursor);
			cursor = h.GetParent(cursor);
		}

		// Compose downwards from the root.
		Comp_WorldTransform expected =
			Comp_WorldTransform::FromLocal(world.Get<Comp_LocalTransform>(chain.Last()));

		for (int64 c = static_cast<int64>(chain.Count()) - 2; c >= 0; --c)
		{
			expected = Comp_WorldTransform::Compose(
				expected, world.Get<Comp_LocalTransform>(chain[c]));
		}

		const Comp_WorldTransform& actual = world.Get<Comp_WorldTransform>(entities[i]);

		EXPECT_NEAR(actual.Translation.x, expected.Translation.x, Tolerance);
		EXPECT_NEAR(actual.Translation.y, expected.Translation.y, Tolerance);
		EXPECT_NEAR(actual.Translation.z, expected.Translation.z, Tolerance);

		for (uint32 row = 0u; row < 3u; ++row)
		{
			for (uint32 col = 0u; col < 3u; ++col)
			{
				EXPECT_NEAR(actual.Basis.M[row][col], expected.Basis.M[row][col], Tolerance)
					<< "basis element [" << row << "][" << col << "]";
			}
		}
	}
}
