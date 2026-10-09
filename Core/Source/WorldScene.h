#pragma once

#include <memory>

#include "System.h"
#include "Containers/Array.h"
#include "Containers/StringView.h"
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
 * A world's game time (Gregory, GEA "Abstract Timelines"): separate from the engine's real-time
 * clock, so pausing, stepping or slowing a game leaves the UI, the editor's cameras and
 * rendering running at full rate.
 */
struct FRT_CORE_API SWorldClock
{
	float TimeScale = 1.0f;
	float MaxDeltaSeconds = 0.1f;            // a hitch (a debugger break) must not become one huge step
	float FixedDeltaSeconds = 0.0f;          // > 0: every simulated frame advances exactly this (scripted runs)
	float StepDeltaSeconds = 1.0f / 60.0f;   // what one Step() advances while paused
	bool bPaused = false;
	uint32 PendingSteps = 0u;

	double TotalSeconds = 0.0;
	uint64 SimulatedFrames = 0u;

	// This frame's game delta, and advances the clock: 0 while paused (unless a step is pending).
	float Advance (float RealDeltaSeconds);
	// Pauses, and advances the next Count frames by StepDeltaSeconds each.
	void Step (uint32 Count = 1u);
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

	/**
	 * A deep copy, as a new initialised world: the editor plays a copy of its edit world and
	 * drops it on Stop (Unreal's PIE duplication; no restore step that could forget something).
	 * ECS entities keep their handles, legacy entities their ids; immutable model data is
	 * shared, everything a session can change is copied. Call between frames.
	 */
	std::unique_ptr<CWorldScene> Clone () const;

	// An empty Name gets "Entity #<n>".
	memory::TRefShared<CEntity> SpawnEntity (FStringView Name = {});

	/**
	 * One frame. Simulation - the Prepare and Update phases, the legacy entity tick - runs only
	 * when GameDeltaSeconds > 0 (Clock.Advance's result); Finalize (transform propagation) runs
	 * every frame, so edits to a paused or edit-mode world still show.
	 */
	void Update (float GameDeltaSeconds);

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
	SWorldClock Clock;

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

	uint32 NextEntityId = 1u;
};
}
