#include "Editor.h"

#include <chrono>
#include <format>
#include <span>
#include <thread>

#include "imgui.h"

#include "EnginePaths.h"
#include "GameInstance.h"
#include "Window.h"
#include "WorldScene.h"
#include "Engine/Engine.h"
#include "Engine/GameModule.h"
#include "Engine/LaunchConfig.h"
#include "Graphics/Render/Renderer.h"
#include "Graphics/Render/RenderView.h"
#include "Input/InputSystem.h"
#include "UI/ImGuiLayer.h"

#include "EditorSmokeTest.h"
#include "MainToolbar.h"


namespace frt::editor
{
namespace
{
constexpr int32 SceneIndex = static_cast<int32>(EViewportTarget::Scene);
constexpr int32 GameIndex = static_cast<int32>(EViewportTarget::Game);

// Frame pacing while another application has the foreground (Unreal's "use less CPU when in
// background"): the editor keeps running, at a fraction of the cost.
constexpr std::chrono::milliseconds BackgroundFrameSleep(33);

// Play mode tints the editor's chrome (Unity's play mode tint): it is never mistaken for edit
// mode, where changes are kept.
// Strong on purpose: the dark theme leaves little to multiply.
constexpr float PlayTint[3] = { 0.72f, 0.85f, 1.60f };
// How long a notice stays in the toolbar.
constexpr double NoticeSeconds = 8.0;

bool HasAnyModifier (SFlags<input::EInputModifier> Modifiers)
{
	return Modifiers.Flags != 0u;
}
}

CEditor::CEditor (CEngine& InEngine, const CGameModule& InModule)
	: Engine(InEngine)
	, Module(InModule)
	, Play(InEngine, InModule)
	, Viewports { { EViewportKind::Scene, "Scene" }, { EViewportKind::Game, "Game" } }
{
}

CEditor::~CEditor ()
{
	ShutDown();
}

bool CEditor::StartUp ()
{
	frt_assert(!bStarted && Engine.HasGraphics());
	config::CConfig& config = Engine.GetConfig();
	graphics::CRenderer& renderer = *Engine.GetRenderer();

	// The game authors the world's content. Its instance exists only for that: edit mode runs
	// no game code.
	EditWorld = std::make_unique<CWorldScene>();
	EditWorld->Initialize();
	{
		std::unique_ptr<GameInstance> builder = Module.CreateGame();
		if (!builder)
		{
			return false;
		}
		const SGameContext context { *EditWorld, Engine.GetMaterials(), Engine.GetInputActions(), config };
		builder->BuildWorld(context);
	}
	ApplySceneLaunchConfig(config, *EditWorld);
	ApplyRenderLaunchConfig(config, renderer);

	// The Scene view starts where the game's camera is, then goes its own way.
	SceneCamera = EditWorld->Camera;
	Tools.SetCameraHome(EditWorld->Camera.Transform);
	// The game's display settings panel would drive the editor's own window.
	Tools.bShowDisplaySettings = false;

	for (CViewportPanel& viewport : Viewports)
	{
		viewport.Create(renderer);
		viewport.GetView().SetRenderMode(Engine.GetUserSettings().DisplaySettings.RenderMode);
		ApplyViewLaunchConfig(config, viewport.GetView());
	}

	if (config.Get("Editor", "SmokeTest", false))
	{
		SmokeTest = std::make_unique<CEditorSmokeTest>(config);
		// Scripted runs start from the default layout and leave the user's alone. ImGui reads
		// the file on its first frame, which has not happened yet.
		ImGui::GetIO().IniFilename = nullptr;
	}

	// Shipped bindings, then the user's overrides - a scripted run's own, so that it neither
	// depends on nor touches the user's.
	Input.Load(paths::GetRepoRoot() / "Editor" / "Config" / "EditorInput.ini",
		SmokeTest ? SmokeTest->GetOutputDir() / "EditorInput.ini" : paths::GetLocalDir() / "Editor" / "EditorInput.ini");
	ApplyInputConfig(true);
	bStarted = true;
	return true;
}

void CEditor::ShutDown ()
{
	if (!bStarted)
	{
		return;
	}
	graphics::CRenderer& renderer = *Engine.GetRenderer();
	Capture.Release(*Engine.GetWindow());
	renderer.FlushCommandQueue();
	Play.End();
	for (CViewportPanel& viewport : Viewports)
	{
		viewport.Destroy(renderer);
	}
	// Before the game module unloads: the world's component pools were instantiated in its code.
	EditWorld.reset();
	SmokeTest.reset();
	bStarted = false;
}

int CEditor::Run ()
{
	CWindow& window = *Engine.GetWindow();
	while (Engine.PumpMessages())
	{
		const float realDeltaSeconds = Engine.BeginFrame();

		if (Input.ReloadIfChanged())
		{
			ApplyInputConfig(false);
		}

		// ---- commands: queued by last frame's UI, shortcuts and scripts ----
		ApplyCommands();
		ForgetClosedViewports();
		if (SmokeTest)
		{
			// After the commands: what it queues applies next frame, like a click would.
			SmokeTest->Update(*this, Engine);
		}
		CWorldScene& world = GetActiveWorld();
		// The first frames measure start-up, not frame time.
		const float frameSample = realDeltaSeconds * 1000.0f;
		FrameMilliseconds = Engine.GetFrameIndex() <= 2u ? frameSample : FrameMilliseconds + (frameSample - FrameMilliseconds) * 0.1f;

		// ---- UI ----
		// Focus moves before the dock space updates: its tab bars follow the focused window, so
		// a viewport restored from maximized shows its own tab on the very first frame.
		if (FocusRequest >= 0)
		{
			ImGui::SetWindowFocus(Viewports[FocusRequest].GetName());
		}
		// The bars first: they take their space from the work area the dock space then fills.
		// Submitted under a fullscreen viewport too - ImGui applies a bar's inset a frame late,
		// so a bar that comes back would cover the dock space's top row for a frame.
		DrawMainMenu();
		{
			SToolbarState toolbar;
			toolbar.bPlaying = Play.IsPlaying();
			toolbar.bPaused = Play.IsPlaying() ? Play.IsPaused() : bStartPaused;
			if (const CWorldScene* playWorld = Play.GetWorld())
			{
				toolbar.PlaySeconds = playWorld->Clock.TotalSeconds;
				toolbar.PlayFrames = playWorld->Clock.SimulatedFrames;
			}
			if (!Notice.empty() && ImGui::GetTime() - NoticeTime < NoticeSeconds)
			{
				toolbar.Message = Notice.c_str();
				toolbar.bMessageIsError = bNoticeIsError;
			}
			toolbar.PlayShortcut = Input.GetShortcutText(EEditorCommand::TogglePlay).c_str();
			toolbar.PauseShortcut = Input.GetShortcutText(EEditorCommand::TogglePause).c_str();
			toolbar.StepShortcut = Input.GetShortcutText(EEditorCommand::Step).c_str();
			if (const std::optional<EEditorCommand> command = DrawMainToolbar(toolbar))
			{
				Execute(*command);
			}
		}
		if (Layout.Draw())
		{
			FocusRequest = SceneIndex;   // a fresh layout opens on the Scene view
		}
		DrawViewports(FrameMilliseconds);

		// The tools inspect the viewport last used, and edit the camera it looks through.
		CViewportPanel& inspected = Viewports[ActiveViewport];
		graphics::CCamera& camera = ActiveViewport == SceneIndex ? SceneCamera : world.Camera;
		const CDebugTools::SContext tools { Engine, world, inspected.GetView(), camera, Extract };
		Tools.Update(tools, realDeltaSeconds);
		// Also under a fullscreen viewport: a docked window left out for a frame gives up its
		// dock node, and the layout would come back rearranged.
		Tools.Draw(tools);

		// ---- input: routed from what the panels reported ----
		HandleShortcuts();
		UpdateCameraInput(realDeltaSeconds);

		// ---- world ----
		if (Play.IsPlaying())
		{
			if (!Play.Tick(realDeltaSeconds, RouteGameInput()))
			{
				PostNotice("The game failed; play stopped (see the log)", true);
				Execute(EEditorCommand::Stop);
			}
		}
		else
		{
			// Edit mode simulates nothing; Finalize still propagates transforms, so edits show.
			world.Update(0.0f);
		}

		// ---- render ----
		RenderFrame();
		ApplyWindowMode();

		if (!window.IsForeground() && !SmokeTest)
		{
			std::this_thread::sleep_for(BackgroundFrameSleep);
		}
	}
	return Engine.GetExitCode();
}

void CEditor::Execute (EEditorCommand Command, EViewportTarget Viewport)
{
	Commands.push_back({ Command, static_cast<EViewportTarget>(ResolveViewport(Viewport)) });
}

void CEditor::FocusViewport (EViewportTarget Viewport)
{
	FocusRequest = ResolveViewport(Viewport);
}

void CEditor::PostNotice (std::string Text, bool bError)
{
	Notice = std::move(Text);
	NoticeTime = ImGui::GetTime();
	bNoticeIsError = bError;
}

void CEditor::ApplyInputConfig (bool bInitial)
{
	const SViewportCameraSettings& settings = Input.GetCameraSettings();
	SceneCameraController.Settings = settings;
	// The configured speed is where a session starts; the wheel and the toolbar change it from
	// there. A new value in the file applies at once.
	if (bInitial || settings.Speed != ConfiguredCameraSpeed)
	{
		SceneCameraController.Speed = settings.Speed;
		ConfiguredCameraSpeed = settings.Speed;
	}

	const std::vector<std::string>& problems = Input.GetProblems();
	if (!problems.empty())
	{
		PostNotice(problems.size() == 1u ? "Input config: " + problems.front()
			: std::format("Input config: {} problems, first: {}", problems.size(), problems.front()), true);
	}
	else if (!bInitial)
	{
		PostNotice("Input config reloaded", false);
	}
}

CWorldScene& CEditor::GetActiveWorld ()
{
	return Play.IsPlaying() ? *Play.GetWorld() : *EditWorld;
}

CViewportPanel& CEditor::GetViewport (EViewportTarget Target)
{
	return Viewports[ResolveViewport(Target)];
}

const CViewportPanel* CEditor::GetMaximizedViewport () const
{
	const int32 shown = FullscreenViewport >= 0 ? FullscreenViewport : MaximizedViewport;
	return shown >= 0 ? &Viewports[shown] : nullptr;
}

int32 CEditor::ResolveViewport (EViewportTarget Target) const
{
	if (Target != EViewportTarget::Active)
	{
		return static_cast<int32>(Target);
	}
	for (int32 i = 0; i < ViewportCount; ++i)
	{
		if (Viewports[i].GetFrame().bHovered)
		{
			return i;
		}
	}
	return ActiveViewport;
}

void CEditor::ApplyCommands ()
{
	// Swapped out first: a command may queue another, which then waits for the next frame.
	std::vector<SEditorCommand> commands;
	commands.swap(Commands);
	for (const SEditorCommand& command : commands)
	{
		ApplyCommand(command);
	}
}

void CEditor::ApplyCommand (const SEditorCommand& Command)
{
	const int32 viewport = static_cast<int32>(Command.Viewport);
	switch (Command.Id)
	{
		case EEditorCommand::Play:
		{
			StartPlay();
			break;
		}
		case EEditorCommand::Stop:
		{
			StopPlay();
			break;
		}
		case EEditorCommand::TogglePlay:
		{
			Play.IsPlaying() ? StopPlay() : StartPlay();
			break;
		}
		case EEditorCommand::TogglePause:
		{
			if (Play.IsPlaying())
			{
				Play.SetPaused(!Play.IsPaused());
			}
			else
			{
				bStartPaused = !bStartPaused;
			}
			break;
		}
		case EEditorCommand::Step:
		{
			Play.Step();
			break;
		}
		case EEditorCommand::ToggleMaximize:
		{
			// From fullscreen, one step back: to whatever the editor showed before.
			if (FullscreenViewport >= 0)
			{
				FocusRequest = FullscreenViewport;
				FullscreenViewport = -1;
			}
			else if (Viewports[viewport].bOpen)
			{
				MaximizedViewport = MaximizedViewport == viewport ? -1 : viewport;
				FocusRequest = viewport;
			}
			break;
		}
		case EEditorCommand::ToggleFullscreen:
		{
			if (FullscreenViewport >= 0)
			{
				FocusRequest = FullscreenViewport;
				FullscreenViewport = -1;
			}
			else if (Viewports[viewport].bOpen)
			{
				FullscreenViewport = viewport;
				FocusRequest = viewport;
			}
			break;
		}
		case EEditorCommand::ExitFullscreen:
		{
			if (FullscreenViewport >= 0)
			{
				FocusRequest = FullscreenViewport;
				FullscreenViewport = -1;
			}
			break;
		}
		case EEditorCommand::ToggleRenderMode:
		{
			graphics::CRenderView& view = Viewports[viewport].GetView();
			view.SetRenderMode(view.GetRenderMode() == graphics::ERenderMode::Raster ? graphics::ERenderMode::Raytracing : graphics::ERenderMode::Raster);
			break;
		}
		case EEditorCommand::ResetLayout:
		{
			Layout.RequestReset();
			MaximizedViewport = -1;
			FullscreenViewport = -1;
			for (CViewportPanel& panel : Viewports)
			{
				panel.bOpen = true;
			}
			break;
		}
		case EEditorCommand::Exit:
		{
			Engine.RequestQuit(0);
			break;
		}
		default:
			break;
	}
}

void CEditor::ForgetClosedViewports ()
{
	// A viewport closed from its tab or the View menu gives up every role it had: otherwise
	// the other one stays covered by an overlay that is no longer drawn, and commands and the
	// tools keep targeting a view that is not rendered.
	if (MaximizedViewport >= 0 && !Viewports[MaximizedViewport].bOpen)
	{
		MaximizedViewport = -1;
	}
	if (FullscreenViewport >= 0 && !Viewports[FullscreenViewport].bOpen)
	{
		FullscreenViewport = -1;
	}
	if (!Viewports[ActiveViewport].bOpen)
	{
		for (int32 i = 0; i < ViewportCount; ++i)
		{
			if (Viewports[i].bOpen)
			{
				ActiveViewport = i;
				break;
			}
		}
	}
}

void CEditor::StartPlay ()
{
	if (Play.IsPlaying())
	{
		return;
	}
	if (!Play.Begin(*EditWorld, bStartPaused))
	{
		PostNotice("The game failed to start (see the log)", true);
		return;
	}
	bStartPaused = false;
	// Another world: no history carries over.
	Engine.GetRenderer()->ResetAccumulation();
	Engine.GetUi()->SetTint(PlayTint[0], PlayTint[1], PlayTint[2]);
	// Unity's "focus on play": the game's view comes forward, with input - reopened if it was
	// closed, and in the place of another viewport that fills the editor or the screen.
	Viewports[GameIndex].bOpen = true;
	if (MaximizedViewport >= 0)
	{
		MaximizedViewport = GameIndex;
	}
	if (FullscreenViewport >= 0)
	{
		FullscreenViewport = GameIndex;
	}
	FocusRequest = GameIndex;
}

void CEditor::StopPlay ()
{
	if (!Play.IsPlaying())
	{
		return;
	}
	Capture.Release(*Engine.GetWindow());
	// The session's world may own GPU-visible data; nothing in flight may still use it.
	Engine.GetRenderer()->FlushCommandQueue();
	Play.End();
	Engine.GetRenderer()->ResetAccumulation();
	Engine.GetUi()->SetTint(1.0f, 1.0f, 1.0f);
}

void CEditor::DrawMainMenu ()
{
	if (!ImGui::BeginMainMenuBar())
	{
		return;
	}
	const auto shortcut = [this] (EEditorCommand Command) { return Input.GetShortcutText(Command).c_str(); };
	if (ImGui::BeginMenu("File"))
	{
		// Alt+F4 is the window's own, whatever is bound.
		if (ImGui::MenuItem("Exit", *shortcut(EEditorCommand::Exit) ? shortcut(EEditorCommand::Exit) : "Alt+F4"))
		{
			Execute(EEditorCommand::Exit);
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("View"))
	{
		for (int32 i = 0; i < ViewportCount; ++i)
		{
			if (ImGui::MenuItem(Viewports[i].GetName(), nullptr, &Viewports[i].bOpen) && Viewports[i].bOpen)
			{
				FocusRequest = i;
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Maximize Viewport", shortcut(EEditorCommand::ToggleMaximize), MaximizedViewport >= 0))
		{
			Execute(EEditorCommand::ToggleMaximize);
		}
		if (ImGui::MenuItem("Fullscreen Viewport", shortcut(EEditorCommand::ToggleFullscreen), FullscreenViewport >= 0))
		{
			Execute(EEditorCommand::ToggleFullscreen);
		}
		if (ImGui::MenuItem("Toggle Path Tracing", shortcut(EEditorCommand::ToggleRenderMode)))
		{
			Execute(EEditorCommand::ToggleRenderMode);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset Layout", shortcut(EEditorCommand::ResetLayout)))
		{
			Execute(EEditorCommand::ResetLayout);
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Play"))
	{
		const bool bPlaying = Play.IsPlaying();
		if (ImGui::MenuItem(bPlaying ? "Stop" : "Play", shortcut(EEditorCommand::TogglePlay)))
		{
			Execute(EEditorCommand::TogglePlay);
		}
		if (ImGui::MenuItem(bPlaying ? "Pause" : "Start Paused", shortcut(EEditorCommand::TogglePause), bPlaying ? Play.IsPaused() : bStartPaused))
		{
			Execute(EEditorCommand::TogglePause);
		}
		if (ImGui::MenuItem("Step", shortcut(EEditorCommand::Step), false, bPlaying))
		{
			Execute(EEditorCommand::Step);
		}
		ImGui::EndMenu();
	}
	ImGui::EndMainMenuBar();
}

void CEditor::DrawViewports (float FrameMilliseconds)
{
	const int32 shown = FullscreenViewport >= 0 ? FullscreenViewport : MaximizedViewport;
	// "F11 or Esc  exit fullscreen", from whatever is bound.
	std::string exitHint;
	for (const EEditorCommand command : { EEditorCommand::ToggleFullscreen, EEditorCommand::ExitFullscreen })
	{
		const std::string& text = Input.GetShortcutText(command);
		if (!text.empty())
		{
			exitHint += exitHint.empty() ? text : " or " + text;
		}
	}
	if (!exitHint.empty())
	{
		exitHint += "  exit fullscreen";
	}

	for (int32 i = 0; i < ViewportCount; ++i)
	{
		SViewportDrawArgs args;
		if (shown >= 0)
		{
			args.Presentation = i != shown ? EViewportPresentation::Covered
				: FullscreenViewport >= 0 ? EViewportPresentation::Fullscreen : EViewportPresentation::Maximized;
		}
		args.bTakeFocus = FocusRequest == i;
		args.bPlaying = Play.IsPlaying();
		args.bPaused = Play.IsPaused();
		args.FrameMilliseconds = FrameMilliseconds;
		args.MaximizeShortcut = Input.GetShortcutText(EEditorCommand::ToggleMaximize).c_str();
		args.RenderModeShortcut = Input.GetShortcutText(EEditorCommand::ToggleRenderMode).c_str();
		args.FullscreenExitHint = exitHint.c_str();
		if (i == SceneIndex)
		{
			args.CameraSpeed = &SceneCameraController.Speed;
			args.DefaultCameraSpeed = ConfiguredCameraSpeed;
		}

		const SViewportFrame& frame = Viewports[i].Draw(*Engine.GetRenderer(), args);
		if (frame.bToggleMaximize)
		{
			Execute(EEditorCommand::ToggleMaximize, static_cast<EViewportTarget>(i));
		}
		if (frame.bFocused)
		{
			ActiveViewport = i;
		}
	}
	FocusRequest = -1;
}

void CEditor::HandleShortcuts ()
{
	// Typing belongs to the text field; while the Scene camera navigates, the keys move it
	// (Unreal swallows them while its camera flies). A button held in the Game view is the
	// game's input, not a navigation: Stop and Pause must still work then.
	if (ImGui::GetIO().WantTextInput || Viewports[SceneIndex].GetFrame().bNavigating || Capture.IsActive())
	{
		return;
	}
	const input::CInputSystem& device = Engine.GetInput();
	for (uint8 i = 0u; i < static_cast<uint8>(EEditorCommand::Count); ++i)
	{
		const EEditorCommand command = static_cast<EEditorCommand>(i);
		if (Input.WasCommandPressed(command, device))
		{
			Execute(command);
		}
	}
}

void CEditor::UpdateCameraInput (float RealDeltaSeconds)
{
	const input::CInputSystem& device = Engine.GetInput();
	const SViewportFrame& scene = Viewports[SceneIndex].GetFrame();
	const SViewportFrame& game = Viewports[GameIndex].GetFrame();
	const SFlags<input::EInputModifier> modifiers = device.GetModifiers();
	const bool bTyping = ImGui::GetIO().WantTextInput;

	// The Scene camera, Unreal's way. Mouse input belongs to a drag that began on the view;
	// the wheel to the view under the cursor. Flight keys need that drag and no modifier;
	// keyboard keys need the view's focus and no modifier.
	SViewportNavigationInput navigation;
	navigation.bAlt = modifiers.HasFlag(input::EInputModifier::Alt);
	navigation.bCtrl = modifiers.HasFlag(input::EInputModifier::Ctrl);
	navigation.bShift = modifiers.HasFlag(input::EInputModifier::Shift);
	if (scene.bNavigating)
	{
		navigation.Buttons = scene.NavigationButtons;
		navigation.MouseDelta = device.GetRawMouseDelta();
	}
	if ((scene.bHovered || scene.bNavigating) && !bTyping)
	{
		navigation.Wheel = device.GetMouseWheelDelta();
	}
	const bool bFlightKeys = scene.bNavigating && !HasAnyModifier(modifiers);
	const bool bKeyboardKeys = scene.bVisible && scene.bFocused && !bTyping && !HasAnyModifier(modifiers);
	for (uint8 i = 0u; i < static_cast<uint8>(EViewportKey::Count); ++i)
	{
		const EViewportKey key = static_cast<EViewportKey>(i);
		navigation.Keys[i] =
			(bFlightKeys && Input.IsKeyDown(EViewportKeySet::Flight, key, device)) ||
			(bKeyboardKeys && Input.IsKeyDown(EViewportKeySet::Keyboard, key, device));
	}
	SceneCameraController.Update(SceneCamera, navigation, RealDeltaSeconds);

	// A right-drag on the Game view while playing: the mouse is the game's until the button is
	// released (Unreal's "capture during mouse down"). A placeholder for a cursor mode the game
	// itself will request.
	bGameLook = !scene.bNavigating && game.bNavigating && (game.NavigationButtons & viewport_button::Right) != 0u && Play.IsPlaying();

	// The cursor hides and stays put while a drag navigates (Unreal), and comes back where the
	// drag started.
	const SViewportFrame& captured = scene.bNavigating ? scene : game;
	Capture.Update(*Engine.GetWindow(), scene.bNavigating || bGameLook, captured.ImageMin, captured.ImageMax);
}

SGameInput CEditor::RouteGameInput () const
{
	// Unity's rule: the game has input while its view has focus. Never while the user types
	// into a panel, navigates the Scene view, or drags something that started outside the view.
	const SViewportFrame& game = Viewports[GameIndex].GetFrame();
	const ImGuiIO& io = ImGui::GetIO();
	const bool bMouseElsewhere = ImGui::IsAnyMouseDown() && !game.bNavigating;
	const bool bHasInput = game.bVisible && game.bFocused && !io.WantTextInput && !bMouseElsewhere &&
		!Viewports[SceneIndex].GetFrame().bNavigating;

	SGameInput input;
	input.ViewAspect = Viewports[GameIndex].GetAspect();
	if (bHasInput)
	{
		const input::CInputSystem& device = Engine.GetInput();
		input.Device = &device;
		input.LookDelta = bGameLook ? device.GetRawMouseDelta() : device.GetMouseDelta();
	}
	return input;
}

void CEditor::RenderFrame ()
{
	CWorldScene& world = GetActiveWorld();
	graphics::CRenderer& renderer = *Engine.GetRenderer();

	// Described after the world's update, so the renderer sees this frame's transforms.
	graphics::ExtractRenderScene(world, Extract, renderer.GetScene());
	if (world.bAccumulationDirty)
	{
		renderer.ResetAccumulation();
		world.bAccumulationDirty = false;
	}
	// One ray-counter buffer per frame: the inspected view fills it.
	renderer.SetCountersView(&Viewports[ActiveViewport].GetView());

	// Only what is on screen is rendered: a background tab or a covered viewport costs nothing.
	SViewToRender views[ViewportCount];
	size_t count = 0u;
	if (Viewports[SceneIndex].GetFrame().bVisible)
	{
		views[count++] = { &Viewports[SceneIndex].GetView(), graphics::MakeCameraView(SceneCamera) };
	}
	if (Viewports[GameIndex].GetFrame().bVisible)
	{
		views[count++] = { &Viewports[GameIndex].GetView(), graphics::MakeCameraView(world.Camera) };
	}
	Engine.RenderFrame(std::span<const SViewToRender>(views, count));
}

void CEditor::ApplyWindowMode ()
{
	// Between frames: the window's new size reaches the swap chain through WM_SIZE right away,
	// which must not happen while a frame is being recorded.
	CWindow& window = *Engine.GetWindow();
	const bool bImmersive = FullscreenViewport >= 0;
	if (window.IsImmersive() != bImmersive)
	{
		window.SetImmersive(bImmersive);
	}
}
}
