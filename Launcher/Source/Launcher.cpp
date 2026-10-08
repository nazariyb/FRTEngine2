/**
 * The launcher: the runtime host. Loads the game module named by the config, gives it one
 * world and one view straight into the swap chain, and runs the frame. The debug tools are an
 * overlay (I toggles them). With FRT_HEADLESS: no window, renderer or UI, the same simulation.
 * With FRT_WITH_GAME_HOT_RELOAD a newer build of the game module replaces the running game: the
 * world is built again by the new code, and play begins again.
 */
#include "Engine/HostEntry.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>

#include "EnginePaths.h"
#include "Exception.h"
#include "GameInstance.h"
#include "WorldScene.h"
#include "Engine/Automation.h"
#include "Engine/Engine.h"
#include "Engine/GameModule.h"
#include "Engine/LaunchConfig.h"
#include "Input/InputSystem.h"
#include "Project/Project.h"

#if !defined(FRT_HEADLESS)
#include "imgui.h"
#include "Window.h"
#include "Graphics/Render/Renderer.h"
#include "Graphics/Render/Scene/WorldExtraction.h"
#include "UI/DebugTools.h"
#endif


namespace frt
{
namespace
{
// How often a newer build of the game module is looked for.
constexpr std::chrono::milliseconds GamePollInterval(250);

int RunGameLoop (CEngine& Engine, CGameModule& Module)
{
	config::CConfig& config = Engine.GetConfig();
	const input::CInputSystem& device = Engine.GetInput();
	std::cout << "Working path: " << std::filesystem::current_path() << std::endl;

	// Replaced together on a hot reload: the world's component pools are the game module's
	// code as much as the game is.
	std::unique_ptr<CWorldScene> world;
	std::unique_ptr<GameInstance> game;
	const auto makeContext = [&] { return SGameContext { *world, Engine.GetMaterials(), Engine.GetInputActions(), config }; };
	CAutomation automation;

	// A new world, built by a new game; an empty one if there is no game.
	const auto buildGame = [&]
	{
		world = std::make_unique<CWorldScene>();
		world->Initialize();
		game = Module.IsLoaded() ? Module.CreateGame() : nullptr;
		if (!game)
		{
			return false;
		}
		game->BuildWorld(makeContext());
		ApplySceneLaunchConfig(config, *world);
		world->Clock.bPaused = config.Get("Scene", "PauseWorld", false);
		return true;
	};
	const auto beginPlay = [&]
	{
		if (automation.IsActive())
		{
			// Scripted runs are frame-indexed: every simulated frame advances the same game time.
			world->Clock.FixedDeltaSeconds = 1.0f / 60.0f;
		}
		game->BeginPlay(makeContext());
	};

	if (!buildGame())
	{
		std::cout << "The game module could not create its game" << std::endl;
		return 1;
	}
	automation.Configure(config, *world);

#if !defined(FRT_HEADLESS)
	graphics::CRenderer& renderer = *Engine.GetRenderer();
	ApplyRenderLaunchConfig(config, renderer);
	// The standalone game draws one view, straight into the swap chain.
	graphics::CRenderView* view = renderer.CreateView({ "Main", graphics::EViewTarget::BackBuffer });
	view->SetRenderMode(Engine.GetUserSettings().DisplaySettings.RenderMode);
	ApplyViewLaunchConfig(config, *view);

	CDebugTools tools;
	tools.SetCameraHome(world->Camera.Transform);
	graphics::SRenderExtractOptions extract;
	bool bShowUi = config.Get("Display", "ShowUi", true);
	bool bUiOwnsDrag = false;
#endif

	beginPlay();
	std::chrono::steady_clock::time_point nextGamePoll = std::chrono::steady_clock::now() + GamePollInterval;

	while (Engine.PumpMessages())
	{
		const float deltaSeconds = Engine.BeginFrame();

#if FRT_WITH_GAME_HOT_RELOAD
		// Between frames, everything holding the old build's code goes before the module
		// reloads (CGameModule). A build that does not load leaves an empty world, and the next
		// build is tried again.
		if (std::chrono::steady_clock::now() >= nextGamePoll)
		{
			nextGamePoll = std::chrono::steady_clock::now() + GamePollInterval;
			if (Module.HasNewerBuild())
			{
				const std::string name = Module.GetName();
				std::cout << "[launcher] new build of game module '" << name << "', reloading" << std::endl;
				if (game)
				{
					game->EndPlay(makeContext());
				}
				game.reset();
#if !defined(FRT_HEADLESS)
				// The world's GPU data may still be in flight.
				renderer.FlushCommandQueue();
#endif
				world.reset();
				Module.Load(name);
				if (buildGame())
				{
					beginPlay();
					std::cout << "[launcher] game module '" << name << "' reloaded" << std::endl;
				}
				else
				{
					std::cout << "[launcher] no game until another build of '" << name << "' loads" << std::endl;
				}
#if !defined(FRT_HEADLESS)
				tools.SetCameraHome(world->Camera.Transform);
				renderer.ResetAccumulation();
#endif
			}
		}
#endif

		SGameFrame frame;
		frame.RealDeltaSeconds = deltaSeconds;
		frame.GameDeltaSeconds = world->Clock.Advance(deltaSeconds);

		// Ctrl is the UI's modifier (Ctrl+Wheel zoom): it outranks every other binding. Keys typed
		// into a text field, and a right-drag that started over a debug panel, belong to the UI.
		const bool bCtrlDown = device.IsKeyDown(input::KeyCode::LeftCtrl) || device.IsKeyDown(input::KeyCode::RightCtrl);
#if !defined(FRT_HEADLESS)
		const ImGuiIO& io = ImGui::GetIO();
		const bool bTyping = io.WantTextInput;
		if (device.WasMouseButtonPressed(input::EMouseButton::Right))
		{
			bUiOwnsDrag = bShowUi && io.WantCaptureMouse;
		}
		else if (!device.IsMouseButtonDown(input::EMouseButton::Right))
		{
			bUiOwnsDrag = false;
		}
#else
		const bool bTyping = false;
		const bool bUiOwnsDrag = false;
#endif
		if (!bCtrlDown && !bTyping)
		{
			if (!bUiOwnsDrag)
			{
				frame.Input.Device = &device;
				frame.Input.LookDelta = device.GetMouseDelta();
			}
#if !defined(FRT_HEADLESS)
			const Vector2f windowSize = Engine.GetWindow()->GetWindowSize();
			frame.Input.ViewAspect = math::Max(windowSize.x, 1.0f) / math::Max(windowSize.y, 1.0f);

			if (device.WasKeyPressed(input::KeyCode::F2))
			{
				// Raster preview <-> path tracing (Hybrid is an alias of path tracing).
				const graphics::ERenderMode mode = view->GetRenderMode() == graphics::ERenderMode::Raster ? graphics::ERenderMode::Raytracing : graphics::ERenderMode::Raster;
				Engine.GetUserSettings().DisplaySettings.RenderMode = mode;
				view->SetRenderMode(mode);
			}
			if (device.WasKeyPressed(input::KeyCode::I))
			{
				bShowUi = !bShowUi;
			}
#endif
			if (device.WasKeyPressed(input::KeyCode::Space) && !device.IsMouseButtonDown(input::EMouseButton::Right))
			{
				world->Clock.bPaused = !world->Clock.bPaused;
			}
		}

#if !defined(FRT_HEADLESS)
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		const CDebugTools::SContext toolsContext { Engine, *world, *view, world->Camera, extract };
		tools.Update(toolsContext, deltaSeconds);
		if (bShowUi)
		{
			tools.Draw(toolsContext);
		}
#endif

		if (game)
		{
			game->Tick(makeContext(), frame);
		}
		world->Update(frame.GameDeltaSeconds);

#if !defined(FRT_HEADLESS)
		// Described after the world's update, so the renderer sees this frame's transforms. The
		// camera is taken now: automation moves it next, landing a frame later by design.
		ExtractRenderScene(*world, extract, renderer.GetScene());
		if (world->bAccumulationDirty)
		{
			renderer.ResetAccumulation();
			world->bAccumulationDirty = false;
		}
		const SViewToRender views[] = { { view, graphics::MakeCameraView(world->Camera) } };
		if (renderer.IsReadyToRender())
		{
			automation.Update(Engine, *world, view, deltaSeconds);
		}
		Engine.RenderFrame(views);
#else
		automation.Update(Engine, *world, nullptr, deltaSeconds);
#endif
	}

	if (game)
	{
		game->EndPlay(makeContext());
	}
#if !defined(FRT_HEADLESS)
	renderer.FlushCommandQueue();
	renderer.DestroyView(view);
#endif
	return Engine.GetExitCode();
}
}

int RunLauncher ()
{
	// The game to run: its project names the module and holds the content and config.
	std::string error;
	const std::filesystem::path projectFile = project::FindProjectToRun(true, &error);
	project::SProjectDescriptor gameProject;
	if (projectFile.empty() || !project::SProjectDescriptor::Load(projectFile, gameProject, &error))
	{
		ReportStartupError(error.empty()
			? "No project to run. Start the launcher with -Project=<project file>, or from a project's directory."
			: error);
		return 1;
	}
	paths::SetProject(gameProject);

	SEngineDesc desc;
	desc.WindowTitle = WidenUtf8(gameProject.Name);
	desc.UiIniPath = paths::GetSavedDir() / "Launcher" / "imgui.ini";
	CEngine engine;
	if (!engine.StartUp(desc))
	{
		return 1;
	}

	int exitCode = 1;
	{
		CGameModule module;
		if (module.Load(CGameModule::GetConfiguredName(engine.GetConfig())))
		{
			try
			{
				exitCode = RunGameLoop(engine, module);
			}
			catch (const Exception& e)
			{
				std::cout << e.What() << " " << e.GetErrorCode() << " : " << e.GetErrorDescription() << std::endl;
			}
			catch (const std::exception& e)
			{
				std::cout << e.what() << std::endl;
			}
			catch (...)
			{
				std::cout << "Unknown exception" << std::endl;
			}
		}
	}

	engine.ShutDown();
	return exitCode;
}
}


FRT_D3D12_AGILITY_EXPORTS

#if !defined(FRT_HEADLESS)
int APIENTRY wWinMain (_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
#else
int main ()
#endif
{
	FRT_VERIFY_REAL_PRECISION_ABI();
	return frt::RunLauncher();
}
