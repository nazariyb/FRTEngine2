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

frt::memory::TRefShared<frt::CEntity> frt::CWorldScene::SpawnEntity (FStringView Name)
{
	auto newEntity = memory::NewShared<CEntity>();
	if (Name.IsEmpty())
	{
		newEntity->Name = FString::Format("Entity #{}", Entities.Count());
	}
	else
	{
		newEntity->Name = Name;
	}
	Entities.Add(newEntity);
	newEntity->Id = NextEntityId++;
	newEntity->RenderModel = memory::NewShared<graphics::Comp_RenderModel>();
	return newEntity;
}

void frt::CWorldScene::Update (float GameDeltaSeconds)
{
	SUpdateContext Context;
	Context.DeltaSeconds = GameDeltaSeconds;
	Context.TotalSeconds = Clock.TotalSeconds;
	const bool bSimulate = GameDeltaSeconds > 0.0f;

	if (bSimulate)
	{
		RunSystemsPhase(EUpdatePhase::Prepare, Context);
		RunSystemsPhase(EUpdatePhase::Update, Context);
	}

	// The safe phase boundary. Systems record structural changes during Update rather than
	// applying them in place, because destroying an entity or moving a component reorders
	// the very pools they are iterating. Applying here means everything downstream - the
	// Finalize systems below included - sees a settled entity set.
	Commands->Flush(*EcsWorld);

	if (bSimulate)
	{
		for (auto& entity : Entities)
		{
			entity->Tick(GameDeltaSeconds);
		}
	}

	// Deliberately last: Finalize exists so a system can observe every Update-phase write,
	// which for Sys_Transform means every local transform touched this frame. The renderer
	// is described afterwards (ExtractRenderScene), so it sees this frame's transforms.
	RunSystemsPhase(EUpdatePhase::Finalize, Context);
}

std::unique_ptr<frt::CWorldScene> frt::CWorldScene::Clone () const
{
	frt_assert(Commands->IsEmpty());

	std::unique_ptr<CWorldScene> copy = std::make_unique<CWorldScene>();
	copy->Initialize();
	copy->EcsWorld->CopyFrom(*EcsWorld);

	for (const memory::TRefShared<CEntity>& source : Entities)
	{
		memory::TRefShared<CEntity> entity = memory::NewShared<CEntity>();
		entity->Name = source->Name;
		entity->Id = source->Id;
		entity->Transform = source->Transform;
		entity->RotationSpeed = source->RotationSpeed;
		entity->bRayTraced = source->bRayTraced;
		// A component of the copy's own (extraction writes bVisible and creates portal quads)
		// around the same, immutable model: no re-upload, no new BLAS.
		if (source->RenderModel)
		{
			entity->RenderModel = memory::NewShared<graphics::Comp_RenderModel>(*source->RenderModel);
		}
		if (source->Light)
		{
			entity->Light = memory::NewShared<graphics::Comp_Light>(*source->Light);
		}
		if (source->Portal)
		{
			entity->Portal = memory::NewShared<graphics::Comp_Portal>(*source->Portal);
		}
		copy->Entities.Add(std::move(entity));
	}
	copy->NextEntityId = NextEntityId;

	copy->Camera = Camera;
	copy->Environment = Environment;
	copy->Clock = Clock;
	copy->bAccumulationDirty = bAccumulationDirty;
	return copy;
}

float frt::SWorldClock::Advance (float RealDeltaSeconds)
{
	float delta = 0.0f;
	if (bPaused)
	{
		if (PendingSteps == 0u)
		{
			return 0.0f;
		}
		--PendingSteps;
		delta = StepDeltaSeconds;
	}
	else if (FixedDeltaSeconds > 0.0f)
	{
		delta = FixedDeltaSeconds;
	}
	else
	{
		delta = (RealDeltaSeconds < MaxDeltaSeconds ? RealDeltaSeconds : MaxDeltaSeconds) * TimeScale;
	}
	if (delta <= 0.0f)
	{
		return 0.0f;
	}
	TotalSeconds += delta;
	++SimulatedFrames;
	return delta;
}

void frt::SWorldClock::Step (uint32 Count)
{
	bPaused = true;
	PendingSteps += Count;
}
