#pragma once

#include "System.h"
#include "Containers/Array.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt
{
class GameInstance;
class Sys_MeshRenderer;
class CEntity;
class ISystem;
class CWorld;
class CCommandBuffer;


class CWorldScene
{
public:
	CWorldScene () = delete;
	CWorldScene (GameInstance& InGame);
	~CWorldScene ();

	CWorldScene (const CWorldScene&) = delete;
	CWorldScene& operator= (const CWorldScene&) = delete;

	bool Initialize ();

	FRT_CORE_API memory::TRefShared<CEntity> SpawnEntity (const std::string& Name = "");

	void RunFrame (float InDeltaSeconds);
	void SubmitFrame (ID3D12GraphicsCommandList4* CommandList);

	bool IsPhasePaused (EUpdatePhase Phase) const;
	void PausePhases (SFlags<EUpdatePhase> Phases);
	void UnpausePhases (SFlags<EUpdatePhase> Phases);
	bool TogglePhasePause (EUpdatePhase Phase);

	const TArray<memory::TRefShared<CEntity>>& GetEntities () const { return Entities; }
	TArray<memory::TRefShared<CEntity>>& GetEntities () { return Entities; }

	/**
	 * ECS world, created in Initialize() rather than in the constructor: CWorldScene is a
	 * member of GameInstance and so is constructed before GameInstance's body sets up the
	 * memory pool, and CWorld allocates its pools up front.
	 *
	 * Runs alongside the CEntity list during the migration - the two do not interact yet.
	 */
	FRT_CORE_API CWorld& GetEcsWorld ();
	FRT_CORE_API const CWorld& GetEcsWorld () const;

	/**
	 * Shared buffer for structural changes recorded by systems.
	 *
	 * Applied once per frame at the end of the Update phase, so everything downstream -
	 * Sys_Transform in Finalize included - sees the resulting entity set rather than one
	 * that changes underneath it.
	 */
	CCommandBuffer& GetCommands ();

	memory::TRefUnique<Sys_MeshRenderer> MeshRenderer;

	// Scene-level dirty flags consumed by Sys_MeshRenderer each frame.
	// Set here because topology and motion are scene knowledge, not renderer knowledge.
	bool bSceneTopologyDirty = false;
	bool bAccumulationDirty = false;


private:
	/** Dispatches one phase across Systems. Draw is handled separately - it needs the command list. */
	void RunSystemsPhase (EUpdatePhase Phase, const SUpdateContext& Context);

	TArray<memory::TRefShared<CEntity>> Entities; // TODO: allocate on stack

	memory::TRefUnique<CWorld> EcsWorld;
	memory::TRefUnique<CCommandBuffer> Commands;

	// Raw pointers because TRefUnique has no derived-to-base conversion, so it cannot
	// hold an ISystem for a concrete system type. Owned here and destroyed through the
	// virtual destructor in ~CWorldScene.
	TArray<ISystem*> Systems;

	SFlags<EUpdatePhase> PausedPhases;
	GameInstance& Game;
};
}
