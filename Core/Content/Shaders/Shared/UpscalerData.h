// Upscaler pass constants, shared by C++ (CTemporalUpscaler / CPassthroughUpscaler, see
// Graphics/Render/Passes/Upscaler.h) and Upscaler/*.hlsl.
#ifndef FRT_UPSCALER_DATA_H
#define FRT_UPSCALER_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

FRT_GPU_CONST uint UpscalerFlag_HistoryValid = 1u << 0u;     // InHistory holds last frame's resolve
FRT_GPU_CONST uint UpscalerFlag_PrevDepthValid = 1u << 1u;   // InPrevViewZ holds last frame's ViewZ
FRT_GPU_CONST uint UpscalerFlag_CatmullRom = 1u << 2u;       // 5-tap bicubic history fetch (else bilinear)
FRT_GPU_CONST uint UpscalerFlag_VarianceClip = 1u << 3u;     // YCoCg variance clip (else min/max clamp)
FRT_GPU_CONST uint UpscalerFlag_AntiFlicker = 1u << 4u;      // Karis luminance-weighted blend (biased dark on sparse input)
FRT_GPU_CONST uint UpscalerFlag_LanczosKernel = 1u << 5u;    // Lanczos-2 on clean pixels, Blackman-Harris on noisy ones (else Blackman-Harris only)
FRT_GPU_CONST uint UpscalerFlag_DepthTest = 1u << 6u;        // depth-reprojection disocclusion test

FRT_GPU_CONST uint UpscalerDebug_None = 0u;
FRT_GPU_CONST uint UpscalerDebug_HistoryWeight = 1u;         // accumulated confidence / max (grey)
FRT_GPU_CONST uint UpscalerDebug_Disocclusion = 2u;          // r = rejected history (depth / off-screen), g = spatial confidence, b = rectification (1 - clip factor)
FRT_GPU_CONST uint UpscalerDebug_MotionVectors = 3u;         // |dilated MV| in display pixels * 0.1
FRT_GPU_CONST uint UpscalerDebug_Count = 4u;

FRT_GPU_CONST uint UpscalerSharpenFlag_Denoise = 1u << 0u;   // RCAS noise attenuation

// Temporal resolve (root CBV b2). One dispatch over the display resolution.
struct STemporalUpscalerConstants
{
	uint InColor;                 // SRV render res, linear HDR (RGBA16F)
	uint InViewZ;                 // SRV render res, R32F linear view depth (>= 1e5 = sky)
	uint InMotionVectors;         // SRV render res, RG16F (previous - current), render pixels
	uint InPrevViewZ;             // SRV render res, last frame's ViewZ (valid if UpscalerFlag_PrevDepthValid)

	uint InHistory;               // SRV display res RGBA16F: rgb linear HDR, a = accumulated confidence
	uint OutHistory;              // UAV display res RGBA16F, same layout
	uint OutColor;                // UAV display res; FRT_INVALID_INDEX when the sharpen pass follows
	uint Flags;                   // UpscalerFlag_*

	float2 RenderSize;
	float2 InvRenderSize;

	float2 DisplaySize;
	float2 InvDisplaySize;

	float2 Jitter;                // render pixels, sample position = pixel + 0.5 + Jitter
	float2 PrevJitter;            // jitter InPrevViewZ was rendered with

	float2 KernelScale;           // kernel compression on accumulated pixels (1 = new-sample width)
	float Exposure;               // exposure-scaled working space (tonemapped neighbourhood, Karis weights)
	float InvExposure;

	float MaxHistoryWeight;       // cap of the accumulated confidence (static pixels): (1/HistoryBlend - 1) * E[confidence], Lanczos kernel
	float MotionHistoryScale;     // cap multiplier at >= MotionSpeedRange display px / frame
	float InvMotionSpeedRange;
	float VarianceGamma;

	float DepthTolerance;         // relative view-depth tolerance of the disocclusion test
	uint DebugMode;               // UpscalerDebug_*
	float2 RenderScale;           // Render / Display per axis, computed exactly on the CPU (1.0 at native)

	uint InHistoryMoments;        // SRV display res RGBA16F: temporal moments of the working-space input (rgb = E[w], a = Var[w.Y])
	uint OutHistoryMoments;       // UAV display res RGBA16F, same layout
	float TemporalGamma;          // rectification slack in temporal standard deviations of the pixel's own input
	float MaxHistoryWeightGaussian; // MaxHistoryWeight for the Blackman-Harris kernel (noisy pixels; = MaxHistoryWeight when Lanczos is off)
};
FRT_GPU_STATIC_ASSERT_SIZE(STemporalUpscalerConstants, 144);

// RCAS sharpening (root constants b0).
struct SUpscalerSharpenConstants
{
	uint Input;                   // SRV display res (resolved, unsharpened)
	uint Output;                  // UAV display res
	uint2 Size;

	float Sharpness;              // 0..1 lobe scale (1 = RCAS maximum)
	float Exposure;
	float InvExposure;
	uint Flags;                   // UpscalerSharpenFlag_*
};
FRT_GPU_STATIC_ASSERT_SIZE(SUpscalerSharpenConstants, 32);

// Spatial resample of the passthrough upscaler (root constants b0).
struct SUpscalerResampleConstants
{
	uint Input;                   // SRV render res
	uint Output;                  // UAV display res
	uint2 OutputSize;

	float2 InvOutputSize;
	uint2 InputSize;
};
FRT_GPU_STATIC_ASSERT_SIZE(SUpscalerResampleConstants, 32);

FRT_GPU_NAMESPACE_END

#endif
