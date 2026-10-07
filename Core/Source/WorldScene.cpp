#include "WorldScene.h"

#include "Entity.h"
#include "ECS/CommandBuffer.h"
#include "ECS/CoreComponents.h"
#include "ECS/Sys_Lifetime.h"
#include "ECS/Sys_Transform.h"
#include "ECS/World.h"

frt::CWorldScene::CWorldScene () = default;

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
	newEntity->RenderModel = memory::NewShared<graphics::Comp_RenderModel>();
	return newEntity;
}

void frt::CWorldScene::RunFrame (float InDeltaSeconds)
{
	SUpdateContext Context;
	Context.DeltaSeconds = InDeltaSeconds;

	RunSystemsPhase(EUpdatePhase::Prepare, Context);
	RunSystemsPhase(EUpdatePhase::Update, Context);

	// The safe phase boundary. Systems record structural changes during Update rather than
	// applying them in place, because destroying an entity or moving a component reorders
	// the very pools they are iterating. Applying here means everything downstream - the
	// Finalize systems below included - sees a settled entity set.
	Commands->Flush(*EcsWorld);

	if (!IsPhasePaused(EUpdatePhase::Update))
	{
		for (auto& entity : Entities)
		{
			entity->Tick(1.f / 60.f);
		}
	}

	// Deliberately last: Finalize exists so a system can observe every Update-phase write,
	// which for Sys_Transform means every local transform touched this frame. The renderer
	// is described afterwards (ExtractRenderScene), so it sees this frame's transforms.
	RunSystemsPhase(EUpdatePhase::Finalize, Context);
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
