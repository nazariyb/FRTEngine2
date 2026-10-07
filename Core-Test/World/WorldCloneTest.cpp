#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Entity.h"
#include "WorldScene.h"
#include "ECS/Comp_Name.h"
#include "ECS/Hierarchy.h"
#include "ECS/World.h"
#include "Graphics/Comp_Light.h"
#include "Graphics/Model.h"
#include "Math/Comp_LocalTransform.h"
#include "Math/Comp_WorldTransform.h"

using namespace frt;

// Play mode in a nutshell: the edit world is duplicated, the copy runs, the copy is dropped.
// (Component ids are registered by TestEnvironment.cpp.)

TEST(WorldClone, CopiesEcsEntitiesWithTheSameHandles)
{
	FRT_TEST_MEMORY_POOL();
	CWorldScene source;
	ASSERT_TRUE(source.Initialize());
	CWorld& ecs = source.GetEcsWorld();
	const EntityId parent = ecs.Spawn();
	const EntityId child = ecs.Spawn();
	ecs.Add<Comp_Name>(parent, "Parent");
	ecs.Add<Comp_LocalTransform>(parent, Vector3r(1.0f, 2.0f, 3.0f));
	ecs.Add<Comp_WorldTransform>(parent);
	ecs.Add<Comp_LocalTransform>(child, Vector3r(0.0f, 1.0f, 0.0f));
	ecs.Add<Comp_WorldTransform>(child);
	ecs.GetHierarchy().SetParent(child, parent);
	const EntityId destroyed = ecs.Spawn();
	ecs.Destroy(destroyed);

	const std::unique_ptr<CWorldScene> copy = source.Clone();
	CWorld& copyEcs = copy->GetEcsWorld();

	EXPECT_EQ(copyEcs.GetAliveCount(), ecs.GetAliveCount());
	EXPECT_TRUE(copyEcs.IsAlive(parent));
	EXPECT_TRUE(copyEcs.IsAlive(child));
	EXPECT_FALSE(copyEcs.IsAlive(destroyed));
	ASSERT_TRUE(copyEcs.Has<Comp_Name>(parent));
	EXPECT_STREQ(copyEcs.Get<Comp_Name>(parent).Get(), "Parent");
	EXPECT_EQ(copyEcs.GetHierarchy().GetParent(child), parent);

	// A spawn in the copy recycles the same index the source would: identical free lists.
	EXPECT_EQ(copyEcs.Spawn().GetIndex(), ecs.Spawn().GetIndex());
}

TEST(WorldClone, CopyIsIndependentOfTheSource)
{
	FRT_TEST_MEMORY_POOL();
	CWorldScene source;
	ASSERT_TRUE(source.Initialize());
	CWorld& ecs = source.GetEcsWorld();
	const EntityId entity = ecs.Spawn();
	ecs.Add<Comp_LocalTransform>(entity, Vector3r(1.0f, 0.0f, 0.0f));
	source.Camera.Transform.SetTranslation(Vector3f(1.0f, 2.0f, 3.0f));

	const std::unique_ptr<CWorldScene> copy = source.Clone();
	copy->GetEcsWorld().Get<Comp_LocalTransform>(entity).Translation = Vector3r(5.0f, 0.0f, 0.0f);
	copy->GetEcsWorld().Destroy(entity);
	copy->Camera.Transform.SetTranslation(Vector3f(9.0f, 9.0f, 9.0f));

	EXPECT_TRUE(ecs.IsAlive(entity));
	EXPECT_EQ(ecs.Get<Comp_LocalTransform>(entity).Translation.x, 1.0f);
	EXPECT_EQ(source.Camera.Transform.GetTranslation().x, 1.0f);
}

TEST(WorldClone, LegacyEntitiesAreDeepCopiesSharingTheirModels)
{
	FRT_TEST_MEMORY_POOL();
	CWorldScene source;
	ASSERT_TRUE(source.Initialize());
	memory::TRefShared<CEntity> lamp = source.SpawnEntity("Lamp");
	lamp->Transform.SetTranslation(1.0f, 2.0f, 3.0f);
	lamp->RotationSpeed = Vector3f(0.0f, 1.0f, 0.0f);
	lamp->RenderModel->Model = memory::NewShared<graphics::SRenderModel>();
	lamp->Light = memory::NewShared<graphics::Comp_Light>();
	lamp->Light->Intensity = 4.0f;

	const std::unique_ptr<CWorldScene> copy = source.Clone();
	ASSERT_EQ(copy->GetEntities().Count(), 1u);
	CEntity& copied = *copy->GetEntities()[0];

	EXPECT_NE(&copied, lamp.GetRawIgnoringLifetime());
	EXPECT_EQ(copied.Id, lamp->Id);
	EXPECT_EQ(copied.Name, "Lamp");
	EXPECT_EQ(copied.Transform.GetTranslation().y, 2.0f);
	// Immutable model data is shared; per-world components are not.
	EXPECT_EQ(copied.RenderModel->Model.GetRawIgnoringLifetime(), lamp->RenderModel->Model.GetRawIgnoringLifetime());
	EXPECT_NE(copied.RenderModel.GetRawIgnoringLifetime(), lamp->RenderModel.GetRawIgnoringLifetime());
	ASSERT_TRUE(copied.Light);
	copied.Light->Intensity = 8.0f;
	EXPECT_EQ(lamp->Light->Intensity, 4.0f);
	EXPECT_FALSE(copied.Portal);
}

TEST(WorldClone, SimulatingTheCopyLeavesTheSourceUntouched)
{
	FRT_TEST_MEMORY_POOL();
	CWorldScene source;
	ASSERT_TRUE(source.Initialize());
	memory::TRefShared<CEntity> spinner = source.SpawnEntity("Spinner");
	spinner->RotationSpeed = Vector3f(0.0f, 1.0f, 0.0f);
	const float sourceYaw = spinner->Transform.GetRotation().y;

	const std::unique_ptr<CWorldScene> copy = source.Clone();
	for (int i = 0; i < 10; ++i)
	{
		copy->Update(1.0f / 60.0f);
	}
	source.Update(0.0f); // the edit world is synced, never simulated

	EXPECT_NE(copy->GetEntities()[0]->Transform.GetRotation().y, sourceYaw);
	EXPECT_EQ(spinner->Transform.GetRotation().y, sourceYaw);
}
