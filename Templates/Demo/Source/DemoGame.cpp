#include "DemoGame.h"

#include "EnginePaths.h"
#include "Entity.h"
#include "WorldScene.h"
#include "ECS/Comp_Name.h"
#include "ECS/World.h"
#include "Graphics/FlyCamera.h"
#include "Graphics/GeometryScript.h"
#include "Graphics/MeshGeneration.h"
#include "Graphics/Model.h"
#include "Graphics/Render/MaterialLibrary.h"
#include "Input/InputActionLibrary.h"
#include "Math/Comp_LocalTransform.h"
#include "Math/Comp_WorldTransform.h"


using namespace frt;
using namespace frt::graphics;

namespace
{
// Everything the demo loads is in its project's Content/.
std::filesystem::path Content (FStringView RelativePath)
{
	return paths::GetProjectContentDir() / RelativePath.ToPath();
}
}

void DemoGame::BuildWorld (const SGameContext& Context)
{
	CWorldScene& World = Context.World;
	CMaterialLibrary& materialLibrary = Context.Materials;

	World.Camera.Transform.SetTranslation(4.9f, 2.2f, -1.5f);
	World.Camera.Transform.SetRotation(-0.038f, -1.356f, .0f);

	const std::filesystem::path floorMaterialPath = Content("Models/Floor/floor_mat0.frtmat.yml");
	// auto floor = World.SpawnEntity("Floor");
	// floor->RenderModel->Model = memory::NewShared<SRenderModel>(
	// 	SRenderModel::FromMesh(
	// 		mesh::GenerateGrid(10.f, 10.f, 16u, 16u),
	// 		materialLibrary.LoadOrCreateMaterial(floorMaterialPath, {})));
	// floor->bRayTraced = true;
	// floor->Transform.SetTranslation(0.f, -1.f, 0.f);

	auto floorEnt = World.GetEcsWorld().Spawn();
	World.GetEcsWorld().Add<Comp_LocalTransform>(floorEnt, Vector3r::DownVector * 1.f);
	World.GetEcsWorld().Add<Comp_WorldTransform>(floorEnt);
	World.GetEcsWorld().Add<Comp_Name>(floorEnt, "Floor");
	World.GetEcsWorld().Add<Comp_RenderModel>(floorEnt, memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateGrid(10.f, 10.f, 16u, 16u),
			materialLibrary.LoadOrCreateMaterial(floorMaterialPath, {}))));

	const std::filesystem::path pillarMaterialPath = Content("Models/Pillar/pillar_mat0.frtmat.yml");
	auto pillar = World.SpawnEntity("Pillar");
	pillar->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(.65f, 1.8f, .65f), 1),
			// mesh::GenerateCylinder(0.65f, 0.65f, 1.8f, 20u, 2u),
			materialLibrary.LoadOrCreateMaterial(pillarMaterialPath, {})));
	pillar->bRayTraced = true;
	pillar->Transform.SetTranslation(-2.5f, -0.2f, 0.f);

	auto mirror = World.SpawnEntity("Mirror");
	mirror->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateQuad(3.f, 3.f),
			materialLibrary.LoadOrCreateMaterial(pillarMaterialPath, {})));
	mirror->Transform.SetTranslation(3.f, 1.f, -.5f);
	mirror->Transform.SetRotation(0.f, 0.f, math::PI_OVER_TWO);

	const std::filesystem::path cubeMaterialPath = Content("Models/Cube/cube_mat0.frtmat.yml");
	auto cube = World.SpawnEntity("Cube");
	cube->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			mesh::GenerateCube(Vector3f(1.f), 1),
			materialLibrary.LoadOrCreateMaterial(cubeMaterialPath, {})));
	cube->Transform.SetTranslation(1.5f, 0.f, -1.5f);

	// Cylinder = World->SpawnEntity();
	// Cylinder->RenderModel.Model = memory::NewShared<graphics::SRenderModel>(
	// 	graphics::SRenderModel::FromMesh(mesh::GenerateCylinder(1.f, 0.5, 1.f, 10u, 10u)));

	auto Sphere = World.SpawnEntity("Sphere");
	Sphere->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::FromMesh(mesh::GenerateSphere(.3f, 30u, 30u)));

	auto skullEnt = World.SpawnEntity("Skull");
	skullEnt->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			Content("Models/Skull/scene.gltf"),
			Content("Models/Skull/textures/defaultMat_baseColor.jpeg"), materialLibrary));
	skullEnt->Transform.SetTranslation(-2.5f, 1.5f, 0.f);
	skullEnt->Transform.SetScale(Vector3f(.45f));
	skullEnt->RotationSpeed = Vector3f::UpVector * (math::PI_OVER_FOUR * 0.25f);

	auto duckEnt = World.SpawnEntity("Duck");
	duckEnt->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			Content("Models/Duck/Duck.gltf"),
			Content("Models/Duck/DuckCM.png"), materialLibrary));
	duckEnt->Transform.SetTranslation(0.f, 0.f, 0.f);

	// TODO: When Sponza is added, the renderer crashes. Probably multiple sections aren't handled properly
	auto sponzaEnt = World.SpawnEntity("Sponza");
	sponzaEnt->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		graphics::SRenderModel::LoadFromFile(
			Content("Models/Sponza/Sponza.gltf"),
			{}, materialLibrary));
	// sponzaEnt->Transform.SetTranslation(1.f, 0.f, 0.f);
	sponzaEnt->bRayTraced = false;

	const std::filesystem::path lightMaterialPath = Content("Light/light_mat0.frtmat.yml");

	auto lightSource1 = World.SpawnEntity("Light 1");
	lightSource1->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
		SRenderModel::FromMesh(mesh::GenerateSphere(.3f, 30u, 30u),
			materialLibrary.LoadOrCreateMaterial(lightMaterialPath, {})));
	lightSource1->Transform.SetTranslation(0.f, 2.f, -3.f);

	// const std::filesystem::path lightMaterialPath2 = Content("Light/light_mat2.frtmat.yml");

	// auto lightSource2 = World.SpawnEntity("Light 2");
	// lightSource2->RenderModel->Model = memory::NewShared<graphics::SRenderModel>(
	// 	SRenderModel::FromMesh(mesh::GenerateQuad(1.f, 1.f),
	// 		materialLibrary.LoadOrCreateMaterial(lightMaterialPath2, {})));
	// lightSource2->Transform.SetTranslation(-2.5f, 2.85f, 0.f);
	// lightSource2->Transform.SetRotation(math::PI, 0.f, 0.f);

	graphics::SGeometryScript ceilingScript;
	using EShape = graphics::SGeometryScript::EShape;
	using EMerge = graphics::SGeometryScript::EMergeOp;

	// outer 10×6 wall
	ceilingScript.Ops.Add({ EShape::Rect, EMerge::AddBase,
		Vector2f(0.f, 0.f), Vector2f(5.1f, 3.f) });
	// window: 1×1.6 at (-1.5, 0)
	ceilingScript.Ops.Add({ EShape::Rect, EMerge::Cut,
		Vector2f(-1.5f, 0.f), Vector2f(0.5f, 0.8f) });
	// door: 0.8×2.4 at (1.5, -0.3)
	// ceilingScript.Ops.Add({ EShape::Rect, EMerge::Cut,
	// 	Vector2f(1.5f, -0.3f), Vector2f(0.4f, 1.2f) });

	auto ceiling = World.SpawnEntity("Ceiling");
	ceiling->RenderModel->Model = memory::NewShared<SRenderModel>(
		SRenderModel::FromMesh(
			graphics::mesh::BuildFromScript(ceilingScript),
			materialLibrary.LoadOrCreateMaterial(cubeMaterialPath, {})));
	ceiling->bRayTraced = true;
	ceiling->Transform.SetTranslation(-0.5f, 9.f, 0.f);
	ceiling->Transform.SetRotation(math::PI_OVER_TWO, 0.f, 0.f);
	ceiling->Transform.SetScale({ 1.5f, 1.f, 1.f });

	auto portal = World.SpawnEntity("Portal1");
	portal->Transform.SetTranslation(1.75f, 9.f, 0.f);   // window center
	portal->Transform.SetRotation(math::PI_OVER_TWO, 0.f, 0.f);   // window center
	portal->Portal = memory::NewShared<graphics::Comp_Portal>();
	portal->Portal->Normal = Vector3f(0, 0, -1);       // faces interior
	portal->Portal->Edge1  = Vector3f(0.8f, 0, 0);     // half-width
	portal->Portal->Edge2  = Vector3f(0, 0.8f, 0);     // half-height
}

void DemoGame::BeginPlay (const SGameContext& Context)
{
	FlyActions = Context.InputActions.LoadOrCreateActionMap(Content("Input/IAM_Editor.frtinputmap"));
}

void DemoGame::Tick (const SGameContext& Context, const SGameFrame& Frame)
{
	// Flies on wall-clock time, so the camera still moves while the world is paused.
	if (Frame.Input.Device && FlyActions)
	{
		const SFlyCameraInput fly = ReadFlyCameraInput(FlyActions->ActionMap, *Frame.Input.Device, Frame.Input.LookDelta);
		UpdateFlyCamera(Context.World.Camera, fly, Frame.RealDeltaSeconds, Frame.Input.ViewAspect);
	}
}
