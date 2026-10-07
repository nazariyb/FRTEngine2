#include "EditorSmokeTest.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "EnginePaths.h"
#include "Timer.h"
#include "Window.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "Entity.h"
#include "WorldScene.h"
#include "Graphics/Render/Renderer.h"

#include "Editor.h"


namespace frt::editor
{
namespace
{
// Everything a play session could plausibly change: entities, camera, environment, clock.
uint64 HashWorld (const CWorldScene& World)
{
	uint64 hash = 14695981039346656037ull;   // FNV-1a
	const auto mix = [&hash] (const void* Data, size_t Size)
	{
		for (size_t i = 0u; i < Size; ++i)
		{
			hash = (hash ^ static_cast<const uint8*>(Data)[i]) * 1099511628211ull;
		}
	};
	const auto mixTransform = [&mix] (const math::STransform& Transform)
	{
		mix(&Transform.GetTranslation(), sizeof(Vector3f));
		mix(&Transform.GetRotation(), sizeof(Vector3f));
		mix(&Transform.GetScale(), sizeof(Vector3f));
	};
	for (const memory::TRefShared<CEntity>& entity : World.GetEntities())
	{
		mix(&entity->Id, sizeof(entity->Id));
		mix(entity->Name.data(), entity->Name.size());
		mixTransform(entity->Transform);
	}
	mixTransform(World.Camera.Transform);
	mix(&World.Environment.TimeOfDay, sizeof(World.Environment.TimeOfDay));
	mix(&World.Clock.TotalSeconds, sizeof(World.Clock.TotalSeconds));
	mix(&World.Clock.SimulatedFrames, sizeof(World.Clock.SimulatedFrames));
	return hash;
}
}

CEditorSmokeTest::CEditorSmokeTest (const config::CConfig& Config)
{
	const std::string dir = Config.Get("Editor", "SmokeTestDir", "");
	OutputDir = dir.empty() ? paths::GetLocalDir() / "Editor" / "SmokeTest" : std::filesystem::path(dir);
	std::error_code ec;
	std::filesystem::create_directories(OutputDir, ec);
	std::ofstream(OutputDir / "SmokeTest.log", std::ios::trunc);
}

void CEditorSmokeTest::Update (CEditor& Editor, CEngine& Engine)
{
	if (StartFrame == 0u)
	{
		StartFrame = Engine.GetFrameIndex();
	}
	const uint64 frame = Engine.GetFrameIndex() - StartFrame;

	// Each step runs on its frame; commands it queues apply on the next one. The gaps leave
	// time for resizes to settle and views to render before anything is checked or captured.
	struct SStep
	{
		uint32 Frame;
		const char* Name;
		void (*Run) (CEditorSmokeTest& Test, CEditor& Editor, CEngine& Engine);
	};
	static const SStep steps[] =
	{
		{ 30u, "docked", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(E.GetViewport(EViewportTarget::Scene).GetFrame().bVisible, "the Scene view is visible");
			T.Check(!E.GetViewport(EViewportTarget::Game).GetFrame().bVisible, "the Game view is a background tab");
			T.Capture(G, "01_docked");
			E.Execute(EEditorCommand::ToggleMaximize, EViewportTarget::Game);
		} },
		// Transitions are captured on their first frame too: a glitch lasts exactly one.
		{ 31u, "maximize, first frame", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.Capture(G, "02_maximize_first_frame"); } },
		{ 60u, "maximized", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(E.GetMaximizedViewport() == &E.GetViewport(EViewportTarget::Game), "the Game view is maximized");
			T.Check(E.GetViewport(EViewportTarget::Game).GetFrame().bVisible, "the maximized Game view is visible");
			T.Check(!E.GetViewport(EViewportTarget::Scene).GetFrame().bVisible, "the covered Scene view is not rendered");
			T.Capture(G, "03_maximized");
			E.Execute(EEditorCommand::ToggleMaximize, EViewportTarget::Game);
		} },
		{ 61u, "restore, first frame", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.Capture(G, "04_restore_first_frame"); } },
		{ 80u, "restored", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.Check(E.GetMaximizedViewport() == nullptr, "nothing is maximized");
			T.Check(E.GetViewport(EViewportTarget::Game).GetFrame().bVisible, "the Game tab is selected");
			E.Execute(EEditorCommand::ToggleFullscreen, EViewportTarget::Scene);
		} },
		{ 82u, "fullscreen, first frame", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.Capture(G, "05_fullscreen_first_frame"); } },
		{ 120u, "fullscreen", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(E.IsFullscreen() && G.GetWindow()->IsImmersive(), "the Scene view is fullscreen");
			const graphics::CRenderView& view = E.GetViewport(EViewportTarget::Scene).GetView();
			const graphics::CRenderer& renderer = *G.GetRenderer();
			T.Check(view.GetDisplayWidth() == renderer.GetBackBufferWidth() && view.GetDisplayHeight() == renderer.GetBackBufferHeight(),
				"the fullscreen view covers the whole back buffer");
			T.Capture(G, "06_fullscreen");
			E.Execute(EEditorCommand::ToggleFullscreen);
		} },
		{ 121u, "windowed, first frame", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.Capture(G, "07_windowed_first_frame"); } },
		{ 150u, "windowed", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(!E.IsFullscreen() && !G.GetWindow()->IsImmersive(), "the window is back to normal");
			T.Check(E.GetViewport(EViewportTarget::Scene).GetFrame().bVisible, "the Scene tab is selected again");
			T.Capture(G, "08_windowed");
			T.EditWorldHash = HashWorld(E.GetEditWorld());
			E.Execute(EEditorCommand::Play);
		} },
		{ 151u, "play, first frame", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.Capture(G, "09_play_first_frame"); } },
		{ 190u, "playing", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			const CPlaySession& play = E.GetPlaySession();
			T.Check(play.IsPlaying() && !play.IsPaused(), "playing");
			T.Check(&E.GetActiveWorld() == play.GetWorld() && play.GetWorld() != &E.GetEditWorld(), "the viewports show the session's copy");
			T.Check(E.GetViewport(EViewportTarget::Game).GetFrame().bVisible && E.GetViewport(EViewportTarget::Game).GetFrame().bFocused,
				"the Game view came forward with focus");
			T.Check(play.GetWorld()->Clock.SimulatedFrames > 0u, "the game simulates");
			T.PlayWorldHash = HashWorld(*play.GetWorld());
			T.Check(T.PlayWorldHash != T.EditWorldHash, "the session's world diverged from the edit world");
			T.Check(HashWorld(E.GetEditWorld()) == T.EditWorldHash, "the edit world is untouched while playing");
			T.Capture(G, "10_playing");
			E.Execute(EEditorCommand::TogglePause);
		} },
		{ 200u, "paused", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			const CPlaySession& play = E.GetPlaySession();
			T.Check(play.IsPlaying() && play.IsPaused(), "paused");
			T.PausedFrames = play.GetWorld()->Clock.SimulatedFrames;
			E.Execute(EEditorCommand::Step);
		} },
		{ 210u, "stepped", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			const CPlaySession& play = E.GetPlaySession();
			T.Check(play.IsPaused() && play.GetWorld()->Clock.SimulatedFrames == T.PausedFrames + 1u, "Step advanced exactly one frame");
			T.Capture(G, "11_paused");
			E.Execute(EEditorCommand::Stop);
		} },
		{ 211u, "stop, first frame", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.Capture(G, "12_stop_first_frame"); } },
		{ 240u, "stopped", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(!E.GetPlaySession().IsPlaying() && &E.GetActiveWorld() == &E.GetEditWorld(), "back in edit mode");
			T.Check(HashWorld(E.GetEditWorld()) == T.EditWorldHash, "Stop left the edit world exactly as Play found it");
			T.Capture(G, "13_stopped");
		} },
	};

	// Hitches: the longest frame between two steps goes into the log with the next step.
	LongestFrameMs = (std::max)(LongestFrameMs, Engine.GetClock().GetDeltaSeconds() * 1000.0f);
	while (NextStep < std::size(steps) && frame >= steps[NextStep].Frame)
	{
		const SStep& step = steps[NextStep++];
		char line[160];
		std::snprintf(line, sizeof(line), "step: %s (frame %llu, longest frame since the last step %.1f ms)",
			step.Name, static_cast<unsigned long long>(frame), LongestFrameMs);
		Log(line);
		LongestFrameMs = 0.0f;
		step.Run(*this, Editor, Engine);
	}

	// Done: quit once the captures are on disk.
	if (NextStep == std::size(steps) && !bFinished && !Engine.GetRenderer()->HasPendingCaptures())
	{
		bFinished = true;
		Log(Failures == 0u ? "PASSED" : "FAILED: " + std::to_string(Failures) + " check(s)");
		Engine.RequestQuit(Failures == 0u ? 0 : 2);
	}
}

void CEditorSmokeTest::Check (bool bPassed, const char* What)
{
	Log(std::string(bPassed ? "  ok    " : "  FAIL  ") + What);
	if (!bPassed)
	{
		++Failures;
	}
}

void CEditorSmokeTest::Capture (CEngine& Engine, const char* Name)
{
	Engine.GetRenderer()->RequestBackBufferCapture(OutputDir / Name);
	Log(std::string("  capture ") + Name);
}

void CEditorSmokeTest::Log (const std::string& Line)
{
	std::printf("[smoke] %s\n", Line.c_str());
	std::fflush(stdout);
	std::ofstream(OutputDir / "SmokeTest.log", std::ios::app) << Line << '\n';
}
}
