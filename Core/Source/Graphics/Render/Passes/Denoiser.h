#pragma once

#include "Graphics/Render/RenderContext.h"


namespace frt::config
{
class CConfig;
}

namespace frt::graphics
{
/**
 * In-house real-time denoiser for the path tracer's demodulated signals (ReLAX / ReBLUR class).
 *
 * Inputs (render resolution, produced by PathTracer.hlsl, see Shared/PathTracerData.h):
 *   Diffuse          RGBA16F  rgb = diffuse radiance / diffuse albedo, a = hit distance (0 = none)
 *   Specular         RGBA16F  rgb = specular radiance / specular albedo, a = hit distance
 *   NormalRoughness  RGBA16F  xyz = world shading normal (unit), w = linear roughness
 *   ViewZ            R32F     linear view depth; >= 1e5 means sky (no surface)
 *   MotionVectors    RG16F    (previous - current) pixel position, render pixels
 * Outputs: denoised Diffuse / Specular in the same demodulated form (RGBA16F); the renderer
 * remodulates with the albedo buffers and adds emission.
 *
 * The input can be extremely sparse (demo scene without the radiance cache, 1 spp: 99 % of the
 * samples exactly zero, the energy in rare samples up to thousands of times the local mean), so
 * every statistic and weight is one that stays unbiased for such signals - neighbourhood boxes,
 * rank filters and value-dependent normalised weights are not; the shader headers give the
 * measurements behind each choice. Pipeline per frame (each part toggleable through Settings):
 *   1. Temporal (Temporal.hlsl): reprojection with motion vectors (diffuse) and hit-distance virtual
 *      motion for low-roughness specular, per-tap disocclusion test (plane distance + normal),
 *      history length, luminance moments, a per-pixel fast history (anti-lag reference), an input
 *      firefly limit far above last frame's denoised value, per-8x8-tile anti-lag sums and the
 *      noise texture (deviation of the accumulated mean, specular filter extent, hit distance).
 *   2. History fix (HistoryFix.hlsl): young pixels only (disocclusions) get a wide geometry-weighted
 *      fill from their converged neighbours.
 *   3. A-trous (Atrous.hlsl, 5 iterations): energy-conserving edge-stopping filter (symmetric pair
 *      weights: plane, normal, luminance deviation; specular also roughness, lobe, hit distance and
 *      a noise-driven extent). Iteration 0 writes the history (optionally its filtered output,
 *      recurrent) and applies the anti-lag significance test pooled over 5x5 tiles.
 */
struct SDenoiserSettings
{
	bool bEnabled = true;
	bool bDenoiseDiffuse = true;
	bool bDenoiseSpecular = true;
	uint32 MaxHistoryLength = 64u;
	uint32 MaxFastHistoryLength = 8u;    // anti-lag reference window
	bool bHistoryFix = true;
	bool bAntiLag = true;
	float AntilagSigma = 4.0f;           // 40x40 mean change, in standard errors of the fast mean, that counts
	float AntilagSensitivity = 1.0f;     // history-length reduction per AntilagSigma of excess change
	// Input samples above FireflyThreshold x last frame's denoised luminance are limited to it. High on
	// purpose: rare large samples are the signal here (demo scene at 1 spp, 300 frames, 64x64 block
	// means / reference: 3000x -> 0.946, 1000x -> 0.908, 300x -> 0.799, off -> 0.967; mean of the
	// reflected floor in the mirror: 30000x -> 0.90, 10000x -> 0.84, 3000x -> 0.77, off -> 0.98). Off,
	// the a-trous lattice leaves the rarest samples as star-shaped sparkles, mostly in reflections.
	bool bFireflySuppression = true;
	float FireflyThreshold = 10000.0f;
	uint32 AtrousIterations = 5u;        // 0..8; 0 = temporal only
	float PhiNormal = 64.0f;
	float PhiDepth = 0.02f;              // relative plane-distance tolerance (plane distance / viewZ)
	float PhiLuminanceDiffuse = 4.0f;
	float PhiLuminanceSpecular = 3.0f;
	float LuminanceFloor = 0.25f;        // luminance differences below this x the mean never stop a tap
	bool bVirtualMotionSpecular = true;

	// Temporal accumulation.
	float DisocclusionThreshold = 0.01f; // plane distance / viewZ for a history tap to be the same surface
	float ReprojectionNormalAngle = 60.0f; // degrees; history taps with a larger normal change are rejected
	// 5-tap bicubic slow-history fetch where the footprint is valid. Off by default: its anti-ringing
	// clamp to the 2x2 range removes the negative lobes of rare bright texels but keeps the positive
	// ones, i.e. adds energy at every resampling of a sparse signal (camera turn 0.003 rad/frame for
	// 100 frames: 64x64 block means at 1.14x the reference with it, 0.96x without).
	bool bCatmullRomHistory = false;
	bool bJitterCompensation = false;    // re-align history to the jitter (sharper TAA input, softer static image)
	float SpecularParallaxScale = 1.0f;  // view-angle change sensitivity of the specular history cap
	float MomentsMinAlpha = 0.0f;        // floor of the luminance-moments blend weight (0 = history rate)

	// History fix (disocclusions).
	uint32 HistoryFixFrames = 4u;        // histories shorter than this are reconstructed spatially
	float HistoryFixRadius = 16.0f;      // pixels at 1080p (scaled with the render height)

	// Spatial filter.
	// Recurrent filtering (SVGF / ReBLUR feedback of a-trous iteration 0). Off by default: on the demo
	// scene it gave no static gain and blurred / smeared reflections under motion (camera move,
	// 100 frames: holes 6.2 % with, 4.7 % without; static relMSE 0.20 with, 0.13 without).
	bool bFeedbackFirstIteration = false;
	bool bFeedbackMoments = false;       // the moments history is filtered the same way (pooled noise statistics)
	bool bAtrousRotation = true;         // rotate the 3x3 taps per 8x8 block on iterations 1+ (no lattice artefacts)
	float SpecularLobeFraction = 0.5f;   // fraction of the lobe half-angle accepted by specular normal weights
	float RoughnessFraction = 0.15f;     // roughness tolerance = 0.02 + RoughnessFraction * roughness
	float SpecularFootprintScale = 1.0f; // specular kernel extent, multiples of the reflection blur footprint
	float SpecularNoiseTarget = 0.3f;    // relative deviation the noise-driven specular radius aims for
	float SpecularNoiseRadius = 16.0f;   // max noise-driven specular radius, pixels at 1080p (0 = lobe footprint only)
	float SpecularHitTolerance = 0.2f;   // relative hit-distance difference that separates reflected surfaces

	// Debug.
	bool bHistoryLengthDebug = false;    // write GetHistoryLengthTexture() (costs bandwidth every frame)
	float DebugInputScale = 1.0f;        // multiplies the noisy inputs from frame DebugInputScaleFrame on:
	uint32 DebugInputScaleFrame = 0u;    // a simulated lighting change for anti-lag tests
	uint32 HistoryDebugSignal = 0u;      // 0 = diffuse, 1 = specular in GetHistoryLengthTexture()
	uint32 OutputSignal = 0u;            // 0 = both, 1 = diffuse only, 2 = specular only, 3 = history length
	                                     // (bHistoryLengthDebug) as a grey diffuse signal, else 1
};

struct SDenoiserInputs
{
	rhi::SGpuTexture* Diffuse = nullptr;
	rhi::SGpuTexture* Specular = nullptr;
	rhi::SGpuTexture* NormalRoughness = nullptr;
	rhi::SGpuTexture* ViewZ = nullptr;
	rhi::SGpuTexture* MotionVectors = nullptr;
	bool bResetHistory = false;          // camera cut / settings change
};

class FRT_CORE_API CDenoiser
{
public:
	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown (rhi::CDevice& Device);
	void Resize (rhi::CDevice& Device, uint32 Width, uint32 Height);

	// Records all passes. Leaves the input textures in NON_PIXEL_SHADER_RESOURCE.
	void Execute (CRenderContext& Context, const SDenoiserInputs& Inputs);

	// Denoised outputs (RGBA16F, rgb = demodulated radiance, a = denoised hit distance), left in
	// NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE. Sky pixels hold the untouched input.
	rhi::SGpuTexture& GetDiffuseOutput ();
	rhi::SGpuTexture& GetSpecularOutput ();

	// Debug: per-pixel history length (R16F, length / MaxHistoryLength in [0, 1], signal chosen by
	// Settings.HistoryDebugSignal; written only with Settings.bHistoryLengthDebug), may return nullptr.
	rhi::SGpuTexture* GetHistoryLengthTexture ();

	void DrawSettingsUi ();

	// Session overrides from the [Denoiser] config section (launch arguments -Denoiser:Key=Value,
	// keys = SDenoiserSettings member names without the b prefix, e.g. -Denoiser:AntiLag=false).
	void ApplyConfig (const config::CConfig& Config);

	SDenoiserSettings Settings;

private:
	struct SImpl;
	SImpl* Impl = nullptr;
};
}
