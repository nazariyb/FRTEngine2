#pragma once

#include "CoreTypes.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Shared/PathTracerData.h"


namespace frt::graphics
{
/**
 * Every renderer setting, as plain data: no GPU types, so the header is safe anywhere -
 * headless builds and tools included. The passes are documented with their classes
 * (Passes/*.h); this file only holds what can be tuned.
 *
 * SRenderProfile is the one set of settings the renderer applies to every view, like a
 * quality level. Launch configuration (-Render:/-Post:/-Taa:/-Denoiser: keys), the settings
 * panels and profiling sessions all write it; each view's pass instances receive a copy right
 * before they record.
 */
enum class EPathTracingMode : uint8
{
	RealTime = 0,   // 1-4 spp, denoised, temporally upscaled
	Reference,      // progressive accumulation, unbiased, converges to ground truth
};

struct SRenderSettings
{
	EPathTracingMode Mode = EPathTracingMode::RealTime;
	float RenderScale = 1.0f;            // real time: render resolution / display resolution
	bool bDenoiser = true;
	uint32 ReferenceSampleTarget = 0u;   // stop tracing after this many samples (0 = never)
	bool bCollectRayCounters = false;
	float FieldOfViewY = 1.5707963f;     // radians (vertical)
	float NearZ = 0.05f;
};

struct SPathTracerSettings
{
	uint32 SamplesPerPixel = 1u;
	uint32 MaxBounces = 4u;
	uint32 RussianRouletteDepth = 3u;
	uint32 RisCandidates = 4u;
	uint32 RisCandidatesSecondary = 1u;  // at bounce vertices: indirect noise is dominated by BSDF sampling there
	float IndirectClamp = 20.0f;         // real time only (reference mode never clamps); 0 = off
	float MinRoughness = 0.0f;
	bool bShaderExecutionReordering = false; // measured net-negative here (+1.8 ms on RTX 4080): one uber shading path in raygen leaves little divergence to sort, and the reorder spills the path state
	bool bInlineRayQueries = true;        // camera/bounce rays via RayQuery: -11% vs TraceRay (no RT-stack spill of the path state)
	bool bNormalMaps = true;
	bool bPathRegularization = false;
	bool bPortalGuiding = true;
	bool bPortalPreFilter = false;       // legacy thesis estimator (biased if portals are incomplete)
	bool bPrimarySurfaceReplacement = true; // real time: delta-mirror pixels give the denoiser/TAA the reflected surface's guide
	bool bDisableNee = false;            // validation: BSDF sampling only
	bool bDisableBsdfMis = false;        // validation: NEE only for sampleable emitters
	bool bWhiteFurnace = false;          // validation: albedo 1, environment 1, no lights
	uint32 DebugMode = gpu::PathDebug_None;
};

struct SRadianceCacheSettings
{
	bool bEnabled = true;
	uint32 CapacityLog2 = 20u;            // hash entries = 2^CapacityLog2 (40 B each); Sponza needs < 2^18
	float CellScale = 0.06f;              // cell size (m) at 1 m from the camera, doubling per octave of distance
	uint32 UpdateTileSize = 5u;           // one update path per TileSize x TileSize pixels per frame (5: 4 %)
	// Resolve history length: stability vs lag after lighting changes. The denoiser's own 64-frame
	// history hides the extra cache noise: 32 -> 16 left the final image unchanged (static default
	// view relMSE 0.0512 / SSIM 0.593, fast camera motion SSIM 0.365 both ways; raw 1-spp relMSE
	// 17.3 -> 17.5) and cut the share of a sudden indirect-lighting change (sun step, TimeOfDay
	// 0.5 -> 0.47) still missing 1 s later from 0.60 to 0.50.
	uint32 AccumulationFrames = 16u;
	uint32 StaleFrames = 128u;            // frames without samples before a cell is evicted
	float RoughnessThreshold = 0.25f;     // glossier vertices are not answered from the cache: irradiance cannot give a sharp reflection
	float FootprintScale = 1.0f;          // query when the ray cone is wider than FootprintScale cells
	float MinSampleWeight = 8.0f;         // accumulated samples before a cell answers queries
	float SampleClamp = 4096.0f;          // max component of one irradiance sample: overflow guard (lower values bias dark)
	float SpecularShareMax = 0.15f;       // ... unless the specular share of the albedo is below this (diffuse dominated)
	bool bFeedback = true;                // update paths end at cached vertices: multi-bounce beyond MaxBounces
	uint32 FeedbackMaxDepth = 4u;         // update paths may end on the cache from a uniform depth in [1, this] (1 starves deep cells)
	bool bJitter = true;                  // jittered lookups (Binder et al.): no visible cell grid
};

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

// Which temporal anti-aliasing / upscaling implementation renders a view (see IUpscaler).
enum class EUpscalerKind : uint8
{
	None = 0,        // bilinear resample, no temporal accumulation
	Temporal,        // in-house TAA / TAAU
	Dlss,            // reserved (NVIDIA Streamline / NGX)
	Fsr,             // reserved (AMD FidelityFX FSR 3.1)
	Xess,            // reserved (Intel XeSS)
	Count
};

struct STemporalUpscalerSettings
{
	float HistoryBlend = 0.06f;            // base weight of the current frame
	float Sharpness = 0.25f;               // post-resolve sharpening (RCAS-like), 0 = off
	bool bCatmullRomHistory = true;        // 5-tap bicubic history fetch
	bool bVarianceClip = true;             // YCoCg variance clipping (vs. min/max clamp)
	float VarianceGamma = 1.25f;           // rectification slack in spatial standard deviations
	// Rectification slack in temporal standard deviations of the pixel's own input. Lets a
	// noisy or jitter-aliased pixel keep a history whose mean its neighbourhood rarely shows
	// (path-traced input, sub-pixel highlights); 0 = classic variance clipping.
	float TemporalGamma = 3.0f;
	// Karis 1 / (1 + L) blend weights. Off by default: on a path-traced (sparse) input they
	// pull the history toward the harmonic mean and lose energy (20% on the default view).
	bool bAntiFlicker = false;
	uint32 JitterPhases = 16u;             // Halton(2,3) sequence length (scaled by upscale ratio)

	bool bLanczosKernel = true;            // Lanczos-2 reconstruction (false = Blackman-Harris, softer)
	float KernelWidth = 1.0f;              // > 1 widens the reconstruction kernel (softer, steadier)
	float MotionHistoryScale = 0.5f;       // history cap multiplier at >= MotionSpeedRange
	float MotionSpeedRange = 32.0f;        // display pixels / frame
	bool bDepthTest = true;                // depth-reprojection disocclusion test
	float DepthTolerance = 0.03f;          // relative view-depth tolerance of that test
	bool bSharpenDenoise = true;           // RCAS noise attenuation (do not sharpen residual noise)
	float MipBiasOffset = -1.0f;           // GetMipBias = log2(Render/Display) + this, clamped
	uint32 DebugMode = 0u;                 // gpu::UpscalerDebug_* (Shared/UpscalerData.h)
};

enum class EToneMapper : uint8
{
	AgX = 0,          // Troy Sobotka's AgX (default; graceful highlight desaturation)
	AcesFitted,       // Hill ACES RRT+ODT fit
	KhronosNeutral,   // Khronos PBR Neutral (preserves base colours, product shots)
	Reinhard,         // luminance Reinhard (reference/debug)
	Linear,           // clamp only (debug)
	Count
};

enum class EAgXLook : uint8
{
	Base = 0,
	Punchy,
	Golden,
	Count
};

struct SPostProcessSettings
{
	// Exposure
	bool bAutoExposure = true;
	float ExposureCompensationEV = 0.0f;
	float ManualExposureEV = 0.0f;       // used when auto exposure is off (EV100-style: L * 2^-EV)
	float MinEV = -16.0f;                // engine light units are relative: enclosed interiors meter very low
	float MaxEV = 20.0f;
	float AdaptationSpeedUp = 3.0f;      // EV per second toward brighter
	float AdaptationSpeedDown = 1.5f;    // toward darker
	// Metered window of the (4x4 block mean) log-luminance histogram. The darkest 30% (deep
	// shadow, where a path-traced estimate is mostly noise) and the brightest 10% (light
	// sources, windows, sun patches) do not steer the exposure. 50-95% exposed the Sponza
	// atrium for its sunlit walls: mean display luma 0.26, 15% of the frame crushed to black;
	// 30-90%: 0.35, 7% crushed, 3% clipped (the window).
	float HistogramLowPercentile = 0.30f;
	float HistogramHighPercentile = 0.90f;
	float AdaptationTransitionEV = 1.5f; // below this distance the linear adaptation eases out exponentially
	float MeteringCenterWeight = 0.6f;   // 0 = average metering, 1 = full centre spot
	float MeteringSpotSize = 0.5f;       // centre spot Gaussian sigma, in half screen heights

	// Bloom: a lens / eye point-spread function. Intensity is the fraction of the light scattered
	// out of the image point (energy conserving: the image is lerped toward the bloom). The
	// pyramid levels are blurs of doubling radius; level i gets weight Falloff^i, so the
	// scattered energy per octave of radius falls by Falloff and the PSF tail goes as
	// r^(log2(Falloff) - 2): 0.5 -> r^-3, like measured veiling glare (Spencer et al. 1995).
	// Falloff 1 (the previous behaviour) puts as much energy at 100 px as at 2 px: a window
	// 11 EV above the interior washed out everything within ~150 px.
	bool bBloom = true;
	float BloomIntensity = 0.04f;
	float BloomFalloff = 0.5f;           // energy ratio between successive pyramid levels (0.1 .. 1)
	float BloomRadius = 1.0f;            // upsample filter radius scale
	uint32 BloomMips = 6u;

	// Tone mapping / grading
	EToneMapper ToneMapper = EToneMapper::AgX;
	EAgXLook AgXLook = EAgXLook::Base;
	float Saturation = 1.0f;
	float Contrast = 1.0f;
	float WhiteBalanceTemperature = 6500.0f;  // scene illuminant to neutralise (lower = cooler image)
	float WhiteBalanceTint = 0.0f;            // [-1, 1], + = magenta (removes a green cast)
	bool bDither = true;
	float VignetteIntensity = 0.0f;
	float ReinhardWhitePoint = 0.0f;          // exposed luminance mapped to white; 0 = plain Reinhard
	float HdrPaperWhiteNits = 200.0f;         // FP16 (scRGB) output only
	// Linear debug views (SPostProcessInputs::bDebugIsLinear): false = show the values exactly
	// (sRGB OETF only; albedo / normals / ids), true = apply exposure + the tone mapper (radiance
	// views such as direct / indirect only). Never bloom or grading.
	bool bDebugToneMap = false;
};

struct SRenderProfile
{
	ERenderMode RenderMode = ERenderMode::Raytracing;
	SRenderSettings Frame;
	EUpscalerKind Upscaler = EUpscalerKind::Temporal;
	SPathTracerSettings PathTracer;
	SRadianceCacheSettings RadianceCache;
	SDenoiserSettings Denoiser;
	STemporalUpscalerSettings Taa;
	SPostProcessSettings Post;
};
}
