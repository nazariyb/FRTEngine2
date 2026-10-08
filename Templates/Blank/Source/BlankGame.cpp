#include "BlankGame.h"

#include "EnginePaths.h"
#include "Entity.h"
#include "WorldScene.h"
#include "Graphics/MeshGeneration.h"
#include "Graphics/Model.h"
#include "Graphics/Render/MaterialLibrary.h"


using namespace frt;
using namespace frt::graphics;

void BlankGame::BuildWorld (const SGameContext& Context)
{
	CWorldScene& world = Context.World;
	CMaterialLibrary& materials = Context.Materials;
	// The project's Content/, where its assets are.
	const std::filesystem::path content = paths::GetProjectContentDir();

	world.Camera.Transform.SetTranslation(0.f, 1.5f, -6.f);

	auto floor = world.SpawnEntity("Floor");
	floor->RenderModel->Model = memory::NewShared<SRenderModel>(SRenderModel::FromMesh(
		mesh::GenerateGrid(12.f, 12.f, 16u, 16u),
		materials.LoadOrCreateMaterial(content / "Materials" / "Floor.frtmat.yml", {})));
	floor->bRayTraced = true;

	auto cube = world.SpawnEntity("Cube");
	cube->RenderModel->Model = memory::NewShared<SRenderModel>(SRenderModel::FromMesh(
		mesh::GenerateCube(Vector3f(1.f), 1u),
		materials.LoadOrCreateMaterial(content / "Materials" / "Shape.frtmat.yml", {})));
	cube->bRayTraced = true;
	cube->Transform.SetTranslation(-1.5f, 0.5f, 0.f);

	auto sphere = world.SpawnEntity("Sphere");
	sphere->RenderModel->Model = memory::NewShared<SRenderModel>(SRenderModel::FromMesh(
		mesh::GenerateSphere(.6f, 32u, 32u),
		materials.LoadOrCreateMaterial(content / "Materials" / "Shape.frtmat.yml", {})));
	sphere->bRayTraced = true;
	sphere->Transform.SetTranslation(1.5f, .6f, 0.f);

	auto light = world.SpawnEntity("Light");
	light->RenderModel->Model = memory::NewShared<SRenderModel>(SRenderModel::FromMesh(
		mesh::GenerateSphere(.2f, 24u, 24u),
		materials.LoadOrCreateMaterial(content / "Materials" / "Light.frtmat.yml", {})));
	light->Transform.SetTranslation(0.f, 2.5f, -1.f);
}
