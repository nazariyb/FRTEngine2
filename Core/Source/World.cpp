#include "World.h"

#include <unordered_map>

#include "GameInstance.h"
#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/Renderer.h"

using namespace frt;

#if !defined(FRT_HEADLESS)
CWorld::CWorld (memory::TRefWeak<graphics::CRenderer> InRenderer)
	: Renderer(InRenderer)
{}
#else
CWorld::CWorld ()
{}
#endif

void CWorld::Tick (float DeltaSeconds)
{
	for (auto& entity : Entities)
	{
		entity->Tick(DeltaSeconds);
	}
#if !defined(FRT_HEADLESS)
	CopyConstantData();
#endif
}

#if !defined(FRT_HEADLESS)
void CWorld::Present (float DeltaSeconds, ID3D12GraphicsCommandList* CommandList)
{
	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	// TODO: assign stable material indices in MaterialLibrary and update constants only when dirty.
	std::unordered_map<const graphics::SMaterial*, uint32> materialIndices;
	TArray<graphics::SMaterialConstants> materialConstants;

	for (uint32 i = 0; i < Entities.Count(); ++i)
	{
		CEntity* entity = Entities[i].GetRawIgnoringLifetime();
		frt_assert(entity);
		if (!entity->RenderModel.Model)
		{
			continue;
		}

		graphics::SRenderModel& model = *entity->RenderModel.Model;
		for (const graphics::SRenderSection& section : model.Sections)
		{
			if (section.MaterialIndex >= model.Materials.Count())
			{
				continue;
			}

			graphics::SMaterial* material = model.Materials[section.MaterialIndex].GetRawIgnoringLifetime();
			frt_assert(material);

			auto it = materialIndices.find(material);
			if (it == materialIndices.end())
			{
				const uint32 materialIndex = materialConstants.Count();
				materialIndices.emplace(material, materialIndex);

				graphics::SMaterialConstants& constants = materialConstants.Add();
				constants.DiffuseAlbedo = material->DiffuseAlbedo;
				material->RuntimeIndex = materialIndex;
			}
			else
			{
				material->RuntimeIndex = it->second;
			}
		}
	}

	Renderer->EnsureMaterialConstantCapacity(materialConstants.Count());
	if (!materialConstants.IsEmpty())
	{
		currentFrameResources.MaterialCB.CopyBunch(
			materialConstants.GetData(),
			materialConstants.Count(),
			currentFrameResources.UploadArena);
	}

	// this is so messed up...
	currentFrameResources.PassCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	if (!currentFrameResources.PassCB.DescriptorHeapHandleGpu.empty())
	{
		CommandList->SetGraphicsRootDescriptorTable(
			render::constants::RootParam_PassCbv,
			currentFrameResources.PassCB.DescriptorHeapHandleGpu[0]);
	}

	currentFrameResources.ObjectCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	currentFrameResources.MaterialCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	auto& ObjectDescriptorHandles = currentFrameResources.ObjectCB.DescriptorHeapHandleGpu;
	for (uint32 i = 0; i < Entities.Count(); ++i)
	{
		if (i < ObjectDescriptorHandles.size())
		{
			CommandList->SetGraphicsRootDescriptorTable(
				render::constants::RootParam_ObjectCbv,
				ObjectDescriptorHandles[i]);
		}
		Entities[i]->Present(DeltaSeconds, CommandList);
	}
}

void CWorld::CopyConstantData ()
{
	// TODO: ideally, CBs should already be stored in one array
	// TODO: use (when it's implemented) memory pool
	const uint32 entityCount = Entities.Count();
	Renderer->EnsureObjectConstantCapacity(entityCount);

	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	if (entityCount > 0u)
	{
		TArray<graphics::SObjectConstants> objectConstants;
		objectConstants.SetSizeUninitialized(entityCount);
		for (uint32 i = 0; i < entityCount; ++i)
		{
			objectConstants[i].World = Entities[i]->Transform.GetMatrix();
		}

		currentFrameResources.ObjectCB.CopyBunch(
			objectConstants.GetData(),
			objectConstants.Count(),
			currentFrameResources.UploadArena);
	}

	using namespace DirectX;

	graphics::SPassConstants passConstants;
	const auto [renderWidth, renderHeight] = GameInstance::GetInstance().GetWindow().GetWindowSize();

	const auto Camera = GameInstance::GetInstance().GetCamera();
	XMMATRIX view = Camera->GetViewMatrix();
	XMMATRIX projection = Camera->GetProjectionMatrix(math::PI_OVER_TWO, (float)renderWidth / renderHeight, 1.f, 1'000.f);

	XMMATRIX viewProj = XMMatrixMultiply(view, projection);

	auto viewDeterminant = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&viewDeterminant, view);

	auto projectionDeterminant = XMMatrixDeterminant(projection);
	XMMATRIX invProj = XMMatrixInverse(&projectionDeterminant, projection);

	auto viewProjDeterminant = XMMatrixDeterminant(viewProj);
	XMMATRIX invViewProj = XMMatrixInverse(&viewProjDeterminant, viewProj);

	XMStoreFloat4x4(&passConstants.View, view);
	XMStoreFloat4x4(&passConstants.ViewInverse, invView);
	XMStoreFloat4x4(&passConstants.Projection, projection);
	XMStoreFloat4x4(&passConstants.ProjectionInverse, invProj);
	XMStoreFloat4x4(&passConstants.ViewProjection, viewProj);
	XMStoreFloat4x4(&passConstants.ViewProjectionInverse, invViewProj);
	passConstants.CameraPosition = math::ToDirectXCoordinates(Camera->Transform.GetTranslation());
	passConstants.RenderTargetSize = Vector2f(renderWidth, renderHeight);
	passConstants.RenderTargetSizeInverse = Vector2f(1.0f / renderWidth, 1.0f / renderHeight);
	passConstants.NearPlane = .1f;
	passConstants.FarPlane = 1'000.0f;
	passConstants.TotalTime = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	passConstants.DeltaTime = GameInstance::GetInstance().GetTime().GetDeltaSeconds();

	currentFrameResources.PassCB.CopyBunch(&passConstants, 1u, currentFrameResources.UploadArena);
}
#endif

memory::TRefShared<CEntity> CWorld::SpawnEntity ()
{
	auto newEntity = memory::NewShared<CEntity>();
	Entities.Add(newEntity);
	return newEntity;
}
