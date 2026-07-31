#include <gtest/gtest.h>

#include "TestCommon.h"

#include "ECS/CoreComponents.h"
#include "ECS/World.h"

using namespace frt;


// ---------------------------------------------------------------------------------
// Entity lifetime
// ---------------------------------------------------------------------------------

TEST(World, SpawnProducesDistinctLiveHandles)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId a = world.Spawn();
	const EntityId b = world.Spawn();

	EXPECT_TRUE(world.IsAlive(a));
	EXPECT_TRUE(world.IsAlive(b));
	EXPECT_NE(a, b);
	EXPECT_EQ(world.GetAliveCount(), 2u);
	EXPECT_FALSE(world.IsAlive(InvalidEntity));
}

TEST(World, DestroyInvalidatesOnlyThatHandle)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId a = world.Spawn();
	const EntityId b = world.Spawn();

	world.Destroy(a);

	EXPECT_FALSE(world.IsAlive(a));
	EXPECT_TRUE(world.IsAlive(b));
	EXPECT_EQ(world.GetAliveCount(), 1u);
}

TEST(World, DoubleDestroyIsNoOp)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId a = world.Spawn();
	world.Destroy(a);
	world.Destroy(a);

	// A second push onto the free list would hand the same index out twice.
	EXPECT_EQ(world.GetAliveCount(), 0u);

	const EntityId first = world.Spawn();
	const EntityId second = world.Spawn();
	EXPECT_NE(first.GetIndex(), second.GetIndex());
}

TEST(World, RecycledIndexDoesNotValidateAsTheOldHandle)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId a = world.Spawn();
	world.Destroy(a);

	const EntityId recycled = world.Spawn();

	EXPECT_EQ(recycled.GetIndex(), a.GetIndex()) << "index should be reused";
	EXPECT_NE(recycled, a) << "generation must differ";
	EXPECT_TRUE(world.IsAlive(recycled));
	EXPECT_FALSE(world.IsAlive(a)) << "stale handle must stay dead";
}


// ---------------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------------

TEST(World, AddGetHasRoundTrip)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId e = world.Spawn();

	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
	EXPECT_EQ(world.TryGet<Comp_LocalTransform>(e), nullptr);

	world.Add<Comp_LocalTransform>(e).SetTranslation(Vector3r(1, 2, 3));

	EXPECT_TRUE(world.Has<Comp_LocalTransform>(e));
	EXPECT_EQ(world.Get<Comp_LocalTransform>(e).Translation.x, Real(1));
	EXPECT_NE(world.TryGet<Comp_LocalTransform>(e), nullptr);
}

TEST(World, DestroyStripsEveryComponent)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId e = world.Spawn();
	world.Add<Comp_LocalTransform>(e);
	world.Add<Comp_WorldTransform>(e);

	world.Destroy(e);

	// Destroy walks all pools, not just the ones the caller happens to remember.
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
	EXPECT_FALSE(world.Has<Comp_WorldTransform>(e));
}

TEST(World, RecycledEntityDoesNotInheritComponents)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId e = world.Spawn();
	world.Add<Comp_LocalTransform>(e).SetTranslation(Vector3r(7, 7, 7));
	world.Destroy(e);

	const EntityId reused = world.Spawn();

	ASSERT_EQ(reused.GetIndex(), e.GetIndex());
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(reused)) << "recycled slot must start clean";
}

TEST(World, RemoveOfAbsentComponentIsNoOp)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId e = world.Spawn();
	world.Remove<Comp_LocalTransform>(e);

	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
}


// ---------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------

TEST(WorldView, VisitsExactlyTheIntersection)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	uint32 expected = 0u;
	for (uint32 i = 0u; i < 40u; ++i)
	{
		const EntityId e = world.Spawn();
		world.Add<Comp_LocalTransform>(e).SetTranslation(Vector3r(static_cast<Real>(i), 0, 0));

		if (i % 2u == 0u)
		{
			world.Add<Comp_WorldTransform>(e);
			++expected;
		}
	}

	uint32 visited = 0u;
	for (auto [id, local, worldXf] : world.View<const Comp_LocalTransform, Comp_WorldTransform>())
	{
		EXPECT_TRUE(world.Has<Comp_WorldTransform>(id));
		worldXf.Translation = math::VectorCast<Real>(local.Translation);
		++visited;
	}

	EXPECT_EQ(visited, expected);
	EXPECT_EQ(visited, 20u);
}

TEST(WorldView, SingleComponentViewSeesWholePool)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	for (uint32 i = 0u; i < 40u; ++i)
	{
		world.Add<Comp_LocalTransform>(world.Spawn());
	}

	uint32 visited = 0u;
	for (auto [id, local] : world.View<const Comp_LocalTransform>())
	{
		(void)id; (void)local;
		++visited;
	}

	EXPECT_EQ(visited, 40u);
}

TEST(WorldView, EmptyWorldYieldsNothing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	uint32 visited = 0u;
	for (auto [id, local] : world.View<const Comp_LocalTransform>())
	{
		(void)id; (void)local;
		++visited;
	}

	EXPECT_EQ(visited, 0u);
}

TEST(WorldView, ExcludeSkipsTaggedEntities)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	for (uint32 i = 0u; i < 30u; ++i)
	{
		const EntityId e = world.Spawn();
		world.Add<Comp_LocalTransform>(e);

		// Comp_Children stands in for a tag component here.
		if (i % 3u == 0u)
		{
			world.Add<Comp_Children>(e);
		}
	}

	uint32 visited = 0u;
	auto view = world.View<const Comp_LocalTransform>();
	for (auto [id, local] : view.Exclude<Comp_Children>())
	{
		EXPECT_FALSE(world.Has<Comp_Children>(id));
		(void)local;
		++visited;
	}

	EXPECT_EQ(visited, 20u);
}

TEST(WorldView, ExcludeIsSafeWhenChainedOntoATemporary)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	for (uint32 i = 0u; i < 30u; ++i)
	{
		const EntityId e = world.Spawn();
		world.Add<Comp_LocalTransform>(e);

		if (i % 3u == 0u)
		{
			world.Add<Comp_Children>(e);
		}
	}

	// The spelling that used to dangle: with Exclude returning a reference, the
	// View<> temporary died at the end of the full expression and iteration read
	// freed memory. It only shows up as a wrong count, not a crash.
	uint32 visited = 0u;
	for (auto [id, local] : world.View<const Comp_LocalTransform>().Exclude<Comp_Children>())
	{
		EXPECT_FALSE(world.Has<Comp_Children>(id));
		(void)local;
		++visited;
	}

	EXPECT_EQ(visited, 20u);
}

TEST(WorldView, ConstViewDoesNotDirtyThePool)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	for (uint32 i = 0u; i < 10u; ++i)
	{
		world.Add<Comp_LocalTransform>(world.Spawn());
	}

	const uint64 mark = world.Pool<Comp_LocalTransform>().GetVersion();

	for (auto [id, local] : world.View<const Comp_LocalTransform>())
	{
		(void)id; (void)local;
	}

	// This is load-bearing: it is what stops a read-only system from marking every
	// entity it inspects as changed.
	EXPECT_EQ(world.Pool<Comp_LocalTransform>().GetVersion(), mark);
}

TEST(WorldView, MutableViewDirtiesThePool)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	for (uint32 i = 0u; i < 10u; ++i)
	{
		world.Add<Comp_LocalTransform>(world.Spawn());
	}

	const uint64 mark = world.Pool<Comp_LocalTransform>().GetVersion();

	for (auto [id, local] : world.View<Comp_LocalTransform>())
	{
		(void)id;
		local.SetTranslation(Vector3r(1, 1, 1));
	}

	EXPECT_GT(world.Pool<Comp_LocalTransform>().GetVersion(), mark);
}


// ---------------------------------------------------------------------------------
// Hierarchy integration
// ---------------------------------------------------------------------------------

TEST(World, DestroyRunsHierarchyFixups)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CHierarchy& h = world.GetHierarchy();

	const EntityId root  = world.Spawn();
	const EntityId child = world.Spawn();
	const EntityId grand = world.Spawn();

	ASSERT_TRUE(h.SetParent(child, root));
	ASSERT_TRUE(h.SetParent(grand, child));
	ASSERT_EQ(h.GetDepth(grand), 2u);

	world.Destroy(child);

	EXPECT_FALSE(world.IsAlive(child));
	EXPECT_EQ(h.GetChildCount(root), 0u) << "root's child list repaired";
	EXPECT_FALSE(h.HasParent(grand))     << "grandchild orphaned";
	EXPECT_EQ(h.GetDepth(grand), 0u)     << "grandchild rebased to a root";
}
