#include "EditorSmokeTest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "EnginePaths.h"
#include "Timer.h"
#include "Window.h"
#include "Config/Config.h"
#include "Containers/String.h"
#include "Engine/Engine.h"
#include "Engine/GameModule.h"
#include "Entity.h"
#include "WorldScene.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/Renderer.h"
#include "Platform/Platform.h"
#include "UI/DebugTools.h"

#include "imgui.h"

#include "Editor.h"
#include "ViewportCamera.h"


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
		mix(entity->Name.GetData(), entity->Name.Len());
		mixTransform(entity->Transform);
	}
	mixTransform(World.Camera.Transform);
	mix(&World.Environment.TimeOfDay, sizeof(World.Environment.TimeOfDay));
	mix(&World.Clock.TotalSeconds, sizeof(World.Clock.TotalSeconds));
	mix(&World.Clock.SimulatedFrames, sizeof(World.Clock.SimulatedFrames));
	return hash;
}

// A key press or release through the window's message queue, as Windows would deliver it: both
// the engine's input and ImGui see it.
void PostKey (CEngine& Engine, uint32 VirtualKey, uint32 ScanCode, bool bExtended, bool bDown)
{
	LPARAM lParam = 1 | (static_cast<LPARAM>(ScanCode) << 16) | (bExtended ? (LPARAM(1) << 24) : 0);
	if (!bDown)
	{
		lParam |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
	}
	::PostMessageW(Engine.GetWindow()->GetHandle(), bDown ? WM_KEYDOWN : WM_KEYUP, VirtualKey, lParam);
}

void PostCtrlP (CEngine& Engine, bool bDown)
{
	if (bDown)
	{
		PostKey(Engine, VK_CONTROL, 0x1D, false, true);
		PostKey(Engine, 'P', 0x19, false, true);
	}
	else
	{
		PostKey(Engine, 'P', 0x19, false, false);
		PostKey(Engine, VK_CONTROL, 0x1D, false, false);
	}
}

float Distance (const Vector3f& A, const Vector3f& B)
{
	const Vector3f d = A - B;
	return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

bool Near (float A, float B, float Tolerance = 1e-4f)
{
	return std::abs(A - B) <= Tolerance;
}
}

CEditorSmokeTest::CEditorSmokeTest (const config::CConfig& Config)
{
	const FString dir = Config.Get("Editor", "SmokeTestDir", "");
	// Text that is not UTF-8 (an INI saved in another code page) names no directory. Such a run
	// fails in the default one: an empty path would put the log, the captures and the input
	// overrides in the working directory, and the run would pass.
	const std::optional<std::filesystem::path> configuredDir = dir.IsEmpty() ? std::nullopt : dir.TryToPath();
	OutputDir = configuredDir.value_or(paths::GetLocalDir() / "SmokeTest");
	std::error_code ec;
	std::filesystem::create_directories(OutputDir, ec);
	std::ofstream(OutputDir / "SmokeTest.log", std::ios::trunc);
	// The run's input overrides start empty: it tests the shipped bindings.
	std::filesystem::remove(OutputDir / "EditorInput.ini", ec);
	if (!dir.IsEmpty() && !configuredDir)
	{
		Check(false, FString::Format("[Editor] SmokeTestDir \"{}\" is not UTF-8; writing to {}",
			FString::FromUtf8Lossy(dir), FString::FromPath(OutputDir)));
	}
}

void CEditorSmokeTest::WriteInputOverrides (FStringView Text)
{
	platform::WriteFileText(OutputDir / "EditorInput.ini", Text);
}

void CEditorSmokeTest::TouchGameBuild (CEditor& Editor)
{
	// What a build of the game module looks like to the editor, short of compiling one: a
	// newer write time on the DLL it watches. The editor looks at it on the next frame.
	const std::filesystem::path build(Editor.GetGameModule().GetBuildPath());
	std::error_code ec;
	bCanReload = !build.empty() && std::filesystem::exists(build, ec);
	if (!bCanReload)
	{
		Log("  skip  hot reload: this build loads no game module DLL it could watch");
		return;
	}
	std::filesystem::last_write_time(build, std::filesystem::file_time_type::clock::now(), ec);
	Check(!ec, "the game module's DLL takes a newer write time");
	Editor.PollGameModuleNow();
}

void CEditorSmokeTest::CheckCameraController ()
{
	// Unreal's numbers, in metres and radians: 0.2 degrees and 1 cm per mouse count at speed 1,
	// 0.96 m per wheel notch, the pivot 10.24 m ahead. Engine axes: +X left, +Y up, +Z forward;
	// +yaw turns left, +pitch looks down.
	constexpr float Degree = 0.017453292519943295f;
	const auto fresh = [] (graphics::CCamera& Camera, float Pitch = 0.0f)
	{
		Camera.Transform.SetTranslation(0.0f, 0.0f, 0.0f);
		Camera.Transform.SetRotation(Pitch, 0.0f, 0.0f);
		Camera.FieldOfViewY = 1.0f;
	};
	const auto drag = [] (uint8 Buttons, float Dx, float Dy, bool bAlt = false, bool bCtrl = false)
	{
		SViewportNavigationInput input;
		input.Buttons = Buttons;
		input.MouseDelta = Vector2f(Dx, Dy);
		input.bAlt = bAlt;
		input.bCtrl = bCtrl;
		return input;
	};
	const float dt = 1.0f / 60.0f;

	{
		graphics::CCamera camera;
		fresh(camera);
		CViewportCameraController controller;
		controller.Update(camera, drag(viewport_button::Right, 100.0f, -50.0f), dt);
		Check(Near(camera.Transform.GetRotation().y, -20.0f * Degree) && Near(camera.Transform.GetRotation().x, -10.0f * Degree),
			"RMB: 100 counts right turns 20 degrees right, 50 up looks 10 degrees up");
		Check(Distance(camera.Transform.GetTranslation(), Vector3f(0.0f, 0.0f, 0.0f)) == 0.0f, "RMB look does not move");
		controller.Update(camera, drag(viewport_button::Right, 0.0f, -100000.0f), dt);
		Check(camera.Transform.GetRotation().x > -1.5708f && camera.Transform.GetRotation().x < -1.5f, "pitch stops short of straight up");
	}
	{
		graphics::CCamera camera;
		fresh(camera, -0.5f);
		CViewportCameraController controller;
		controller.Update(camera, drag(viewport_button::Left, 30.0f, -100.0f), dt);
		const Vector3f p = camera.Transform.GetTranslation();
		Check(Near(p.z, 1.0f) && Near(p.y, 0.0f) && Near(p.x, 0.0f), "LMB: 100 counts up moves 1 m along the ground, pitch ignored");
		Check(Near(camera.Transform.GetRotation().y, -6.0f * Degree) && Near(camera.Transform.GetRotation().x, -0.5f), "LMB turns, never pitches");

		controller.Update(camera, drag(viewport_button::Left, 50.0f, -100.0f, false, true), dt);
		Check(Near(camera.Transform.GetTranslation().z, p.z) && Near(camera.Transform.GetRotation().y, -6.0f * Degree), "Ctrl suspends mouse navigation, turning too");
	}
	{
		graphics::CCamera camera;
		fresh(camera, -0.5f);
		CViewportCameraController controller;
		controller.Update(camera, drag(viewport_button::Left | viewport_button::Right, 100.0f, -100.0f), dt);
		const Vector3f p = camera.Transform.GetTranslation();
		Check(Near(p.x, -1.0f) && Near(p.y, 1.0f) && Near(p.z, 0.0f), "LMB+RMB: pans 1 m right and 1 m up the world");

		// Pitched up 0.5 rad, the camera's up leans back: MMB pans along it, also with RMB.
		const Vector3f before = camera.Transform.GetTranslation();
		const Vector3f rotationBefore = camera.Transform.GetRotation();
		controller.Update(camera, drag(viewport_button::Right | viewport_button::Middle, 0.0f, -100.0f), dt);
		const Vector3f moved = camera.Transform.GetTranslation() - before;
		Check(Near(moved.y, std::cos(0.5f)) && Near(moved.z, -std::sin(0.5f)) && Near(camera.Transform.GetRotation().x, rotationBefore.x),
			"RMB+MMB pans along the camera's up, as MMB does");
	}
	{
		graphics::CCamera camera;
		fresh(camera);
		CViewportCameraController controller;
		controller.Speed = 5.0f;
		SViewportNavigationInput wheel;
		wheel.Wheel = 1.0f;
		controller.Update(camera, wheel, dt);
		Check(Near(camera.Transform.GetTranslation().z, 0.96f), "wheel: 0.96 m forward per notch, whatever the speed");

		SViewportNavigationInput speed = drag(viewport_button::Right, 0.0f, 0.0f);
		speed.Wheel = 2.0f;
		controller.Update(camera, speed, dt);
		Check(Near(controller.Speed, 5.0f * 1.1f * 1.1f, 1e-3f), "wheel during a drag: speed x1.1 per notch");
	}
	{
		graphics::CCamera camera;
		fresh(camera);
		CViewportCameraController controller;
		controller.Update(camera, SViewportNavigationInput(), dt);
		const Vector3f pivot = controller.GetLookAt();
		Check(Near(Distance(pivot, Vector3f(0.0f, 0.0f, 10.24f)), 0.0f), "the pivot starts 10.24 m ahead");

		controller.Update(camera, drag(viewport_button::Left, 50.0f, -30.0f, true), dt);
		Check(Near(Distance(camera.Transform.GetTranslation(), pivot), 10.24f, 1e-3f), "Alt+LMB orbits at the same distance");
		Check(Near(camera.Transform.GetRotation().y, -10.0f * Degree), "Alt+LMB: 50 counts orbit 10 degrees");
		const Vector3f toPivot = pivot - camera.Transform.GetTranslation();
		const Vector3f forward = camera.GetLookDirection();
		Check(Near((toPivot.x * forward.x + toPivot.y * forward.y + toPivot.z * forward.z) / 10.24f, 1.0f, 1e-3f), "... still looking at the pivot");

		controller.Update(camera, drag(viewport_button::Right, 100.0f, 0.0f, true), dt);
		Check(Near(Distance(camera.Transform.GetTranslation(), pivot), 9.24f, 1e-3f), "Alt+RMB: 100 counts right dollies 1 m in");
		controller.Update(camera, drag(viewport_button::Right, 100000.0f, 0.0f, true), dt);
		Check(Distance(camera.Transform.GetTranslation(), pivot) > 0.0f, "... and never through the pivot");

		controller.Update(camera, drag(viewport_button::Right, -200.0f, 0.0f, true), dt);
		const float orbitDistance = Distance(camera.Transform.GetTranslation(), pivot);
		SViewportNavigationInput orbitWheel = drag(viewport_button::Left, 0.0f, 0.0f, true);
		orbitWheel.Wheel = -1.0f;
		controller.Update(camera, orbitWheel, dt);
		Check(Near(Distance(camera.Transform.GetTranslation(), pivot), orbitDistance + 0.96f, 1e-3f) && Near(Distance(controller.GetLookAt(), pivot), 0.0f),
			"the wheel during an orbit dollies away from the same pivot");

		const Vector3f before = camera.Transform.GetTranslation();
		const Vector3f offset = controller.GetLookAt() - before;
		controller.Update(camera, drag(viewport_button::Middle, 40.0f, 25.0f, true), dt);
		const Vector3f offsetAfter = controller.GetLookAt() - camera.Transform.GetTranslation();
		Check(Distance(camera.Transform.GetTranslation(), before) > 0.4f && Near(Distance(offset, offsetAfter), 0.0f, 1e-4f), "Alt+MMB tracks camera and pivot together");
	}
	{
		graphics::CCamera camera;
		fresh(camera);
		CViewportCameraController controller;
		SViewportNavigationInput fly = drag(viewport_button::Right, 0.0f, 0.0f);
		fly.Keys[static_cast<size_t>(EViewportKey::Forward)] = true;
		for (int i = 0; i < 120; ++i)
		{
			controller.Update(camera, fly, dt);
		}
		const float speed = Distance(controller.GetVelocity(), Vector3f(0.0f, 0.0f, 0.0f));
		Check(speed > 18.0f && speed < 20.5f, "W while flying accelerates to about 20 m/s at speed 1");
		for (int i = 0; i < 120; ++i)
		{
			controller.Update(camera, SViewportNavigationInput(), dt);
		}
		const Vector3f stopped = camera.Transform.GetTranslation();
		controller.Update(camera, SViewportNavigationInput(), dt);
		Check(Distance(controller.GetVelocity(), Vector3f(0.0f, 0.0f, 0.0f)) == 0.0f && Distance(stopped, camera.Transform.GetTranslation()) == 0.0f,
			"released, it glides to a stop");
	}
	{
		graphics::CCamera camera;
		fresh(camera);
		CViewportCameraController controller;
		SViewportNavigationInput zoom = drag(viewport_button::Right, 0.0f, 0.0f);
		zoom.Keys[static_cast<size_t>(EViewportKey::FovZoomIn)] = true;
		for (int i = 0; i < 30; ++i)
		{
			controller.Update(camera, zoom, dt);
		}
		const float zoomed = camera.FieldOfViewY;
		Check(zoomed < 0.9f, "C while flying narrows the view");
		const SViewportNavigationInput holding = drag(viewport_button::Right, 0.0f, 0.0f);
		for (int i = 0; i < 30; ++i)
		{
			controller.Update(camera, holding, dt);
		}
		Check(camera.FieldOfViewY < 0.9f, "... and stays zoomed while the button is held");
		for (int i = 0; i < 60; ++i)
		{
			controller.Update(camera, SViewportNavigationInput(), dt);
		}
		Check(camera.FieldOfViewY == 1.0f, "... and springs back exactly once released");
	}
	{
		graphics::CCamera camera;
		fresh(camera);
		CViewportCameraController controller;
		controller.Update(camera, SViewportNavigationInput(), dt);
		camera.Transform.SetTranslation(5.0f, 0.0f, 0.0f);
		controller.Update(camera, SViewportNavigationInput(), dt);
		Check(Near(Distance(controller.GetLookAt(), Vector3f(5.0f, 0.0f, 10.24f)), 0.0f), "a camera moved by something else keeps its pivot ahead");
	}
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
		FStringLiteral Name;
		void (*Run) (CEditorSmokeTest& Test, CEditor& Editor, CEngine& Engine);
	};
	static const SStep steps[] =
	{
		{ 2u, "camera controller", [] (CEditorSmokeTest& T, CEditor&, CEngine&) { T.CheckCameraController(); } },
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
			E.Execute(EEditorCommand::ToggleMaximize, EViewportTarget::Game);
		} },
		// Closing a maximized viewport (here as the View menu does) must hand the editor back.
		{ 260u, "close while maximized", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.Check(E.GetMaximizedViewport() == &E.GetViewport(EViewportTarget::Game), "the Game view is maximized");
			E.GetViewport(EViewportTarget::Game).bOpen = false;
		} },
		{ 262u, "closed", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.Check(E.GetMaximizedViewport() == nullptr, "closing the maximized viewport restores the editor");
			T.Check(E.GetViewport(EViewportTarget::Scene).GetFrame().bVisible, "the Scene view shows again");
			E.GetViewport(EViewportTarget::Game).bOpen = true;
			E.Execute(EEditorCommand::ToggleMaximize, EViewportTarget::Scene);
		} },
		// Play while another viewport fills the editor: the Game view takes its place.
		{ 280u, "play over a maximized Scene view", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.Check(E.GetMaximizedViewport() == &E.GetViewport(EViewportTarget::Scene), "the Scene view is maximized");
			E.Execute(EEditorCommand::Play);
		} },
		{ 300u, "playing maximized", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			const SViewportFrame& game = E.GetViewport(EViewportTarget::Game).GetFrame();
			T.Check(E.GetMaximizedViewport() == &E.GetViewport(EViewportTarget::Game), "the Game view took the maximized place");
			T.Check(game.bVisible && game.bFocused, "... visible and focused");
			T.Capture(G, "14_playing_maximized");
			E.Execute(EEditorCommand::Stop);
		} },
		{ 320u, "done", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.Check(!E.GetPlaySession().IsPlaying() && HashWorld(E.GetEditWorld()) == T.EditWorldHash, "stopped, edit world unchanged");
		} },
		// ---- input bindings, end to end ----
		{ 330u, "shipped bindings", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			const CEditorInput& input = E.GetInput();
			T.Check(input.HasDefaults() && input.GetProblems().empty(), "the shipped input config reads without problems");
			T.Check(input.GetShortcutText(EEditorCommand::TogglePlay) == "Ctrl+P" && input.GetShortcutText(EEditorCommand::ExitFullscreen) == "Escape",
				"TogglePlay is Ctrl+P, ExitFullscreen is Escape");
			T.InputGeneration = input.GetGeneration();
			PostCtrlP(G, true);
		} },
		{ 335u, "Ctrl+P", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(E.GetPlaySession().IsPlaying(), "a posted Ctrl+P starts play");
			PostCtrlP(G, false);
		} },
		{ 338u, "Ctrl+P again", [] (CEditorSmokeTest&, CEditor&, CEngine& G) { PostCtrlP(G, true); } },
		{ 343u, "Ctrl+P stopped", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(!E.GetPlaySession().IsPlaying(), "... and stops it");
			PostCtrlP(G, false);
			T.WriteInputOverrides("[Commands]\nTogglePlay=F5\n");
			E.GetInput().PollNow();
		} },
		{ 400u, "user override", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			const CEditorInput& input = E.GetInput();
			T.Check(input.GetGeneration() > T.InputGeneration && input.GetShortcutText(EEditorCommand::TogglePlay) == "F5",
				"an override written while the editor runs is picked up");
			T.InputGeneration = input.GetGeneration();
			PostKey(G, VK_F5, 0x3F, false, true);
		} },
		{ 403u, "F5", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(E.GetPlaySession().IsPlaying(), "the overriding chord plays");
			PostKey(G, VK_F5, 0x3F, false, false);
			PostKey(G, VK_F5, 0x3F, false, true);
		} },
		{ 406u, "F5 again", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(!E.GetPlaySession().IsPlaying(), "... and stops");
			PostKey(G, VK_F5, 0x3F, false, false);
			PostCtrlP(G, true);
		} },
		{ 409u, "Ctrl+P unbound", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(!E.GetPlaySession().IsPlaying(), "the overridden chord no longer plays");
			PostCtrlP(G, false);
			T.WriteInputOverrides("[Commands]\nTogglePlay=Bogus\n[Nonsense]\nX=1\n");
			E.GetInput().PollNow();
		} },
		{ 470u, "bad override", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			const CEditorInput& input = E.GetInput();
			T.Check(input.GetGeneration() > T.InputGeneration && input.GetProblems().size() == 2u && input.GetChords(EEditorCommand::TogglePlay).empty(),
				"an unreadable chord and an unknown section are reported, and the binding is dropped");
			T.InputGeneration = input.GetGeneration();
			std::error_code ec;
			std::filesystem::remove(T.OutputDir / "EditorInput.ini", ec);
			E.GetInput().PollNow();
		} },
		{ 530u, "override removed", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			const CEditorInput& input = E.GetInput();
			T.Check(input.GetGeneration() > T.InputGeneration && input.GetProblems().empty() && input.GetShortcutText(EEditorCommand::TogglePlay) == "Ctrl+P",
				"removing the override restores the shipped binding");
			E.Execute(EEditorCommand::ResetLayout);
		} },
		{ 550u, "keyboard navigation", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			T.Check(E.GetViewport(EViewportTarget::Scene).GetFrame().bFocused, "the Scene view has focus");
			T.CameraStart = E.GetSceneCamera().Transform.GetTranslation();
			PostKey(G, VK_UP, 0x48, true, true);
		} },
		{ 580u, "Up held", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			const Vector3f moved = E.GetSceneCamera().Transform.GetTranslation() - T.CameraStart;
			const Vector3f forward = E.GetSceneCamera().GetLookDirection();
			T.Check(moved.x * forward.x + moved.y * forward.y + moved.z * forward.z > 0.2f, "Up moves the focused Scene camera forward, no mouse button needed");
			T.ReleaseSpeed = Distance(E.GetSceneCameraController().GetVelocity(), Vector3f(0.0f, 0.0f, 0.0f));
			PostKey(G, VK_UP, 0x48, true, false);
		} },
		{ 640u, "Up released", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			const float speed = Distance(E.GetSceneCameraController().GetVelocity(), Vector3f(0.0f, 0.0f, 0.0f));
			T.Check(T.ReleaseSpeed > 1.0f && speed < T.ReleaseSpeed * 0.5f, "released, it slows down");
		} },
		// A second at the very least, even at 250 frames per second.
		{ 900u, "Up stopped", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.Check(Distance(E.GetSceneCameraController().GetVelocity(), Vector3f(0.0f, 0.0f, 0.0f)) == 0.0f, "... and comes to a stop");
		} },
		// ---- game module hot reload: a build stood in for by a newer write time ----
		{ 910u, "rebuild in edit mode", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			T.GameReloads = E.GetGameReloads();
			T.EditEntities = E.GetEditWorld().GetEntities().Count();
			T.TouchGameBuild(E);
		} },
		{ 915u, "reloaded in edit mode", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			if (!T.bCanReload)
			{
				return;
			}
			T.Check(E.GetGameReloads() == T.GameReloads + 1u && E.GetGameModule().IsLoaded(), "a newer build of the game module is loaded");
			T.Check(E.GetEditWorld().GetEntities().Count() == T.EditEntities, "... and has built the edit world again");
			T.Capture(G, "15_reloaded");
			E.Execute(EEditorCommand::Play);
		} },
		{ 945u, "rebuild while playing", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			if (!T.bCanReload)
			{
				return;
			}
			const CPlaySession& play = E.GetPlaySession();
			T.Check(play.IsPlaying() && play.GetWorld()->Clock.SimulatedFrames > 0u, "the reloaded game plays");
			T.GameReloads = E.GetGameReloads();
			T.TouchGameBuild(E);
		} },
		{ 950u, "reloaded while playing", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			if (!T.bCanReload)
			{
				return;
			}
			T.Check(E.GetGameReloads() == T.GameReloads + 1u, "a newer build is loaded during play");
			T.Check(!E.GetPlaySession().IsPlaying() && &E.GetActiveWorld() == &E.GetEditWorld(), "... which stops play");
			T.Check(E.GetEditWorld().GetEntities().Count() == T.EditEntities, "... and builds the edit world again");
			T.Capture(G, "16_reloaded_from_play");
			E.Execute(EEditorCommand::Play);
		} },
		{ 980u, "playing after a reload", [] (CEditorSmokeTest& T, CEditor& E, CEngine& G)
		{
			if (!T.bCanReload)
			{
				return;
			}
			const CPlaySession& play = E.GetPlaySession();
			T.Check(play.IsPlaying() && play.GetWorld()->Clock.SimulatedFrames > 0u, "play starts again on the new build");
			T.Capture(G, "17_playing_after_reload");
			E.Execute(EEditorCommand::Stop);
		} },
		{ 1000u, "stopped after a reload", [] (CEditorSmokeTest& T, CEditor& E, CEngine&)
		{
			if (T.bCanReload)
			{
				T.Check(!E.GetPlaySession().IsPlaying(), "stopped");
			}
		} },
		// Text beyond ASCII in the editor's own widgets: the Editor panel's entity combo and name
		// field show the selected entity's name. A project whose names are all ASCII skips this.
		// A capture is of the frame it is asked in: the next selection waits a frame.
		{ 1010u, "entity names beyond ASCII", [] (CEditorSmokeTest& T, CEditor& E, CEngine&) { T.SelectNamedEntity(E, 0u); } },
		{ 1020u, "first such name", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.CaptureNamedEntity(G, 0u, "18_entity_name_1"); } },
		{ 1021u, "second such name", [] (CEditorSmokeTest& T, CEditor& E, CEngine&) { T.SelectNamedEntity(E, 1u); } },
		{ 1030u, "second such name, shown", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.CaptureNamedEntity(G, 1u, "19_entity_name_2"); } },
		{ 1031u, "third such name", [] (CEditorSmokeTest& T, CEditor& E, CEngine&) { T.SelectNamedEntity(E, 2u); } },
		{ 1040u, "third such name, shown", [] (CEditorSmokeTest& T, CEditor&, CEngine& G) { T.CaptureNamedEntity(G, 2u, "20_entity_name_3"); } },
		{ 1041u, "names done", [] (CEditorSmokeTest&, CEditor& E, CEngine&) { E.GetTools().SelectEntity(-1); } },
	};

	// Hitches: the longest frame between two steps goes into the log with the next step.
	LongestFrameMs = (std::max)(LongestFrameMs, Engine.GetClock().GetDeltaSeconds() * 1000.0f);
	while (NextStep < std::size(steps) && frame >= steps[NextStep].Frame)
	{
		const SStep& step = steps[NextStep++];
		Log(FString::Format("step: {} (frame {}, longest frame since the last step {:.1f} ms)", step.Name, frame, LongestFrameMs));
		LongestFrameMs = 0.0f;
		step.Run(*this, Editor, Engine);
	}

	// Done: quit once the captures are on disk.
	if (NextStep == std::size(steps) && !bFinished && !Engine.GetRenderer()->HasPendingCaptures())
	{
		bFinished = true;
		// Glyphs are rasterised as text first needs them: the atlas grows with what the run drew.
		const ImTextureData* atlas = ImGui::GetIO().Fonts->TexData;
		Log(FString::Format("UI font atlas: {}x{}", atlas->Width, atlas->Height));
		Log(Failures == 0u ? "PASSED" : FString::Format("FAILED: {} check(s)", Failures));
		Engine.RequestQuit(Failures == 0u ? 0 : 2);
	}
}

void CEditorSmokeTest::SelectNamedEntity (CEditor& Editor, uint32 Which)
{
	if (Which == 0u)
	{
		NamedEntities.clear();
		const auto& entities = Editor.GetEditWorld().GetEntities();
		for (uint32 i = 0u; i < entities.Count() && NamedEntities.size() < 3u; ++i)
		{
			const CEntity* entity = entities[i].GetRawIgnoringLifetime();
			if (entity && !entity->Name.IsAscii())
			{
				NamedEntities.push_back(static_cast<int32>(i));
			}
		}
		if (NamedEntities.empty())
		{
			Log("  skip  every entity name is ASCII");
		}
	}
	if (Which >= NamedEntities.size())
	{
		return;
	}

	// Drawn with the UI font, so every character needs a glyph in it - a fallback's included.
	const CEntity& entity = *Editor.GetEditWorld().GetEntities()[NamedEntities[Which]].GetRawIgnoringLifetime();
	ImFont* font = ImGui::GetFont();
	uint32 missing = 0u;
	for (const char32_t codePoint : entity.Name.CodePoints())
	{
		if (!font->IsGlyphInFont(static_cast<ImWchar>(codePoint)))
		{
			Log(FString::Format("  no glyph for U+{:04X}", static_cast<uint32>(codePoint)));
			++missing;
		}
	}
	Check(missing == 0u, "the UI font has a glyph for every character of \"" + entity.Name + "\"");
	Editor.GetTools().SelectEntity(NamedEntities[Which]);
	// The panel's tab to the front: the dock node it shares may show another one by now.
	ImGui::SetWindowFocus("Editor");
}

void CEditorSmokeTest::CaptureNamedEntity (CEngine& Engine, uint32 Which, FStringLiteral Name)
{
	if (Which < NamedEntities.size())
	{
		Capture(Engine, Name);
	}
}

void CEditorSmokeTest::Check (bool bPassed, FStringView What)
{
	Log(FString(bPassed ? "  ok    " : "  FAIL  ") + What);
	if (!bPassed)
	{
		++Failures;
	}
}

void CEditorSmokeTest::Capture (CEngine& Engine, FStringLiteral Name)
{
	Engine.GetRenderer()->RequestBackBufferCapture(OutputDir / Name.ToPath());
	Log("  capture " + Name);
}

void CEditorSmokeTest::Log (FStringView Line)
{
	std::printf("[smoke] %.*s\n", static_cast<int>(Line.Len()), Line.GetData());
	std::fflush(stdout);
	std::ofstream(OutputDir / "SmokeTest.log", std::ios::app) << Line << '\n';
}
}
