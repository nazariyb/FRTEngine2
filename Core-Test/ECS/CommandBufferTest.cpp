#include <gtest/gtest.h>

#include <string>

#include "TestCommon.h"

#include "ECS/CommandBuffer.h"
#include "ECS/CoreComponents.h"
#include "ECS/World.h"

using namespace frt;

namespace
{
/** Non-trivial payload, so the buffer's construct/move/destroy path is exercised. */
struct SComp_CommandPayload
{
	std::string Value;
};
}

FRT_DECLARE_COMPONENT_NAMED(SComp_CommandPayload, "Test_CommandPayload");


// ---------------------------------------------------------------------------------
// Deferral
// ---------------------------------------------------------------------------------

TEST(CommandBuffer, RecordsWithoutApplying)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();

	commands.Add<Comp_LocalTransform>(e);
	commands.Destroy(e);

	// Nothing may happen until Flush - that is the entire point.
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
	EXPECT_TRUE(world.IsAlive(e));
	EXPECT_EQ(commands.Count(), 2u);
}

TEST(CommandBuffer, FlushAppliesAndClears)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();
	commands.Add<Comp_LocalTransform>(e);

	commands.Flush(world);

	EXPECT_TRUE(world.Has<Comp_LocalTransform>(e));
	EXPECT_TRUE(commands.IsEmpty());
	EXPECT_EQ(commands.Count(), 0u);
}

TEST(CommandBuffer, FlushingAnEmptyBufferIsHarmless)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	commands.Flush(world);

	EXPECT_TRUE(commands.IsEmpty());
}


// ---------------------------------------------------------------------------------
// Individual operations
// ---------------------------------------------------------------------------------

TEST(CommandBuffer, AddCarriesConstructorArguments)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();
	commands.Add<Comp_Name>(e, "Sponza");

	commands.Flush(world);

	ASSERT_TRUE(world.Has<Comp_Name>(e));
	EXPECT_TRUE(world.Get<Comp_Name>(e) == "Sponza");
}

TEST(CommandBuffer, RemoveIsDeferred)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();
	world.Add<Comp_LocalTransform>(e);

	commands.Remove<Comp_LocalTransform>(e);
	EXPECT_TRUE(world.Has<Comp_LocalTransform>(e));

	commands.Flush(world);
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
}

TEST(CommandBuffer, DestroyStripsComponentsAndInvalidatesTheHandle)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();
	world.Add<Comp_LocalTransform>(e);
	world.Add<Comp_WorldTransform>(e);

	commands.Destroy(e);
	commands.Flush(world);

	EXPECT_FALSE(world.IsAlive(e));
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
	EXPECT_FALSE(world.Has<Comp_WorldTransform>(e));
}


// ---------------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------------

TEST(CommandBuffer, CommandsApplyInRecordedOrder)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();

	commands.Add<Comp_Name>(e, "first");
	commands.Add<Comp_Name>(e, "second");

	commands.Flush(world);

	EXPECT_TRUE(world.Get<Comp_Name>(e) == "second") << "later command must win";
}

TEST(CommandBuffer, AddAfterDestroyIsSkippedRatherThanAsserting)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();

	// A perfectly ordinary pattern: one system destroys, another adds, both queued in the
	// same phase. The Add must be dropped, not fire CWorld::Add's liveness assert.
	commands.Destroy(e);
	commands.Add<Comp_LocalTransform>(e);

	commands.Flush(world);

	EXPECT_FALSE(world.IsAlive(e));
	EXPECT_FALSE(world.Has<Comp_LocalTransform>(e));
}

TEST(CommandBuffer, RemoveAfterDestroyIsSkipped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();
	world.Add<Comp_LocalTransform>(e);

	commands.Destroy(e);
	commands.Remove<Comp_LocalTransform>(e);

	commands.Flush(world);

	EXPECT_FALSE(world.IsAlive(e));
}

TEST(CommandBuffer, DuplicateDestroyIsHarmless)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();

	commands.Destroy(e);
	commands.Destroy(e);
	commands.Flush(world);

	EXPECT_FALSE(world.IsAlive(e));
	EXPECT_EQ(world.GetAliveCount(), 0u) << "the second destroy must not double-free the slot";

	// A double free-list push would hand the same index out twice.
	const EntityId a = world.Spawn();
	const EntityId b = world.Spawn();
	EXPECT_NE(a.GetIndex(), b.GetIndex());
}


// ---------------------------------------------------------------------------------
// Payload storage
// ---------------------------------------------------------------------------------

TEST(CommandBuffer, HoldsNonTriviallyCopyablePayloads)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < 200u; ++i)
	{
		entities.Add(world.Spawn());
	}

	for (uint32 i = 0u; i < entities.Count(); ++i)
	{
		commands.Add<SComp_CommandPayload>(entities[i], std::string("payload-") + std::to_string(i));
	}

	commands.Flush(world);

	for (uint32 i = 0u; i < entities.Count(); ++i)
	{
		ASSERT_TRUE(world.Has<SComp_CommandPayload>(entities[i]));
		EXPECT_EQ(world.Get<SComp_CommandPayload>(entities[i]).Value, "payload-" + std::to_string(i));
	}
}

TEST(CommandBuffer, PayloadsSurviveManyCommandsAcrossChunks)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	// Far more payload than one chunk holds, so the arena has to span several. A flat
	// buffer that reallocated would have relocated these strings by raw copy.
	constexpr uint32 count = 2000u;

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < count; ++i)
	{
		entities.Add(world.Spawn());
		commands.Add<SComp_CommandPayload>(entities.Last(), std::string("v") + std::to_string(i));
	}

	commands.Flush(world);

	EXPECT_EQ(world.Get<SComp_CommandPayload>(entities[0]).Value, "v0");
	EXPECT_EQ(world.Get<SComp_CommandPayload>(entities[count - 1u]).Value, "v" + std::to_string(count - 1u));
}

TEST(CommandBuffer, ClearDiscardsWithoutApplying)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	const EntityId e = world.Spawn();
	commands.Add<SComp_CommandPayload>(e, std::string("discarded"));

	commands.Clear();

	EXPECT_TRUE(commands.IsEmpty());
	EXPECT_FALSE(world.Has<SComp_CommandPayload>(e));

	// Reusable afterwards, and the discarded payload must not have corrupted the arena.
	commands.Add<SComp_CommandPayload>(e, std::string("kept"));
	commands.Flush(world);

	ASSERT_TRUE(world.Has<SComp_CommandPayload>(e));
	EXPECT_EQ(world.Get<SComp_CommandPayload>(e).Value, "kept");
}

TEST(CommandBuffer, ReusableAcrossManyFlushes)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	for (uint32 frame = 0u; frame < 50u; ++frame)
	{
		const EntityId e = world.Spawn();
		commands.Add<SComp_CommandPayload>(e, std::string("frame-") + std::to_string(frame));
		commands.Add<Comp_LocalTransform>(e);
		commands.Flush(world);

		ASSERT_TRUE(world.Has<SComp_CommandPayload>(e));
		EXPECT_EQ(world.Get<SComp_CommandPayload>(e).Value, "frame-" + std::to_string(frame));

		commands.Destroy(e);
		commands.Flush(world);
		EXPECT_FALSE(world.IsAlive(e));
	}
}


// ---------------------------------------------------------------------------------
// The reason it exists
// ---------------------------------------------------------------------------------

TEST(CommandBuffer, StructuralChangeQueuedFromInsideAView)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < 100u; ++i)
	{
		const EntityId e = world.Spawn();
		world.Add<Comp_LocalTransform>(e).SetTranslation(Vector3r(static_cast<Real>(i), 0, 0));
		entities.Add(e);
	}

	// Destroying directly in this loop would swap-and-pop the pool underneath it.
	uint32 visited = 0u;
	for (auto [id, local] : world.View<const Comp_LocalTransform>())
	{
		++visited;

		if (local.Translation.x >= Real(50))
		{
			commands.Destroy(id);
		}
	}

	EXPECT_EQ(visited, 100u) << "iteration must see every entity exactly once";

	commands.Flush(world);

	EXPECT_EQ(world.GetAliveCount(), 50u);
	EXPECT_TRUE(world.IsAlive(entities[0]));
	EXPECT_FALSE(world.IsAlive(entities[99]));
}

TEST(CommandBuffer, DestroyThroughBufferRunsHierarchyFixups)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	CCommandBuffer commands;
	CHierarchy& hierarchy = world.GetHierarchy();

	const EntityId root  = world.Spawn();
	const EntityId child = world.Spawn();
	const EntityId grand = world.Spawn();

	ASSERT_TRUE(hierarchy.SetParent(child, root));
	ASSERT_TRUE(hierarchy.SetParent(grand, child));

	commands.Destroy(child);
	commands.Flush(world);

	// Deferred destruction must be the same operation as the immediate one, not a
	// shortcut that skips the link repair.
	EXPECT_FALSE(world.IsAlive(child));
	EXPECT_EQ(hierarchy.GetChildCount(root), 0u);
	EXPECT_FALSE(hierarchy.HasParent(grand));
	EXPECT_EQ(hierarchy.GetDepth(grand), 0u);
}
