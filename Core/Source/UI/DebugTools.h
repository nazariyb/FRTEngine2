#pragma once

#include <filesystem>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Math/Transform.h"
#include "Profiler/MetricsAggregator.h"
#include "Profiler/ProfilingSession.h"
#include "Profiler/SceneDescriptor.h"


namespace frt
{
class CEngine;
class CWorldScene;
}

namespace frt::graphics
{
class CCamera;
class CRenderView;
struct SRenderExtractOptions;
}


namespace frt
{
/**
 * The engine's debug tools (GEA "Debugging and Development Tools"): frame stats and GPU
 * timings, ray-counter metrics, profiling sessions, and the Stats, Renderer, Sky / Sun, Editor
 * and DisplaySettings panels. Shared by the runtime (an overlay, toggled with I) and the editor
 * (docked panels). Window names are stable: saved ImGui layouts keep finding them.
 */
class FRT_CORE_API CDebugTools
{
public:
	// What the tools read and edit this frame.
	struct SContext
	{
		CEngine& Engine;
		CWorldScene& World;
		graphics::CRenderView& View;          // render settings readouts, stats, captures
		graphics::CCamera& Camera;            // the camera the Editor panel edits
		graphics::SRenderExtractOptions& Extract;
	};

	// Where "Reset Camera" returns to.
	void SetCameraHome (const math::STransform& Home) { CameraHome = Home; }

	// Every frame, shown or not: frame stats, metrics, the profiling session.
	void Update (const SContext& Context, float RealDeltaSeconds);
	void Draw (const SContext& Context);

	// The entity the Editor panel shows and edits: an index into CWorldScene::GetEntities(), -1 for
	// none. One past the end reads as none on the next Draw.
	void SelectEntity (int32 Index) { SelectedEntityIndex = Index; }

	bool IsProfiling () const { return ProfilingSession.IsActive(); }
	float GetFps () const { return StatFps; }
	float GetMsPerFrame () const { return StatMsPerFrame; }

	bool bShowDisplaySettings = true;   // the game's display settings (not the editor's window)

private:
	void DrawStatsPanel (const SContext& Context);
	void DrawRendererPanel (const SContext& Context);
	void DrawSkyPanel (const SContext& Context);
	void DrawEditorPanel (const SContext& Context);
	void DrawDisplaySettingsPanel (const SContext& Context);
	void BeginProfilingOverride (const SContext& Context);
	void EndProfilingOverride (const SContext& Context);

	math::STransform CameraHome;
	int32 SelectedEntityIndex = -1;       // into CWorldScene::GetEntities(); -1 = none

	float StatFps = 0.0f;
	float StatMsPerFrame = 0.0f;
	int StatFrameAccum = 0;
	float StatTimeElapsed = 0.0f;

	// Per-frame metrics + rolling N-frame average, read by the panels and the profiling session.
	profiler::CMetricsAggregator Metrics;

	// Batch profiling sweep. While active it drives the path tracer settings; the pre-session
	// settings are restored when it ends.
	profiler::CProfilingSession ProfilingSession;
	struct SProfilingSnapshot
	{
		uint32 SamplesPerPixel = 1u;
		uint32 MaxBounces = 4u;
		uint32 RussianRouletteDepth = 3u;
		bool bPortalPreFilter = false;
		bool bCollectRayCounters = false;
	};
	SProfilingSnapshot SavedProfilingSettings;
	profiler::SSceneDescriptor SessionSceneDesc;   // static scene snapshot at session start
	std::filesystem::path SessionDir;               // GetProfilingDir()/<SessionName>
	static constexpr uint32 MaxSessionNameBytes = 63u;
	FString SessionName = "session";

	// The Editor panel's name field edits a copy, so a selected entity's name is cut to the
	// field's limit only when someone edits it. A member: reused from frame to frame. The limit is
	// the panel's (63 bytes, what its field shows): CEntity::Name itself has none.
	static constexpr uint32 MaxEntityNameBytes = 63u;
	FString EntityNameEdit;
};
}
