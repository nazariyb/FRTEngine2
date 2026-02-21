#include "WorldScene.h"

#include "GameInstance.h"
#include "Sys_MeshRenderer.h"

frt::CWorldScene::CWorldScene (GameInstance& InGame)
	: Game(InGame)
{}

bool frt::CWorldScene::Initialize ()
{
	MeshRenderer = memory::NewUnique<Sys_MeshRenderer>(Game.GetRenderer());
	return true;
}

frt::memory::TRefShared<frt::CEntity> frt::CWorldScene::SpawnEntity ()
{
	auto newEntity = memory::NewShared<CEntity>();
	Entities.Add(newEntity);
	newEntity->RenderModel = MeshRenderer->SpawnRenderModel();
	bSceneTopologyDirty = true;
	return newEntity;
}

void frt::CWorldScene::RunFrame ()
{
	SUpdateContext Context;

	const SFlags<EUpdatePhase>& MeshRendererPhases = MeshRenderer->GetPhases();

	if (!IsPhasePaused(EUpdatePhase::Prepare) && (MeshRendererPhases && EUpdatePhase::Prepare))
	{
		MeshRenderer->Prepare(Context);
	}

	if (!IsPhasePaused(EUpdatePhase::Update) && (MeshRendererPhases && EUpdatePhase::Update))
	{
		MeshRenderer->Update(Context);
	}

	if (!IsPhasePaused(EUpdatePhase::Finalize) && (MeshRendererPhases && EUpdatePhase::Finalize))
	{
		MeshRenderer->Finalize(Context);
	}

	for (auto& entity : Entities)
	{
		entity->Tick(1.f / 60.f);
	}

	// TODO: ideally, CBs should already be stored in one array
	// TODO: use (when it's implemented) memory pool
	const uint32 entityCount = Entities.Count();
	Game.GetRenderer()->EnsureObjectConstantCapacity(entityCount);

	auto& currentFrameResources = Game.GetRenderer()->GetCurrentFrameResource();

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
}

void frt::CWorldScene::SubmitFrame (ID3D12GraphicsCommandList4* CommandList)
{
	SDrawUpdateContext Context;
	Context.CommandList = CommandList;

	const SFlags<EUpdatePhase>& MeshRendererPhases = MeshRenderer->GetPhases();
	if (MeshRendererPhases && EUpdatePhase::Draw)
	{
		MeshRenderer->Draw(Context);
	}
}

bool frt::CWorldScene::IsPhasePaused (EUpdatePhase Phase) const
{
	return PausedPhases && Phase;
}

void frt::CWorldScene::PausePhases (SFlags<EUpdatePhase> Phases)
{
	PausedPhases += Phases;
}

void frt::CWorldScene::UnpausePhases (SFlags<EUpdatePhase> Phases)
{
	PausedPhases -= Phases;
}

bool frt::CWorldScene::TogglePhasePause (EUpdatePhase Phase)
{
	PausedPhases ^= Phase;
	return PausedPhases && Phase;
}
