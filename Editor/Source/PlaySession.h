#pragma once

#include <cstdio>
#include <exception>
#include <memory>

#include "CoreTypes.h"
#include "Exception.h"


namespace frt
{
class CEngine;
class CGameModule;
class CWorldScene;
class GameInstance;
struct SGameContext;
struct SGameInput;
}


namespace frt::editor
{
// Runs a call into game code. Whatever it throws ends up in the log, never in the editor.
template <typename TCall>
bool CallGame (const char* What, TCall&& Call)
{
	try
	{
		Call();
		return true;
	}
	catch (const ::frt::Exception& e)
	{
		std::printf("[game] %s failed: %s %d : %s\n", What, e.What(), static_cast<int>(e.GetErrorCode()), e.GetErrorDescription().c_str());
	}
	catch (const std::exception& e)
	{
		std::printf("[game] %s failed: %s\n", What, e.what());
	}
	catch (...)
	{
		std::printf("[game] %s failed: unknown exception\n", What);
	}
	return false;
}

/**
 * Play mode (Unity's Play button, Unreal's Play In Editor): the game running inside the editor
 * on a copy of the edited world.
 *
 * Begin duplicates the edit world (CWorldScene::Clone) and creates a fresh game instance; End
 * drops both. Nothing a session does can reach the edit world, so there is no restore step to
 * get wrong, and every session starts from the same state - including the game's own, since
 * its instance is new each time (the role of Unity's domain reload).
 *
 * The session's game time is the copy's SWorldClock: pausing or stepping it stops or advances
 * the game alone, while the editor, its cameras and rendering run on. A game that throws ends
 * its session instead of taking the editor down.
 */
class CPlaySession
{
public:
	CPlaySession (CEngine& Engine, const CGameModule& Module);
	~CPlaySession ();
	CPlaySession (const CPlaySession&) = delete;
	CPlaySession& operator= (const CPlaySession&) = delete;

	// Between frames. False if the game failed to begin (logged; no session runs).
	bool Begin (const CWorldScene& EditWorld, bool bStartPaused);
	void End ();

	bool IsPlaying () const { return Game != nullptr; }
	bool IsPaused () const;
	void SetPaused (bool bPaused);
	// Pauses, and advances the next frame by one fixed game step.
	void Step ();

	// The session's world; null outside play mode.
	CWorldScene* GetWorld () const { return World.get(); }

	// One frame: the world's clock, the game's tick, the world's update. False if the game
	// threw (logged): the session should end.
	bool Tick (float RealDeltaSeconds, const SGameInput& Input);

private:
	SGameContext MakeContext () const;

	CEngine& Engine;
	const CGameModule& Module;
	std::unique_ptr<CWorldScene> World;
	std::unique_ptr<GameInstance> Game;
};
}
