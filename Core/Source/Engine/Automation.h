#pragma once

#include <filesystem>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Math/Math.h"


namespace frt
{
class CEngine;
class CWorldScene;
}

namespace frt::graphics
{
class CRenderView;
}


namespace frt
{
/**
 * Scripted runs for capture-based testing and benchmarking, driven by [Automation] keys
 * (usually -Automation:Key=Value on the command line):
 *   CaptureFrame / CapturePath / CaptureHdr  PNG (+ linear PFM) of the view at a frame; the log
 *                                            line prints the camera pose for reference renders
 *   CaptureTimeoutFrames                     how long past CaptureFrame a capture may wait for
 *                                            texture streaming (default 600; then the run fails)
 *   ExitFrame                                quit once that frame is reached and captures are written
 *   ProfileFrames                            average GPU scope times over the frames before exit
 *   CameraMove / CameraTurn / Motion*Frame   deterministic camera motion (ghosting tests)
 *   TimeOfDayFrame / TimeOfDayValue          a lighting change mid-run (lag tests)
 *   DeterministicStreaming                   default true; false streams textures as interactive
 *                                            runs do (captures are then not repeatable)
 * Update runs once per frame after the frame's camera was taken, so motion lands a frame later -
 * captures depend on that timing.
 *
 * Scripted runs repeat: the world steps by a fixed time per frame (the host sets that up), and
 * textures become resident on frames that depend only on when they were requested
 * (CTextureManager's deterministic mode), not on decode-thread timing. That matters long after
 * loading: temporal histories (TAA, denoiser) keep a trace of every earlier frame, so a texture
 * arriving one frame later changes a capture taken hundreds of frames after. Raster captures of
 * identical binaries are bit-identical; path-traced ones still vary very slightly, as the
 * radiance cache inserts with GPU atomics. A capture waits for streaming to finish: it is taken
 * on the first frame from CaptureFrame on with no texture still loading.
 */
class FRT_CORE_API CAutomation
{
public:
	// Reads the [Automation] keys; a scripted run also switches the renderer to deterministic
	// texture streaming.
	void Configure (CEngine& Engine, const CWorldScene& World);

	// True when any [Automation] key is set: the run is scripted.
	bool IsActive () const { return bActive; }

	// View may be null (headless): captures and GPU profiling are then skipped.
	void Update (CEngine& Engine, CWorldScene& World, graphics::CRenderView* View, float RealDeltaSeconds);

private:
	bool bActive = false;
	int64 CaptureFrame = -1;
	int64 CaptureTimeoutFrames = 600;
	int64 ExitFrame = -1;
	bool bCaptureHdr = true;
	bool bCaptureRequested = false;
	std::filesystem::path CapturePath;
	int64 ProfileFrames = 0;
	Vector3f CameraMove = Vector3f::ZeroVector;     // camera-local metres per frame
	Vector3f CameraTurn = Vector3f::ZeroVector;     // pitch, yaw, roll radians per frame
	int64 MotionStartFrame = 0;
	int64 MotionEndFrame = -1;                       // -1: until exit
	bool bMotion = false;
	int64 TimeOfDayFrame = -1;
	float TimeOfDayValue = 0.5f;
	std::vector<std::pair<FString, std::pair<double, uint32>>> Profile;
	double CpuFrameMs = 0.0;
	uint32 CpuFrames = 0u;
};
}
