#include "PlaySession.h"

#include <cstdio>

#include "GameInstance.h"
#include "WorldScene.h"
#include "Engine/Engine.h"
#include "Engine/GameModule.h"


namespace frt::editor
{
CPlaySession::CPlaySession (CEngine& InEngine, const CGameModule& InModule)
	: Engine(InEngine)
	, Module(InModule)
{
}

CPlaySession::~CPlaySession ()
{
	End();
}

SGameContext CPlaySession::MakeContext () const
{
	return { *World, Engine.GetMaterials(), Engine.GetInputActions(), Engine.GetConfig() };
}

bool CPlaySession::Begin (const CWorldScene& EditWorld, bool bStartPaused)
{
	frt_assert(!IsPlaying());
	const bool bCreated = CallGame("Begin", [&]
	{
		World = EditWorld.Clone();
		World->Clock.bPaused = bStartPaused;
		World->Clock.PendingSteps = 0u;
		Game = Module.CreateGame();   // null if the game's constructor threw
	});
	if (!bCreated || !Game || !CallGame("BeginPlay", [&] { Game->BeginPlay(MakeContext()); }))
	{
		Game.reset();
		World.reset();
		return false;
	}
	std::printf("[play] started%s\n", bStartPaused ? " (paused)" : "");
	return true;
}

void CPlaySession::End ()
{
	if (!IsPlaying())
	{
		return;
	}
	const SGameContext context = MakeContext();
	CallGame("EndPlay", [&] { Game->EndPlay(context); });
	// The instance first: it may still refer to the world.
	Game.reset();
	World.reset();
	std::printf("[play] stopped\n");
}

bool CPlaySession::IsPaused () const
{
	return World && World->Clock.bPaused;
}

void CPlaySession::SetPaused (bool bPaused)
{
	if (World)
	{
		World->Clock.bPaused = bPaused;
		World->Clock.PendingSteps = 0u;
	}
}

void CPlaySession::Step ()
{
	if (World)
	{
		World->Clock.Step(1u);
	}
}

bool CPlaySession::Tick (float RealDeltaSeconds, const SGameInput& Input)
{
	frt_assert(IsPlaying());
	SGameFrame frame;
	frame.RealDeltaSeconds = RealDeltaSeconds;
	frame.GameDeltaSeconds = World->Clock.Advance(RealDeltaSeconds);
	frame.Input = Input;

	// Ticked while paused too, with no game time: like the launcher, the game decides what
	// still moves on real time (the demo's free camera).
	const SGameContext context = MakeContext();
	return CallGame("Tick", [&]
	{
		Game->Tick(context, frame);
		World->Update(frame.GameDeltaSeconds);
	});
}
}
