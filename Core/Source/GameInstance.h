#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Math/Math.h"


namespace frt
{
class CWorldScene;
}

namespace frt::config
{
class CConfig;
}

namespace frt::graphics
{
class CMaterialLibrary;
}

namespace frt::input
{
class CInputActionLibrary;
class CInputSystem;
}


namespace frt
{
// What a game may use while it runs. Everything else - window, renderer, UI - belongs to the
// host (the launcher, the editor), never to the game.
struct SGameContext
{
	CWorldScene& World;
	graphics::CMaterialLibrary& Materials;
	input::CInputActionLibrary& InputActions;
	const config::CConfig& Config;
};

// The game's input for one frame, as data (DOOM's ticcmd): the host decides what the game may
// see - in the editor only a focused Game view routes input to it.
struct SGameInput
{
	const input::CInputSystem* Device = nullptr;   // null: the game has no input this frame
	Vector2f LookDelta = Vector2f(0.0f, 0.0f);     // mouse motion, pixels
	float ViewAspect = 1.0f;                       // of the view the game is shown in
};

struct SGameFrame
{
	float RealDeltaSeconds = 0.0f;   // wall clock: cameras, UI-like feedback
	float GameDeltaSeconds = 0.0f;   // the world's clock: 0 while paused
	SGameInput Input;
};

/**
 * A game, as a service the host calls (Muratori's platform/game split; DOOM 3's game DLL).
 *
 * The host owns the world, the loop and the frame. A game builds the world's content, then -
 * for each play session - is told it began, ticked once per frame and told it ended. Game
 * state belongs in the world: the editor plays a copy of its edit world and drops the copy on
 * Stop, and creates a fresh instance for every session (so nothing a session leaves in the
 * instance leaks into the next, the role of Unity's domain reload).
 *
 * Games are modules: a game DLL exposes its instance through FRT_IMPLEMENT_GAME_MODULE
 * (Engine/GameModule.h), which both hosts load.
 */
class FRT_CORE_API GameInstance
{
public:
	virtual ~GameInstance ();

	// Authors the world's content (entities, camera, environment) into an empty world.
	virtual void BuildWorld (const SGameContext& Context) = 0;

	virtual void BeginPlay (const SGameContext& Context) {}
	virtual void Tick (const SGameContext& Context, const SGameFrame& Frame) {}
	virtual void EndPlay (const SGameContext& Context) {}
};
}
