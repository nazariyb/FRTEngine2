#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "Core.h"
#include "Singleton.h"
#include "Window.h"
#include "Sys_MeshRenderer.h"
#include "WorldScene.h"
#include "Config/Config.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Input/InputActionLibrary.h"
#include "Input/InputSystem.h"
#include "Memory/MemoryPool.h"
#include "Memory/Ref.h"
#include "Profiler/MetricsAggregator.h"
#include "Profiler/ProfilingSession.h"
#include "Profiler/SceneDescriptor.h"
#include "User/UserSettings.h"


namespace frt::graphics
{
class CCamera;
}


namespace frt::graphics
{
class CRenderer;
}


namespace frt
{
class CTimer;
class CWindow;
}


NAMESPACE_FRT_START
class FRT_CORE_API GameInstance : public Singleton<GameInstance>
{
public:
	GameInstance ();
	virtual ~GameInstance ();

	FRT_SINGLETON_GETTERS(GameInstance)

	CTimer& GetTime () const;

	config::CConfig& GetConfig () { return Config; }
	const config::CConfig& GetConfig () const { return Config; }

	bool HasGraphics () const;
#if !defined(FRT_HEADLESS)
	memory::TRefWeak<graphics::CRenderer> GetRenderer () const;

	// temp
	CWindow& GetWindow () const { return *Window; }
	memory::TRefWeak<graphics::CCamera> GetCamera () const { return Camera.GetWeak(); }
	// ~temp
#endif
	input::CInputSystem& GetInputSystem () { return InputSystem; }
	const input::CInputSystem& GetInputSystem () const { return InputSystem; }
	input::CInputActionLibrary& GetInputActionLibrary () { return InputActionLibrary; }
	const input::CInputActionLibrary& GetInputActionLibrary () const { return InputActionLibrary; }
	input::CInputActionMap* GetActiveInputActionMap ();
	const input::CInputActionMap* GetActiveInputActionMap () const;

	virtual void Load ();
	virtual void OnLoaded ();

	// Update
	virtual void Input (float DeltaSeconds);
	virtual void Tick (float DeltaSeconds);
#if !defined(FRT_HEADLESS)
	virtual void Draw (float DeltaSeconds);
#endif
	// ~Update

	uint64 GetFrameCount () const;

	CWorldScene& GetWorldScene () { return World; }
	const CWorldScene& GetWorldScene () const { return World; }

protected:
	// Pure stats computation — updates StatFps / StatMsPerFrame, no ImGui.
	void CalculateFrameStats ();

#ifndef FRT_HEADLESS
	virtual void OnWindowResize ();
	virtual void OnLoseFocus ();
	virtual void OnGainFocus ();
	virtual void OnMinimize ();
	virtual void OnRestoreFromMinimize ();

	// UI entry point, called from Tick. Master dispatches to per-panel functions.
	// All panel bodies live in GameInstanceUI.cpp (long-term they move out of GameInstance).
	virtual void DrawUI ();
	void DrawStatsPanel ();
	void DrawRaytracingPanel ();
	void DrawSkyPanel ();
	void DrawEditorPanel ();
	void DrawDisplaySettingsPanel ();
#endif

protected:
	// Declared before the memory pool because it is loaded before it: config depends on
	// nothing but the standard library, and everything below reads settings out of it.
	config::CConfig Config;

	memory::CMemoryPool MemoryPool;
	CTimer* Timer;
#ifndef FRT_HEADLESS
	CWindow* Window;
	memory::TRefUnique<graphics::CRenderer> Renderer;
	memory::TRefShared<graphics::CCamera> Camera;
	graphics::CRenderView* MainView = nullptr;    // owned by the renderer
	graphics::SCameraView MainCameraView;

#endif
	CWorldScene World;
	memory::TRefWeak<Sys_MeshRenderer> MeshRenderer;
	input::CInputSystem InputSystem;
	input::CInputActionLibrary InputActionLibrary;
	input::SInputActionMapAsset* ActiveActionMap = nullptr;

	graphics::SDisplayOptions DisplayOptions;
	SUserSettings UserSettings;

	// Live raytracing knobs — written by ImGui sliders, read by Sys_MeshRenderer when
	// filling SPassConstants. Tweak at runtime without shader recompile.
	struct SRtSettings
	{
		uint32 SampleCount = 32u;
		uint32 MaxBounces = 4u;
		uint32 RussianRouletteDepth = 2u;
		bool bPortalPreFilter = true;
		bool bShowPortalMeshes = false; // portal viz quads occlude rays — off by default
		bool bCollectCounters = false;  // ray-counter atomics — off by default (perf), on in sessions
	};

public:
	SRtSettings& GetRtSettings () { return RtSettings; }
	const SRtSettings& GetRtSettings () const { return RtSettings; }

	graphics::SSkyConstants& GetSkySettings () { return SkySettings; }
	const graphics::SSkyConstants& GetSkySettings () const { return SkySettings; }

	float GetSunAngularRadius () const { return SunAngularRadius; }

	float& GetTimeOfDay () { return TimeOfDay; }
	float GetTimeOfDay () const { return TimeOfDay; }
	bool& UseTimeOfDay () { return bUseTimeOfDay; }
	bool UseTimeOfDay () const { return bUseTimeOfDay; }

protected:
	SRtSettings RtSettings;

	graphics::SSkyConstants SkySettings;
	float TimeOfDay = 0.5f;    // [0,1]; 0.5 = noon
	float SunAngularRadius = 0.004654f; // radians; the real sun is ~0.267 deg
	bool bUseTimeOfDay = true; // when true, ImGui slider drives SkySettings via ComputeSkyFromTimeOfDay
	bool bShowImGui = true;    // toggled by key I; when false DrawUI is skipped

	// Editor selection — index into CWorldScene::GetEntities(). -1 = nothing selected.
	// Naive but stable as long as entities aren't reordered mid-frame.
	int32 SelectedEntityIndex = -1;

	uint64 FrameCount;

	// Frame-stats state (was static locals in CalculateFrameStats).
	float StatFps = 0.f;
	float StatMsPerFrame = 0.f;
	int StatFrameAccum = 0;
	float StatTimeElapsed = 0.f;

	// Per-frame metrics + rolling N-frame average. Sampled in Tick, read by panels.
	profiler::CMetricsAggregator Metrics;

	// Batch profiling sweep. While active it overrides RtSettings per profile; the
	// pre-session settings are restored on completion.
	profiler::CProfilingSession ProfilingSession;
	SRtSettings SavedRtSettings;
	// Static scene snapshot captured at session start (camera fixed for the run).
	profiler::SSceneDescriptor SessionSceneDesc;
	// Output folder for the active session: GetProfilingDir()/<SessionNameBuf>.
	std::filesystem::path SessionDir;
	char SessionNameBuf[64] = "session";

	bool bCameraMovementEnabled = false;
	math::STransform CameraInitialTransform;

#ifndef FRT_HEADLESS
	// Launch-time configuration ([Render] / [Automation] sections, usually from the command
	// line: -Render:Mode=Reference -Automation:CaptureFrame=300 ...).
	void ApplyLaunchConfig ();
	void UpdateAutomation (float DeltaSeconds);
	void BeginProfilingOverride ();
	void EndProfilingOverride ();

	int64 AutomationCaptureFrame = -1;
	int64 AutomationExitFrame = -1;
	bool bAutomationCaptureHdr = true;
	bool bAutomationCaptureRequested = false;
	std::filesystem::path AutomationCapturePath;
	// -Automation:ProfileFrames=N: average GPU scope times over the N frames before ExitFrame,
	// printed to stdout at exit ("[profile] <scope> <ms>").
	int64 AutomationProfileFrames = 0;
	// -Automation:CameraMove="x,y,z" (camera-local metres per frame) and CameraTurn="pitch,yaw,roll"
	// (radians per frame), applied from MotionStartFrame up to MotionEndFrame (-1 = until exit):
	// deterministic camera motion for ghosting / disocclusion tests.
	Vector3f AutomationCameraMove = Vector3f::ZeroVector;
	Vector3f AutomationCameraTurn = Vector3f::ZeroVector;
	int64 AutomationMotionStartFrame = 0;
	int64 AutomationMotionEndFrame = -1;
	bool bAutomationMotion = false;
	std::vector<std::pair<std::string, std::pair<double, uint32>>> AutomationProfile;
	double AutomationCpuFrameMs = 0.0;
	uint32 AutomationCpuFrames = 0u;

	// Path tracer settings saved while a profiling session drives them.
	struct SProfilingSnapshot
	{
		uint32 SamplesPerPixel = 1u;
		uint32 MaxBounces = 4u;
		uint32 RussianRouletteDepth = 3u;
		bool bPortalPreFilter = false;
		bool bCollectRayCounters = false;
	};
	SProfilingSnapshot SavedProfilingSettings;
#endif
};


NAMESPACE_FRT_END
