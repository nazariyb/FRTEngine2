// Denoiser pass constants (root CBV b2), shared by C++ (CDenoiser, see
// Graphics/Render/Passes/Denoiser.h) and Denoiser/*.hlsl.
//
// One constant block serves every denoiser dispatch of a frame; each shader reads the indices it
// needs. Per-iteration values of the a-trous chain (step size, ping-pong textures) travel as root
// constants (SDenoiserAtrousRootConstants, b0).
#ifndef FRT_DENOISER_DATA_H
#define FRT_DENOISER_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

FRT_GPU_CONST uint DenoiserFlag_GuideHistoryValid = 1u << 0u;     // PrevGuide holds last frame's guide
FRT_GPU_CONST uint DenoiserFlag_DiffuseHistoryValid = 1u << 1u;   // previous diffuse histories usable
FRT_GPU_CONST uint DenoiserFlag_SpecularHistoryValid = 1u << 2u;  // previous specular histories usable
FRT_GPU_CONST uint DenoiserFlag_Diffuse = 1u << 3u;               // denoise diffuse (else pass through)
FRT_GPU_CONST uint DenoiserFlag_Specular = 1u << 4u;              // denoise specular (else pass through)
FRT_GPU_CONST uint DenoiserFlag_VirtualMotion = 1u << 5u;         // specular hit-distance virtual motion
FRT_GPU_CONST uint DenoiserFlag_CatmullRom = 1u << 6u;            // 5-tap bicubic slow-history fetch
FRT_GPU_CONST uint DenoiserFlag_JitterCompensation = 1u << 7u;    // align history to this frame's jitter
FRT_GPU_CONST uint DenoiserFlag_HistoryFix = 1u << 8u;            // wide blur for short histories
FRT_GPU_CONST uint DenoiserFlag_AntiLag = 1u << 10u;              // shorten histories whose 40x40 mean left the noise band
FRT_GPU_CONST uint DenoiserFlag_Firefly = 1u << 11u;              // limit input samples far above last frame's output
FRT_GPU_CONST uint DenoiserFlag_Feedback = 1u << 12u;             // a-trous iteration 0 -> next history (recurrent)
FRT_GPU_CONST uint DenoiserFlag_NoAtrous = 1u << 13u;             // fix pass writes history + outputs (no anti-lag)
FRT_GPU_CONST uint DenoiserFlag_OutputDiffuseOnly = 1u << 14u;    // debug: specular output zeroed
FRT_GPU_CONST uint DenoiserFlag_OutputSpecularOnly = 1u << 15u;   // debug: diffuse output zeroed
FRT_GPU_CONST uint DenoiserFlag_AtrousRotation = 1u << 16u;       // per-block rotated taps on a-trous iterations 1+
FRT_GPU_CONST uint DenoiserFlag_HistoryLengthDebug = 1u << 17u;   // write the history-length debug texture
FRT_GPU_CONST uint DenoiserFlag_FeedbackMoments = 1u << 18u;      // a-trous iteration 0 filters the moments history

// SDenoiserAtrousRootConstants::IterationFlags
FRT_GPU_CONST uint DenoiserAtrous_First = 1u << 0u;               // input = temporal / history-fix result (rgb, length)
FRT_GPU_CONST uint DenoiserAtrous_Last = 1u << 1u;                // output = final denoised textures

// ViewZ at or beyond this is sky (the path tracer writes 1e6 for escaped primary rays).
FRT_GPU_CONST float DenoiserSkyViewZ = 1e5f;

struct SDenoiserConstants
{
	// Noisy inputs (SRV, render resolution).
	uint InDiffuse;               // RGBA16F demodulated diffuse radiance, a = hit distance (0 = none)
	uint InSpecular;              // RGBA16F demodulated specular radiance, a = hit distance
	uint InNormalRoughness;       // RGBA16F world normal, a = linear roughness
	uint InViewZ;                 // R32F linear view depth, >= DenoiserSkyViewZ = sky

	uint InMotionVectors;         // RG16F (previous - current) unjittered screen position, pixels
	uint Flags;                   // DenoiserFlag_*
	uint2 Size;

	float2 InvSize;
	float2 Jitter;                // sample position of pixel p = p + 0.5 + Jitter (render pixels)

	float2 PrevJitter;
	uint FrameIndex;
	uint HistoryDebugSignal;      // 0 = diffuse, 1 = specular history length in the debug texture

	// Previous-frame state (SRV).
	uint PrevGuide;               // RG32_UINT: x = oct normal 12:12 + roughness 8, y = asuint(viewZ)
	uint PrevDiffHistory;         // RGBA16F slow history: rgb, a = history length (frames)
	uint PrevSpecHistory;
	uint PrevFast;                // RGBA16F fast history: (diffuse luminance, diffuse hit distance,
	                              // specular luminance, specular hit distance), per pixel, unfiltered
	uint PrevMoments;             // RGBA16F moments history (written by a-trous iteration 0, filtered with
	                              // DenoiserFlag_FeedbackMoments): diffuse (m1, sqrt m2), specular (m1, sqrt m2)
	uint CurGuide;                // SRV views of this frame's state (read by the later passes)
	uint CurFast;
	uint CurMoments;              // this frame's accumulated (unfiltered) moments, same encoding

	// This frame's state and outputs (UAV).
	uint OutGuide;
	uint OutFast;
	uint OutMoments;
	uint YoungTiles;              // R8_UINT, one texel per 8x8 tile: 1 = the tile has a history to fix (Temporal)

	uint OutDiffHistory;
	uint OutSpecHistory;
	uint OutDiffuse;              // final denoised outputs (RGBA16F rgb, a = denoised hit distance)
	uint OutSpecular;

	// Transients: temporal accumulation result (rgb, length; every pixel) and history-fix result
	// (rgb, length; written only for histories shorter than HistoryFixFrames).
	uint AccumDiff;
	uint AccumSpec;
	uint AccumDiffUav;
	uint AccumSpecUav;

	uint FixedDiff;
	uint FixedSpec;
	uint FixedDiffUav;
	uint FixedSpecUav;

	uint Variance;                // RGBA16F noise texture (Temporal): luminance standard deviation of the accumulated
	                              // signal (diffuse, specular), z = specular filter extent (px), w = specular hit distance
	uint VarianceUav;
	uint HistoryLengthUav;        // R16F debug: history length / MaxHistoryLength
	uint AtrousAux;               // R11G11B10F (specular footprint px, specular hit distance, unused),

	uint AtrousAuxUav;            // written by iteration 0 so later iterations read 4 bytes per centre
	uint OutMomentsHistory;       // UAV of the moments history (iteration 0, or the fix pass without a-trous)
	uint YoungTilesUav;
	uint PadU2;

	uint PrevDiffOutput;          // last frame's final outputs (still intact when the temporal pass runs):
	uint PrevSpecOutput;          // the wide-area local mean the input firefly limit is relative to
	uint AntilagTilesD;           // RGBA32F per 8x8 tile (Temporal): sums of (slow luminance, fast luminance,
	uint AntilagTilesS;           // per-sample variance / fast length, pixel count), read by a-trous iteration 0

	uint AntilagTilesDUav;
	uint AntilagTilesSUav;
	uint PadU6;
	uint PadU7;

	// Camera rays scaled to unit view depth, affine in screen uv (uv = 0 top-left):
	// position relative to the camera = viewZ * (RayC + uv.x * RayX + uv.y * RayY).
	float3 CurRayX;
	float PixelAngle;             // angle subtended by one pixel at the screen centre (radians)
	float3 CurRayY;
	float Pad1;
	float3 CurRayC;
	float Pad2;
	float3 PrevRayX;
	float Pad3;
	float3 PrevRayY;
	float Pad4;
	float3 PrevRayC;
	float Pad5;

	// Inverse of the matrix [PrevRayX PrevRayY PrevRayC]: rows map a position relative to the
	// previous camera to (z * u, z * v, z), i.e. previous screen uv and previous view depth.
	float3 PrevProj0;
	float Pad6;
	float3 PrevProj1;
	float Pad7;
	float3 PrevProj2;
	float Pad8;

	float3 CameraDelta;           // current - previous camera position (world)
	float Pad9;

	// Temporal accumulation.
	float MaxHistoryLength;
	float MaxFastHistoryLength;
	float DisocclusionThreshold;  // plane distance / viewZ for a history tap to count as the same surface
	float ReprojectionNormalCos;  // min cos between current and history normals

	float SpecularParallaxScale;  // multiplies the per-frame view-angle change of the specular lobe test
	float MomentsMinAlpha;        // floor of the moments' blend weight (short moment window)
	float HistoryFixFrames;       // history shorter than this is reconstructed spatially (0 = off)
	float HistoryFixRadius;       // pixels, at history length 1

	// Anti-lag / fireflies.
	float AntilagSigma;           // tile mean change (in standard errors) that counts as a lighting change
	float AntilagSensitivity;     // history-length reduction per AntilagSigma of change
	float FireflyThreshold;       // input luminance limit, multiples of last frame's denoised luminance
	float SpecularLobeFraction;   // fraction of the specular lobe half-angle accepted by normal weights

	// A-trous edge stopping.
	float PhiNormal;              // diffuse: pow(dot(n, n'), PhiNormal)
	float PhiDepth;               // plane distance / viewZ tolerance
	float PhiLuminanceDiffuse;
	float PhiLuminanceSpecular;

	float RoughnessFraction;      // roughness tolerance = 0.02 + RoughnessFraction * roughness
	float SpecularFootprintScale; // specular kernel limit, multiples of the reflection blur footprint
	float LuminanceFloor;         // luminance differences below this fraction of the local mean never stop a tap
	float SpecularNoiseTarget;    // relative deviation the noise-driven specular radius aims for

	// Specular spatial extent beyond the lobe footprint.
	float SpecularNoiseRadiusMax; // pixels (already scaled to the render height)
	float SpecularHitTolerance;   // relative hit-distance difference at which specular taps are rejected
	float InputScale;             // debug: multiplies the noisy inputs (simulated lighting change; 1 = off)
	float Pad13;
};
FRT_GPU_STATIC_ASSERT_SIZE(SDenoiserConstants, 480);

// A-trous iteration (root constants b0).
struct SDenoiserAtrousRootConstants
{
	uint StepSize;                // tap spacing in pixels (1 << iteration)
	uint InDiffuse;               // SRV: (rgb, variance), or the history-fix output (rgb, length) first
	uint InSpecular;
	uint OutDiffuse;              // UAV: (rgb, variance), or the final outputs on the last iteration
	uint OutSpecular;
	uint IterationFlags;          // DenoiserAtrous_*
	uint Pad0;
	uint Pad1;
};
FRT_GPU_STATIC_ASSERT_SIZE(SDenoiserAtrousRootConstants, 32);

FRT_GPU_NAMESPACE_END

#endif // FRT_DENOISER_DATA_H
