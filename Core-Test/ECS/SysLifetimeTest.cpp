#include <gtest/gtest.h>

#include "TestCommon.h"

#include "ECS/CommandBuffer.h"
#include "ECS/CoreComponents.h"
#include "ECS/Sys_Lifetime.h"
#include "ECS/World.h"

using namespace frt;

namespace
{
/** One frame: run the system, then apply whatever it queued. */
void Step (Sys_Lifetime& InSystem, CCommandBuffer& InCommands, CWorld& InWorld, float InDeltaSeconds)
{
	SUpdateContext context;
	context.DeltaSeconds = InDeltaSeconds;

	InSystem.Update(context);
	InCommands.Flush(InWorld);
}

EntityId SpawnWithLifetime (CWorld& InWorld, Real InSeconds)
{
	const EntityId e = InWorld.Spawn();
	InWorld.Add<Comp_Lifetime>(e).Remaining = InSeconds;
	return e;
}
}


TEST(SysLifetime, CountsDownWithoutExpiring)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId e = SpawnWithLifetime(world, Real(1));

	Step(system, commands, world, 0.25f);

	EXPECT_TRUE(world.IsAlive(e));
	EXPECT_EQ(system.GetLastExpiredCount(), 0u);
	EXPECT_NEAR(static_cast<double>(world.Get<Comp_Lifetime>(e).Remaining), 0.75, 1e-5);
}

TEST(SysLifetime, DestroysOnReachingZero)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId e = SpawnWithLifetime(world, Real(1));

	Step(system, commands, world, 1.0f);

	EXPECT_FALSE(world.IsAlive(e)) << "exactly zero must expire, not just below zero";
	EXPECT_EQ(system.GetLastExpiredCount(), 1u);
}

TEST(SysLifetime, OvershootStillDestroys)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId e = SpawnWithLifetime(world, Real(0.1));

	// A long frame must not let an entity survive past its lifetime.
	Step(system, commands, world, 5.0f);

	EXPECT_FALSE(world.IsAlive(e));
}

TEST(SysLifetime, DestroysAcrossSeveralFrames)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId e = SpawnWithLifetime(world, Real(1));

	for (uint32 frame = 0u; frame < 3u; ++frame)
	{
		Step(system, commands, world, 0.3f);
		EXPECT_TRUE(world.IsAlive(e)) << "still alive at frame " << frame;
	}

	Step(system, commands, world, 0.3f);
	EXPECT_FALSE(world.IsAlive(e));
}

TEST(SysLifetime, ExpiresOnlyTheEntitiesThatRanOut)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < 100u; ++i)
	{
		// Half expire this step, half do not.
		entities.Add(SpawnWithLifetime(world, i % 2u == 0u ? Real(0.5) : Real(10)));
	}

	Step(system, commands, world, 1.0f);

	EXPECT_EQ(system.GetLastExpiredCount(), 50u);
	EXPECT_EQ(world.GetAliveCount(), 50u);

	for (uint32 i = 0u; i < entities.Count(); ++i)
	{
		EXPECT_EQ(world.IsAlive(entities[i]), i % 2u != 0u) << "entity " << i;
	}
}

TEST(SysLifetime, IterationSurvivesMassExpiry)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	// Every entity expires at once. Destroying in place would swap-and-pop the very pool
	// the countdown is walking, so this is the case the command buffer exists for.
	for (uint32 i = 0u; i < 500u; ++i)
	{
		SpawnWithLifetime(world, Real(0.1));
	}

	Step(system, commands, world, 1.0f);

	EXPECT_EQ(system.GetLastExpiredCount(), 500u) << "every entity must be visited exactly once";
	EXPECT_EQ(world.GetAliveCount(), 0u);
	EXPECT_EQ(world.Pool<Comp_Lifetime>().Count(), 0u);
}

TEST(SysLifetime, DestroysTheEntityNotJustTheComponent)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId e = SpawnWithLifetime(world, Real(0.5));
	world.Add<Comp_LocalTransform>(e);
	world.Add<Comp_Name>(e, "temporary");

	Step(system, commands, world, 1.0f);

	EXPECT_FALSE(world.IsAlive(e));
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
	EXPECT_FALSE(world.Has<Comp_Name>(e));
}

TEST(SysLifetime, EntitiesWithoutTheComponentAreUntouched)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId permanent = world.Spawn();
	world.Add<Comp_LocalTransform>(permanent);

	const EntityId temporary = SpawnWithLifetime(world, Real(0.5));

	Step(system, commands, world, 1.0f);

	EXPECT_TRUE(world.IsAlive(permanent));
	EXPECT_FALSE(world.IsAlive(temporary));
}

TEST(SysLifetime, ExpiryRunsHierarchyFixups)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);
	CHierarchy& hierarchy = world.GetHierarchy();

	const EntityId root  = world.Spawn();
	const EntityId child = SpawnWithLifetime(world, Real(0.5));
	const EntityId grand = world.Spawn();

	ASSERT_TRUE(hierarchy.SetParent(child, root));
	ASSERT_TRUE(hierarchy.SetParent(grand, child));

	Step(system, commands, world, 1.0f);

	EXPECT_FALSE(world.IsAlive(child));
	EXPECT_EQ(hierarchy.GetChildCount(root), 0u);
	EXPECT_FALSE(hierarchy.HasParent(grand)) << "grandchild orphaned, not destroyed";
	EXPECT_TRUE(world.IsAlive(grand));
}

TEST(SysLifetime, RunsInTheUpdatePhase)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	// Update, so the flush that follows settles the entity set before Finalize systems
	// like Sys_Transform run.
	EXPECT_TRUE(system.GetPhases() && EUpdatePhase::Update);
	EXPECT_FALSE(system.GetPhases() && EUpdatePhase::Finalize);
}

TEST(SysLifetime, EmptyWorldIsHarmless)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	Step(system, commands, world, 1.0f);

	EXPECT_EQ(system.GetLastExpiredCount(), 0u);
}

TEST(SysLifetime, ZeroDeltaDoesNotExpireALivingEntity)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	Sys_Lifetime system(world, commands);

	const EntityId e = SpawnWithLifetime(world, Real(1));

	// A paused frame must not consume lifetime.
	Step(system, commands, world, 0.0f);

	EXPECT_TRUE(world.IsAlive(e));
	EXPECT_NEAR(static_cast<double>(world.Get<Comp_Lifetime>(e).Remaining), 1.0, 1e-5);
}
