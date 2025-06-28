#include "World.h"

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

	// this is so messed up...
	currentFrameResources.PassCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	if (!currentFrameResources.PassCB.DescriptorHeapHandleGpu.empty())
	{
		CommandList->SetGraphicsRootDescriptorTable(2, currentFrameResources.PassCB.DescriptorHeapHandleGpu[0]);
	}

	currentFrameResources.ObjectCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	auto& ObjectDescriptorHandles = currentFrameResources.ObjectCB.DescriptorHeapHandleGpu;
	for (uint32 i = 0; i < Entities.Count(); ++i)
	{
		if (i < ObjectDescriptorHandles.size())
		{
			CommandList->SetGraphicsRootDescriptorTable(1, ObjectDescriptorHandles[i]);
		}
		Entities[i]->Present(DeltaSeconds, CommandList);
	}
}

void CWorld::CopyConstantData ()
{
	// TODO: ideally, CBs should already be stored in one array
	// TODO: use (when it's implemented) memory pool
	uint64 alignedSize = memory::AlignAddress(sizeof(graphics::SObjectConstants), 256);
	auto objectsData = malloc(alignedSize * Entities.Count());
	for (int i = 0; i < Entities.Count(); ++i)
	{
		memcpy(
			(uint8*)objectsData + alignedSize * i, &Entities[i]->Transform.GetMatrix(),
			sizeof(graphics::SObjectConstants));
	}

	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	currentFrameResources.ObjectCB.CopyBunch(
		(graphics::SObjectConstants*)objectsData, currentFrameResources.UploadArena);

	free(objectsData);

	using namespace DirectX;

	graphics::SPassConstants passConstants;
	const auto [renderWidth, renderHeight] = GameInstance::GetInstance().GetWindow().GetWindowSize();

	const auto Camera = GameInstance::GetInstance().GetCamera();
	XMMATRIX view = Camera->GetViewMatrix();
	XMMATRIX projection = Camera->GetProjectionMatrix(90.f, (float)renderWidth / renderHeight, 1.f, 1'000.f);

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
	passConstants.CameraPosition = Camera->Position;
	passConstants.RenderTargetSize = Vector2f(renderWidth, renderHeight);
	passConstants.RenderTargetSizeInverse = Vector2f(1.0f / renderWidth, 1.0f / renderHeight);
	passConstants.NearPlane = 1.0f;
	passConstants.FarPlane = 1000.0f;
	passConstants.TotalTime = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	passConstants.DeltaTime = GameInstance::GetInstance().GetTime().GetDeltaSeconds();

	currentFrameResources.PassCB.CopyBunch(&passConstants, currentFrameResources.UploadArena);
}
#endif

memory::TRefShared<CEntity> CWorld::SpawnEntity ()
{
	auto newEntity = memory::NewShared<CEntity>();
	Entities.Add(newEntity);
	return newEntity;
}
