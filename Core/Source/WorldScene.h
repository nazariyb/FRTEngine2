#pragma once

#include "System.h"
#include "Containers/Array.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt
{
class CEntity;
class ISystem;
class CWorld;
class CCommandBuffer;


// Lighting environment of a world: sky, sun and the time of day that can drive them.
struct SWorldEnvironment
{
	graphics::SSkyConstants Sky;
	float TimeOfDay = 0.5f;               // [0,1]; 0.5 = noon
	bool bUseTimeOfDay = true;            // Sky follows TimeOfDay (graphics::ComputeSkyFromTimeOfDay)
	float SunAngularRadius = 0.004654f;   // radians; the real sun is ~0.267 deg
};


/**
 * A world: everything a game simulates and the renderer shows - entities (the legacy list and
 * the ECS), the systems that update them, the game camera and the lighting environment. Holds
 * no reference to the game, the engine or the renderer, so several can coexist (the editor's
 * edit world and its play copy) and the renderer is fed by ExtractRenderScene.
 */
class FRT_CORE_API CWorldScene
{
public:
	CWorldScene ();
	~CWorldScene ();

	CWorldScene (const CWorldScene&) = delete;
	CWorldScene& operator= (const CWorldScene&) = delete;

	bool Initialize ();

	memory::TRefShared<CEntity> SpawnEntity (const std::string& Name = "");

	void RunFrame (float InDeltaSeconds);

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
	CWorld& GetEcsWorld ();
	const CWorld& GetEcsWorld () const;

	/**
	 * Shared buffer for structural changes recorded by systems.
	 *
	 * Applied once per frame at the end of the Update phase, so everything downstream -
	 * Sys_Transform in Finalize included - sees the resulting entity set rather than one
	 * that changes underneath it.
	 */
	CCommandBuffer& GetCommands ();

	// The game's point of view. Part of the world, so a play session's camera moves revert
	// with everything else when it ends.
	graphics::CCamera Camera;
	SWorldEnvironment Environment;

	// Set by anything that changes what the world looks like outside the renderer's own change
	// tracking (e.g. a settings panel); the frame's owner resets the renderer's accumulation.
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
};
}
