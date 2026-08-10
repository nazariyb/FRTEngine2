#include "WorldScene.h"

#include "GameInstance.h"
#include "Sys_MeshRenderer.h"
#include "ECS/CommandBuffer.h"
#include "ECS/CoreComponents.h"
#include "ECS/Sys_Lifetime.h"
#include "ECS/Sys_Transform.h"
#include "ECS/World.h"

frt::CWorldScene::CWorldScene (GameInstance& InGame)
	: Game(InGame)
{}

frt::CWorldScene::~CWorldScene ()
{
	// Virtual destructor, then release the storage: NewUnmanaged / DestroyUnmanaged only
	// move raw bytes and never run a destructor.
	for (uint32 i = 0u; i < Systems.Count(); ++i)
	{
		if (Systems[i] != nullptr)
		{
			Systems[i]->~ISystem();
			memory::DestroyUnmanaged(Systems[i]);
			Systems[i] = nullptr;
		}
	}
}

bool frt::CWorldScene::Initialize ()
{
#ifndef FRT_HEADLESS
	MeshRenderer = memory::NewUnique<Sys_MeshRenderer>(Game.GetRenderer());
#else
	MeshRenderer = memory::NewUnique<Sys_MeshRenderer>();
#endif

	// Safe here but not in the constructor: GameInstance builds its memory pool in its
	// own body, which runs after CWorldScene is constructed as a member. CWorld allocates
	// its parent and children pools up front, so it has to wait for Initialize().
	EcsWorld = memory::NewUnique<CWorld>();
	Commands = memory::NewUnique<CCommandBuffer>();

	// Registration order IS execution order within a phase, until systems can declare
	// dependencies.
	Systems.Add(memory::NewUnmanaged<Sys_Lifetime>(*EcsWorld, *Commands));
	Systems.Add(memory::NewUnmanaged<Sys_Transform>(*EcsWorld));

	for (ISystem* system : Systems)
	{
		if (!system->Initialize())
		{
			return false;
		}
	}

	return true;
}

frt::CWorld& frt::CWorldScene::GetEcsWorld ()
{
	frt_assert(EcsWorld);
	return *EcsWorld;
}

const frt::CWorld& frt::CWorldScene::GetEcsWorld () const
{
	frt_assert(EcsWorld);
	return *EcsWorld;
}

frt::CCommandBuffer& frt::CWorldScene::GetCommands ()
{
	frt_assert(Commands);
	return *Commands;
}

void frt::CWorldScene::RunSystemsPhase (EUpdatePhase Phase, const SUpdateContext& Context)
{
	if (IsPhasePaused(Phase))
	{
		return;
	}

	for (ISystem* system : Systems)
	{
		if (!(system->GetPhases() && Phase))
		{
			continue;
		}

		switch (Phase)
		{
			case EUpdatePhase::Input:    system->Input(Context);    break;
			case EUpdatePhase::Prepare:  system->Prepare(Context);  break;
			case EUpdatePhase::Update:   system->Update(Context);   break;
			case EUpdatePhase::Finalize: system->Finalize(Context); break;
			default: break;
		}
	}
}

frt::memory::TRefShared<frt::CEntity> frt::CWorldScene::SpawnEntity (const std::string& Name)
{
	auto newEntity = memory::NewShared<CEntity>();
	if (Name.empty())
	{
		newEntity->Name = "Entity #" + std::to_string(Entities.Count());
	}
	else
	{
		newEntity->Name = Name;
	}
	Entities.Add(newEntity);
	newEntity->RenderModel = MeshRenderer->SpawnRenderModel();
	bSceneTopologyDirty = true;
	return newEntity;
}

void frt::CWorldScene::RunFrame (float InDeltaSeconds)
{
	SUpdateContext Context;
	Context.DeltaSeconds = InDeltaSeconds;

	const SFlags<EUpdatePhase>& MeshRendererPhases = MeshRenderer->GetPhases();

	if (!IsPhasePaused(EUpdatePhase::Prepare) && (MeshRendererPhases && EUpdatePhase::Prepare))
	{
		MeshRenderer->Prepare(Context);
	}

	RunSystemsPhase(EUpdatePhase::Prepare, Context);

	if (!IsPhasePaused(EUpdatePhase::Update) && (MeshRendererPhases && EUpdatePhase::Update))
	{
		MeshRenderer->Update(Context);
	}

	RunSystemsPhase(EUpdatePhase::Update, Context);

	// The safe phase boundary. Systems record structural changes during Update rather than
	// applying them in place, because destroying an entity or moving a component reorders
	// the very pools they are iterating. Applying here means everything downstream - the
	// Finalize systems below included - sees a settled entity set.
	Commands->Flush(*EcsWorld);

	// NOTE: Sys_MeshRenderer's Finalize runs before the entity tick below, so the
	// acceleration structure it builds trails the transforms by a frame. That predates
	// the ECS and is left alone here rather than silently reordered.
	if (!IsPhasePaused(EUpdatePhase::Finalize) && (MeshRendererPhases && EUpdatePhase::Finalize))
	{
		MeshRenderer->Finalize(Context);
	}

	if (!IsPhasePaused(EUpdatePhase::Update))
	{
		for (auto& entity : Entities)
		{
			entity->Tick(1.f / 60.f);
		}
	}

	// Deliberately last: Finalize exists so a system can observe every Update-phase write,
	// which for Sys_Transform means every local transform touched this frame.
	RunSystemsPhase(EUpdatePhase::Finalize, Context);

	// Object constants used to be filled here, one per CEntity, and indexed in the raster
	// draw loop by render-model slot. The two lined up only because SpawnEntity created a
	// render model for every entity, drawable or not. Sys_MeshRenderer now fills them from
	// its own drawable list - the same one the raster loop and the acceleration structure
	// index into, and the one that includes ECS entities.
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

	for (ISystem* system : Systems)
	{
		if (system->GetPhases() && EUpdatePhase::Draw)
		{
			system->Draw(Context);
		}
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
