#pragma once

#include "Core.h"
#include "Singleton.h"
#include "Window.h"
#include "Sys_MeshRenderer.h"
#include "WorldScene.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Graphics/Render/RenderResourceAllocators.h"
#include "Input/InputActionLibrary.h"
#include "Input/InputSystem.h"
#include "Memory/MemoryPool.h"
#include "Memory/Ref.h"
#include "Profiler/MetricsAggregator.h"
#include "Profiler/ProfilingSession.h"
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
	memory::CMemoryPool MemoryPool;
	CTimer* Timer;
#ifndef FRT_HEADLESS
	CWindow* Window;
	memory::TRefUnique<graphics::CRenderer> Renderer;
	memory::TRefShared<graphics::CCamera> Camera;

	// Dedicated descriptor heap for ImGui (font + a few spares).
	// Kept separate from Renderer's ShaderDescriptorHeap so engine-side heap rebuilds
	// on material/texture growth never invalidate ImGui's font slot or trash its state.
	graphics::DX12_DescriptorHeap ImGuiDescriptorHeap;
#endif
	CWorldScene World;
	memory::TRefWeak<Sys_MeshRenderer> MeshRenderer;
	input::CInputSystem InputSystem;
	input::CInputActionLibrary InputActionLibrary;
	input::SInputActionMapAsset* ActiveActionMap = nullptr;

	// temp
	memory::TRefShared<CEntity> Cube;
	memory::TRefShared<CEntity> Cylinder;
	memory::TRefShared<CEntity> Sphere;
	void UpdateEntities (float DeltaSeconds);
	// ~temp

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

	float& GetTimeOfDay () { return TimeOfDay; }
	float GetTimeOfDay () const { return TimeOfDay; }
	bool& UseTimeOfDay () { return bUseTimeOfDay; }
	bool UseTimeOfDay () const { return bUseTimeOfDay; }

protected:
	SRtSettings RtSettings;

	graphics::SSkyConstants SkySettings;
	float TimeOfDay = 0.5f;    // [0,1]; 0.5 = noon
	bool bUseTimeOfDay = true; // when true, ImGui slider drives SkySettings via ComputeSkyFromTimeOfDay

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

	bool bCameraMovementEnabled = false;
	math::STransform CameraInitialTransform;
};


NAMESPACE_FRT_END
