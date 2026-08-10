// Projection of ECS entities into the GPU light and portal lists. These replace
// Sys_MeshRenderer's scans of the CEntity list, so the assertions track that collector's
// semantics rather than what might be nicer - a migration has to be behaviour-preserving
// before it can be an improvement.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include "ECS/CoreComponents.h"
#include "ECS/Sys_Transform.h"
#include "ECS/World.h"
#include "Graphics/GraphicsComponents.h"
#include "Graphics/Model.h"
#include "Graphics/SceneExtraction.h"
#include "Math/Transform.h"

using namespace frt;

namespace
{
constexpr double Tolerance = 1e-5;

EntityId SpawnAt (CWorld& InWorld, const Vector3r& InTranslation)
{
	const EntityId e = InWorld.Spawn();
	InWorld.Add<Comp_LocalTransform>(e).SetTranslation(InTranslation);
	InWorld.Add<Comp_WorldTransform>(e);
	return e;
}
}


// ---------------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------------

TEST(ExtractLights, EmptyWorldAppendsNothing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	EXPECT_EQ(lights.Count(), 0u);
}

TEST(ExtractLights, AppendsRatherThanReplacing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	// The caller owns the array and puts the sun and sky entries in first; those come from
	// sky settings, not from any entity.
	TArray<graphics::SLight> lights;
	lights.Add(graphics::SLight{});
	lights.Add(graphics::SLight{});

	const EntityId e = SpawnAt(world, Vector3r(1, 0, 0));
	world.Add<graphics::Comp_Light>(e);

	transforms.Run();
	graphics::ExtractLights(world, lights);

	EXPECT_EQ(lights.Count(), 3u) << "existing entries must survive";
}

TEST(ExtractLights, PointLightTakesPositionFromTheWorldTransform)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(1, 2, 3));
	graphics::Comp_Light& light = world.Add<graphics::Comp_Light>(e);
	light.Kind = graphics::Comp_Light::EKind::Point;
	light.Intensity = 12.0f;

	transforms.Run();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	ASSERT_EQ(lights.Count(), 1u);
	EXPECT_EQ(lights[0].Type, static_cast<uint32>(graphics::ELightType::Point));
	EXPECT_FLOAT_EQ(lights[0].Intensity, 12.0f);

	// ToDirectXCoordinates negates X.
	EXPECT_NEAR(lights[0].Position.x, -1.0, Tolerance);
	EXPECT_NEAR(lights[0].Position.y, 2.0, Tolerance);
	EXPECT_NEAR(lights[0].Position.z, 3.0, Tolerance);
}

TEST(ExtractLights, DirectionalMapsKindAndDirection)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(0, 0, 0));
	graphics::Comp_Light& light = world.Add<graphics::Comp_Light>(e);
	light.Kind = graphics::Comp_Light::EKind::Directional;
	light.Direction = Vector3f(1.0f, -1.0f, 0.0f);

	transforms.Run();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	ASSERT_EQ(lights.Count(), 1u);
	EXPECT_EQ(lights[0].Type, static_cast<uint32>(graphics::ELightType::Directional));
	EXPECT_NEAR(lights[0].Direction.x, -1.0, Tolerance);
	EXPECT_NEAR(lights[0].Direction.y, -1.0, Tolerance);
}

TEST(ExtractLights, AreaQuadCarriesEdgesAndComputedArea)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(0, 0, 0));
	graphics::Comp_Light& light = world.Add<graphics::Comp_Light>(e);
	light.Kind = graphics::Comp_Light::EKind::AreaQuad;
	light.Edge1 = Vector3f(2.0f, 0.0f, 0.0f);
	light.Edge2 = Vector3f(0.0f, 3.0f, 0.0f);

	transforms.Run();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	ASSERT_EQ(lights.Count(), 1u);
	EXPECT_EQ(lights[0].Type, static_cast<uint32>(graphics::ELightType::AreaQuad));

	// Edges are half-extents, so area is 4 * |Edge1| * |Edge2|.
	EXPECT_NEAR(lights[0].Area, 24.0, Tolerance);
	EXPECT_NEAR(lights[0].Edge1.x, -2.0, Tolerance);
	EXPECT_NEAR(lights[0].Edge2.y, 3.0, Tolerance);
}

TEST(ExtractLights, DisabledLightsAreSkipped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId on = SpawnAt(world, Vector3r(0, 0, 0));
	world.Add<graphics::Comp_Light>(on).bEnabled = true;

	const EntityId off = SpawnAt(world, Vector3r(5, 0, 0));
	world.Add<graphics::Comp_Light>(off).bEnabled = false;

	transforms.Run();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	// Not merely flagged: LightCount drives the shader loop, so a disabled light must not
	// occupy a slot at all.
	EXPECT_EQ(lights.Count(), 1u);
}

TEST(ExtractLights, LightWithoutAWorldTransformIsSkipped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	const EntityId e = world.Spawn();
	world.Add<graphics::Comp_Light>(e);

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	EXPECT_EQ(lights.Count(), 0u);
}

TEST(ExtractLights, InheritsAParentTransform)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId parent = SpawnAt(world, Vector3r(10, 0, 0));
	const EntityId lamp = SpawnAt(world, Vector3r(0, 5, 0));
	world.Add<graphics::Comp_Light>(lamp);

	ASSERT_TRUE(world.GetHierarchy().SetParent(lamp, parent));
	transforms.Run();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	// The capability the CEntity scan structurally could not have.
	ASSERT_EQ(lights.Count(), 1u);
	EXPECT_NEAR(lights[0].Position.x, -10.0, Tolerance);
	EXPECT_NEAR(lights[0].Position.y, 5.0, Tolerance);
}

TEST(ExtractLights, ReflectsMovementOnTheNextCall)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(1, 0, 0));
	world.Add<graphics::Comp_Light>(e);

	transforms.Run();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);
	ASSERT_EQ(lights.Count(), 1u);

	world.Get<Comp_LocalTransform>(e).SetTranslation(Vector3r(7, 0, 0));
	transforms.Run();

	lights.Clear();
	graphics::ExtractLights(world, lights);

	ASSERT_EQ(lights.Count(), 1u);
	EXPECT_NEAR(lights[0].Position.x, -7.0, Tolerance);
}

TEST(ExtractLights, DoesNotDirtyWhatItReads)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(1, 0, 0));
	world.Add<graphics::Comp_Light>(e);
	transforms.Run();

	const uint64 transformMark = world.Pool<Comp_WorldTransform>().GetVersion();
	const uint64 lightMark = world.Pool<graphics::Comp_Light>().GetVersion();

	TArray<graphics::SLight> lights;
	graphics::ExtractLights(world, lights);

	// The views request their components by const reference. A collector that dirtied what
	// it read would make every downstream change-detection check useless.
	EXPECT_EQ(world.Pool<Comp_WorldTransform>().GetVersion(), transformMark);
	EXPECT_EQ(world.Pool<graphics::Comp_Light>().GetVersion(), lightMark);
}


// ---------------------------------------------------------------------------------
// Portals
// ---------------------------------------------------------------------------------

TEST(ExtractPortals, EmptyWorldAppendsNothing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	TArray<graphics::SPortal> portals;
	graphics::ExtractPortals(world, portals);

	EXPECT_EQ(portals.Count(), 0u);
}

TEST(ExtractPortals, RectCarriesCentreNormalAndEdges)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(1, 9, 0));
	graphics::Comp_Portal& portal = world.Add<graphics::Comp_Portal>(e);
	portal.Shape = graphics::Comp_Portal::EShape::Rect;
	portal.Normal = Vector3f(0.0f, 0.0f, 1.0f);
	portal.Edge1 = Vector3f(2.0f, 0.0f, 0.0f);
	portal.Edge2 = Vector3f(0.0f, 1.5f, 0.0f);

	transforms.Run();

	TArray<graphics::SPortal> portals;
	graphics::ExtractPortals(world, portals);

	ASSERT_EQ(portals.Count(), 1u);
	EXPECT_NEAR(portals[0].Center.x, -1.0, Tolerance);
	EXPECT_NEAR(portals[0].Center.y, 9.0, Tolerance);
	EXPECT_NEAR(portals[0].Normal.z, 1.0, Tolerance);
	EXPECT_NEAR(portals[0].Edge1.x, -2.0, Tolerance);
	EXPECT_NEAR(portals[0].Edge2.y, 1.5, Tolerance);
	EXPECT_EQ(portals[0].Flags, 0u) << "rect must not set the ellipse bit";
}

TEST(ExtractPortals, EllipseSetsTheShapeFlag)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(0, 0, 0));
	world.Add<graphics::Comp_Portal>(e).Shape = graphics::Comp_Portal::EShape::Ellipse;

	transforms.Run();

	TArray<graphics::SPortal> portals;
	graphics::ExtractPortals(world, portals);

	ASSERT_EQ(portals.Count(), 1u);
	EXPECT_EQ(portals[0].Flags, graphics::portal_flags::Ellipse);
}

TEST(ExtractPortals, DisabledPortalsAreSkipped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId on = SpawnAt(world, Vector3r(0, 0, 0));
	world.Add<graphics::Comp_Portal>(on).bEnabled = true;

	const EntityId off = SpawnAt(world, Vector3r(5, 0, 0));
	world.Add<graphics::Comp_Portal>(off).bEnabled = false;

	transforms.Run();

	TArray<graphics::SPortal> portals;
	graphics::ExtractPortals(world, portals);

	EXPECT_EQ(portals.Count(), 1u);
}

TEST(ExtractPortals, DoesNotMutateRenderState)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnAt(world, Vector3r(0, 0, 0));
	world.Add<graphics::Comp_Portal>(e);
	transforms.Run();

	const uint64 portalMark = world.Pool<graphics::Comp_Portal>().GetVersion();

	TArray<graphics::SPortal> portals;
	graphics::ExtractPortals(world, portals);

	// Unlike the scan it replaces, which lazily built visualization meshes and toggled
	// render-model visibility from inside the collector.
	EXPECT_EQ(world.Pool<graphics::Comp_Portal>().GetVersion(), portalMark);
}

TEST(ExtractPortals, DestroyedPortalDropsOut)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId a = SpawnAt(world, Vector3r(0, 0, 0));
	const EntityId b = SpawnAt(world, Vector3r(5, 0, 0));
	world.Add<graphics::Comp_Portal>(a);
	world.Add<graphics::Comp_Portal>(b);

	transforms.Run();

	TArray<graphics::SPortal> portals;
	graphics::ExtractPortals(world, portals);
	ASSERT_EQ(portals.Count(), 2u);

	world.Destroy(a);

	portals.Clear();
	graphics::ExtractPortals(world, portals);
	EXPECT_EQ(portals.Count(), 1u);
}


// ---------------------------------------------------------------------------------
// Mesh instances
// ---------------------------------------------------------------------------------

namespace
{
/** A model with InSectionCount sections, enough for the hit-group prefix sums. */
memory::TRefShared<graphics::SRenderModel> MakeModel (uint32 InSectionCount)
{
	auto model = memory::NewShared<graphics::SRenderModel>();
	for (uint32 i = 0u; i < InSectionCount; ++i)
	{
		model->Sections.Add(graphics::SRenderSection{});
	}
	return model;
}

EntityId SpawnMesh (CWorld& InWorld, const Vector3r& InTranslation, uint32 InSectionCount = 1u)
{
	const EntityId e = SpawnAt(InWorld, InTranslation);
	InWorld.Add<graphics::Comp_RenderModel>(e).Model = MakeModel(InSectionCount);
	return e;
}
}

TEST(ExtractMeshInstances, EmptyWorldAppendsNothing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	EXPECT_EQ(instances.Count(), 0u);
}

TEST(ExtractMeshInstances, CarriesEntityModelAndTransform)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnMesh(world, Vector3r(1, 2, 3));
	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	ASSERT_EQ(instances.Count(), 1u);
	EXPECT_EQ(instances[0].Entity, e) << "needed for reverse lookup from InstanceID";
	EXPECT_NE(instances[0].Model, nullptr);

	// Row-major 3x4 with translation in column 3.
	//
	// NOT negated on X, unlike a light's position. ToDirectXHandedness right-multiplies by
	// diag(-1,1,1), which mirrors the source basis and leaves the world position alone,
	// whereas ToDirectXCoordinates negates a bare vector. This reproduces exactly what
	// STransform::GetRaytracingTransform does today - see the byte-for-byte comparison
	// below - so the asymmetry between geometry and lights is inherited, not introduced.
	EXPECT_NEAR(instances[0].Transform.M[0][3], 1.0, Tolerance);
	EXPECT_NEAR(instances[0].Transform.M[1][3], 2.0, Tolerance);
	EXPECT_NEAR(instances[0].Transform.M[2][3], 3.0, Tolerance);
}

TEST(ExtractMeshInstances, MatchesTheSTransformRaytracingTransform)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const Vector3f translation(1.0f, 2.0f, 3.0f);
	const Vector3f rotation(0.3f, 0.7f, 1.1f);
	const Vector3f scale(2.0f);

	const EntityId e = world.Spawn();
	Comp_LocalTransform& local = world.Add<Comp_LocalTransform>(e);
	local.SetTranslation(math::VectorCast<Real>(translation));
	local.SetRotation(math::VectorCast<Real>(rotation));
	local.SetScale(math::VectorCast<Real>(scale));
	world.Add<Comp_WorldTransform>(e);
	world.Add<graphics::Comp_RenderModel>(e).Model = MakeModel(1u);

	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);
	ASSERT_EQ(instances.Count(), 1u);

	// Byte-for-byte the same instance transform the CEntity path produces today, which is
	// what makes swapping the source safe.
	math::STransform reference;
	reference.SetTranslation(translation);
	reference.SetRotation(rotation);
	reference.SetScale(scale);
	const DirectX::XMFLOAT3X4 expected = reference.GetRaytracingTransform();

	for (int row = 0; row < 3; ++row)
	{
		for (int col = 0; col < 4; ++col)
		{
			EXPECT_NEAR(instances[0].Transform.M[row][col], expected.m[row][col], Tolerance)
				<< "element [" << row << "][" << col << "]";
		}
	}
}

TEST(ExtractMeshInstances, HiddenAndModellessEntitiesAreSkipped)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	SpawnMesh(world, Vector3r(0, 0, 0));

	const EntityId hidden = SpawnMesh(world, Vector3r(1, 0, 0));
	world.Get<graphics::Comp_RenderModel>(hidden).bVisible = false;

	// Present but with nothing to draw, as when a portal's visualization quad is not built.
	const EntityId modelless = SpawnAt(world, Vector3r(2, 0, 0));
	world.Add<graphics::Comp_RenderModel>(modelless);

	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	EXPECT_EQ(instances.Count(), 1u);
}

TEST(ExtractMeshInstances, OriginIsSubtractedBeforeNarrowing)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = SpawnMesh(world, Vector3r(100, 50, 25));
	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r(100, 50, 0), instances);

	ASSERT_EQ(instances.Count(), 1u);
	EXPECT_NEAR(instances[0].Transform.M[0][3], 0.0, Tolerance);
	EXPECT_NEAR(instances[0].Transform.M[1][3], 0.0, Tolerance);
	EXPECT_NEAR(instances[0].Transform.M[2][3], 25.0, Tolerance);
}


// The property the acceleration structure actually depends on.

TEST(ExtractMeshInstances, OrderIsStableWhileTheSetIsUnchanged)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	TArray<EntityId> entities;
	for (uint32 i = 0u; i < 50u; ++i)
	{
		entities.Add(SpawnMesh(world, Vector3r(static_cast<Real>(i), 0, 0)));
	}

	transforms.Run();

	TArray<graphics::SMeshInstance> first;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, first);

	// Move everything. Transforms change; membership does not.
	for (uint32 i = 0u; i < entities.Count(); ++i)
	{
		world.Get<Comp_LocalTransform>(entities[i]).SetTranslation(Vector3r(static_cast<Real>(i) * Real(2), 1, 0));
	}
	transforms.Run();

	TArray<graphics::SMeshInstance> second;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, second);

	ASSERT_EQ(first.Count(), second.Count());
	for (uint32 i = 0u; i < first.Count(); ++i)
	{
		// InstanceID is the array index and is visible to shaders; hit-group offsets are a
		// running sum over this order. If entries moved, both silently change meaning.
		EXPECT_EQ(first[i].Entity, second[i].Entity) << "entry " << i << " changed position";
	}
}

TEST(ExtractMeshInstances, StructuralVersionIsTheRefitVersusRebuildSignal)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId a = SpawnMesh(world, Vector3r(0, 0, 0));
	SpawnMesh(world, Vector3r(1, 0, 0));
	transforms.Run();

	const uint64 afterSpawn = world.Pool<graphics::Comp_RenderModel>().GetStructuralVersion();

	// Moving an entity is a data change, not a structural one: refit is valid.
	world.Get<Comp_LocalTransform>(a).SetTranslation(Vector3r(9, 0, 0));
	transforms.Run();
	EXPECT_EQ(world.Pool<graphics::Comp_RenderModel>().GetStructuralVersion(), afterSpawn)
		<< "a transform change must not force a rebuild";

	// Destroying one is structural: positions after it shift, so the TLAS must be rebuilt.
	world.Destroy(a);
	EXPECT_NE(world.Pool<graphics::Comp_RenderModel>().GetStructuralVersion(), afterSpawn);
}

TEST(ExtractMeshInstances, DestroyedInstanceDropsOut)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId a = SpawnMesh(world, Vector3r(0, 0, 0));
	const EntityId b = SpawnMesh(world, Vector3r(5, 0, 0));
	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);
	ASSERT_EQ(instances.Count(), 2u);

	world.Destroy(a);
	transforms.Run();

	instances.Clear();
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	ASSERT_EQ(instances.Count(), 1u);
	EXPECT_EQ(instances[0].Entity, b);
}

TEST(ExtractMeshInstances, ModelReferenceSurvivesPoolGrowth)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	// Comp_RenderModel holds a TRefShared, so it is not trivially copyable and relies on
	// TArray's move path when the pool grows.
	for (uint32 i = 0u; i < 300u; ++i)
	{
		SpawnMesh(world, Vector3r(static_cast<Real>(i), 0, 0), 1u + (i % 3u));
	}

	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	ASSERT_EQ(instances.Count(), 300u);
	for (uint32 i = 0u; i < instances.Count(); ++i)
	{
		ASSERT_NE(instances[i].Model, nullptr) << "instance " << i;
		EXPECT_EQ(instances[i].Model->Sections.Count(), 1u + (i % 3u));
	}
}

TEST(ExtractMeshInstances, InheritsAParentTransform)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId parent = SpawnAt(world, Vector3r(10, 0, 0));
	const EntityId child = SpawnMesh(world, Vector3r(0, 5, 0));
	ASSERT_TRUE(world.GetHierarchy().SetParent(child, parent));

	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	ASSERT_EQ(instances.Count(), 1u);
	EXPECT_NEAR(instances[0].Transform.M[0][3], 10.0, Tolerance);
	EXPECT_NEAR(instances[0].Transform.M[1][3], 5.0, Tolerance);
}

TEST(ExtractMeshInstances, DoesNotDirtyWhatItReads)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	SpawnMesh(world, Vector3r(0, 0, 0));
	transforms.Run();

	const uint64 meshMark = world.Pool<graphics::Comp_RenderModel>().GetVersion();
	const uint64 transformMark = world.Pool<Comp_WorldTransform>().GetVersion();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);

	EXPECT_EQ(world.Pool<graphics::Comp_RenderModel>().GetVersion(), meshMark);
	EXPECT_EQ(world.Pool<Comp_WorldTransform>().GetVersion(), transformMark);
}


// ---------------------------------------------------------------------------------
// Raster / raytracing transform bridge
// ---------------------------------------------------------------------------------

TEST(ToWorldMatrix, RoundTripsSTransformsWorldMatrix)
{
	// Both pipelines are now driven by one transform per drawable: the acceleration
	// structure takes the instance form directly, and raster object constants come from
	// this conversion. If it is wrong, every rasterised object is silently misplaced.
	const Vector3f translations[] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 3.0f }, { -5.0f, 0.5f, 9.0f } };
	const Vector3f rotations[]    = { { 0.0f, 0.0f, 0.0f }, { 0.3f, 0.7f, 1.1f }, { -1.2f, 2.4f, -0.6f } };
	const Vector3f scales[]       = { { 1.0f, 1.0f, 1.0f }, { 2.0f, 2.0f, 2.0f }, { 1.5f, 1.0f, 1.0f } };

	for (uint32 c = 0u; c < 3u; ++c)
	{
		SCOPED_TRACE(testing::Message() << "case " << c);

		math::STransform transform;
		transform.SetTranslation(translations[c]);
		transform.SetRotation(rotations[c]);
		transform.SetScale(scales[c]);

		const DirectX::XMFLOAT4X4 expected = transform.GetMatrix();
		const DirectX::XMFLOAT4X4 actual = graphics::ToWorldMatrix(transform.GetRaytracingTransform());

		for (int row = 0; row < 4; ++row)
		{
			for (int col = 0; col < 4; ++col)
			{
				EXPECT_NEAR(actual.m[row][col], expected.m[row][col], Tolerance)
					<< "element [" << row << "][" << col << "]";
			}
		}
	}
}

TEST(ToWorldMatrix, RoundTripsAnEcsInstanceTransform)
{
	FRT_TEST_MEMORY_POOL();
	CWorld world;
	Sys_Transform transforms(world);

	const EntityId e = world.Spawn();
	Comp_LocalTransform& local = world.Add<Comp_LocalTransform>(e);
	local.SetTranslation(Vector3r(1, 2, 3));
	local.SetRotation(Vector3r(Real(0.3), Real(0.7), Real(1.1)));
	local.SetScale(Vector3r(2, 2, 2));
	world.Add<Comp_WorldTransform>(e);
	world.Add<graphics::Comp_RenderModel>(e).Model = MakeModel(1u);

	transforms.Run();

	TArray<graphics::SMeshInstance> instances;
	graphics::ExtractMeshInstances(world, Vector3r::ZeroVector, instances);
	ASSERT_EQ(instances.Count(), 1u);

	DirectX::XMFLOAT3X4 instanceTransform = {};
	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 4u; ++col)
		{
			instanceTransform.m[row][col] = instances[0].Transform.M[row][col];
		}
	}

	// An ECS entity must reach the rasteriser with the same world matrix a CEntity does.
	math::STransform reference;
	reference.SetTranslation(Vector3f(1.0f, 2.0f, 3.0f));
	reference.SetRotation(Vector3f(0.3f, 0.7f, 1.1f));
	reference.SetScale(Vector3f(2.0f));
	const DirectX::XMFLOAT4X4 expected = reference.GetMatrix();

	const DirectX::XMFLOAT4X4 actual = graphics::ToWorldMatrix(instanceTransform);

	for (int row = 0; row < 4; ++row)
	{
		for (int col = 0; col < 4; ++col)
		{
			EXPECT_NEAR(actual.m[row][col], expected.m[row][col], Tolerance)
				<< "element [" << row << "][" << col << "]";
		}
	}
}


// ---------------------------------------------------------------------------------
// Component registration
// ---------------------------------------------------------------------------------

TEST(GraphicsComponents, LightAndPortalAreUsableAsComponents)
{
	// Both were already flat data on CEntity, so moving them into pools needs no change to
	// either struct - only the TRefShared ownership goes away.
	EXPECT_TRUE(std::is_trivially_copyable_v<graphics::Comp_Light>);
	EXPECT_TRUE(std::is_trivially_copyable_v<graphics::Comp_Portal>);

	EXPECT_NE(CComponentRegistry::Get().FindByName("Comp_Light"), nullptr);
	EXPECT_NE(CComponentRegistry::Get().FindByName("Comp_Portal"), nullptr);
}

TEST(GraphicsComponents, RenderModelIsRegisteredAndNonTrivial)
{
	// Unlike the other two: it holds a shared reference, so it depends on the pool's
	// support for resource-owning components. Swapping it for a MeshHandle would make it
	// trivially copyable again, and that is the intended direction.
	EXPECT_FALSE(std::is_trivially_copyable_v<graphics::Comp_RenderModel>);
	EXPECT_NE(CComponentRegistry::Get().FindByName("Comp_RenderModel"), nullptr);
}
