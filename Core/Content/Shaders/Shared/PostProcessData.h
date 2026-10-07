// Post-process (auto exposure, bloom, display transform) constants and the exposure buffer
// layout, shared by C++ (CPostProcess) and the shaders under PostProcess/.
#ifndef FRT_POST_PROCESS_DATA_H
#define FRT_POST_PROCESS_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------------------------
// Exposure buffer (raw, persistent). Written once per frame by ExposureAdapt.hlsl; any pass may
// read it through ByteAddressBuffer (see LoadExposureState below). Byte layout:
//   [0, 32)                  SExposureState
//   [32, 32 + 256 * 4)       snapshot of this frame's weighted luminance histogram (uint bins)
// Exposure is relative to *scene-referred* radiance. A pass reading a texture that carries
// g_Frame.PreExposure must apply Exposure / PreExposure.
// ---------------------------------------------------------------------------------------------
FRT_GPU_CONST uint ExposureHistogramBins = 256u;
FRT_GPU_CONST uint ExposureStateBytes = 32u;
FRT_GPU_CONST uint ExposureHistogramSnapshotOffset = 32u;
FRT_GPU_CONST uint ExposureBufferBytes = 32u + 256u * 4u;
FRT_GPU_CONST uint ExposureStateValidMagic = 0x45585031u;   // 'EXP1'
// Fixed-point scale of the per-pixel metering weight (6 bits). 2^32 / 64 = 67M pixels of
// headroom per bin, i.e. 8K is safe even when every pixel lands in the same bin.
FRT_GPU_CONST uint ExposureWeightScale = 64u;

// EV100 of a reflected-light meter (ISO 100, calibration K = 12.5):
// EV100 = log2(L * 100 / 12.5) = log2(L) + 3 (Lagarde & de Rousiers 2014, "Moving Frostbite to
// PBR", sec. 5.1). Exposure multiplier = 2^(Compensation - EV100) so the metered average lands
// at 1/8 before tone mapping.
FRT_GPU_CONST float ExposureLog2CalibrationOffset = 3.0f;

FRT_GPU_CONST uint ExposureFlag_Auto = 1u << 0u;        // meter + adapt (else ManualEV)
FRT_GPU_CONST uint ExposureFlag_Reset = 1u << 1u;       // jump to target, no adaptation
FRT_GPU_CONST uint ExposureFlag_ClearOnly = 1u << 2u;   // only zero the histogram bins

struct SExposureState
{
	float Exposure;               // multiplier for scene-referred radiance, compensation included
	float AdaptedEV;              // EV100 after temporal adaptation (no compensation)
	float TargetEV;               // EV100 metered this frame (clamped to [MinEV, MaxEV])
	float AverageLog2Luminance;   // percentile-trimmed mean of log2 luminance (unclamped)

	float PreviousExposure;       // last frame's Exposure (history rescaling)
	float MeteredWeight;          // total histogram weight (0 = nothing metered)
	uint Valid;                   // ExposureStateValidMagic once written
	uint FrameIndex;              // g_Frame.FrameIndex of the write
};
FRT_GPU_STATIC_ASSERT_SIZE(SExposureState, 32);

struct SExposurePassConstants
{
	uint InputTexture;            // HDR colour SRV (display resolution)
	uint HistogramBuffer;         // RWByteAddressBuffer, 256 uint bins
	uint ExposureBuffer;          // RWByteAddressBuffer, ExposureBufferBytes
	uint Flags;                   // ExposureFlag_*

	uint2 InputSize;
	float2 InvInputSize;

	float MinLog2Luminance;       // histogram range, log2 of scene luminance
	float Log2LuminanceRange;
	float InvLog2LuminanceRange;
	float CenterWeight;           // 0 = average metering, 1 = full centre spot

	float LowPercentile;          // trimmed-mean window, fractions of the total weight
	float HighPercentile;
	float MinEV;
	float MaxEV;

	float SpeedUp;                // EV / s toward a brighter scene (higher EV)
	float SpeedDown;              // EV / s toward a darker scene
	float TransitionEV;           // linear -> exponential adaptation hand-over distance
	float CompensationEV;

	float ManualEV;
	float HalfHeightInv;          // 2 / InputSize.y (centre-weight coordinates)
	float SpotInvTwoSigmaSq;      // 1 / (2 sigma^2) of the centre Gaussian
	float FallbackDeltaTime;      // CPU-measured frame time, used while g_Frame.DeltaTime is 0
};
FRT_GPU_STATIC_ASSERT_SIZE(SExposurePassConstants, 96);

// ---------------------------------------------------------------------------------------------
// Bloom (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare").
// ---------------------------------------------------------------------------------------------
struct SBloomPassConstants
{
	uint SourceTexture;           // SRV: level above (prefilter: HDR colour)
	uint CurrentTexture;          // upsample: downsample of the output level (SRV)
	uint OutputTexture;           // UAV
	uint ExposureBuffer;          // raw SRV (prefilter only)

	uint2 SourceSize;
	uint2 OutputSize;

	float2 SourceTexelSize;       // 1 / SourceSize
	float2 InvOutputSize;

	float FilterRadius;           // upsample tent radius in source texels
	float FallbackExposure;       // used while the exposure buffer is not valid yet
	float MaxExposedValue;        // clamp of exposed input (keeps half precision finite)
	float CurrentWeight;          // upsample: w_i / (w_i + ... + w_{N-1}), w_i = Falloff^i (level weights of the PSF)
};
FRT_GPU_STATIC_ASSERT_SIZE(SBloomPassConstants, 64);

// ---------------------------------------------------------------------------------------------
// Final display pass (full-screen triangle).
// ---------------------------------------------------------------------------------------------
FRT_GPU_CONST uint ToneMapper_AgX = 0u;
FRT_GPU_CONST uint ToneMapper_AcesFitted = 1u;
FRT_GPU_CONST uint ToneMapper_KhronosNeutral = 2u;
FRT_GPU_CONST uint ToneMapper_Reinhard = 3u;
FRT_GPU_CONST uint ToneMapper_Linear = 4u;

FRT_GPU_CONST uint AgXLook_Base = 0u;
FRT_GPU_CONST uint AgXLook_Punchy = 1u;
FRT_GPU_CONST uint AgXLook_Golden = 2u;

FRT_GPU_CONST uint FinalFlag_Bloom = 1u << 0u;
FRT_GPU_CONST uint FinalFlag_Dither = 1u << 1u;
FRT_GPU_CONST uint FinalFlag_Debug = 1u << 2u;            // show DebugTexture instead
FRT_GPU_CONST uint FinalFlag_DebugLinear = 1u << 3u;      // debug is scene-linear: tone map it
FRT_GPU_CONST uint FinalFlag_HdrSameSize = 1u << 4u;      // HDR matches the output: Load()
FRT_GPU_CONST uint FinalFlag_SrgbRenderTarget = 1u << 5u; // RTV is *_SRGB: hand back linear
FRT_GPU_CONST uint FinalFlag_ScRgbOutput = 1u << 6u;      // FP16 RTV: scRGB, no OETF/dither
FRT_GPU_CONST uint FinalFlag_Grading = 1u << 7u;          // colour matrix / contrast active
FRT_GPU_CONST uint FinalFlag_Vignette = 1u << 8u;
FRT_GPU_CONST uint FinalFlag_HasHdr = 1u << 9u;
FRT_GPU_CONST uint FinalFlag_DebugToneMap = 1u << 10u;    // linear debug: exposure + tone mapper

struct SPostFinalConstants
{
	float4 ColorMatrixR;          // white balance (Bradford) x saturation, linear Rec.709 rows
	float4 ColorMatrixG;
	float4 ColorMatrixB;

	uint HdrTexture;
	uint BloomTexture;
	uint ExposureBuffer;
	uint DebugTexture;

	float2 OutputSize;
	float2 InvOutputSize;

	float2 BloomTapOffset;        // tent offset: BloomRadius / bloom texture size
	float BloomIntensity;         // lerp weight; the pyramid is already energy-normalised
	float Pad2;

	float Contrast;
	float VignetteStrength;
	float AspectRatio;
	float ReinhardWhitePoint;     // 0 = plain Reinhard

	uint ToneMapper;              // ToneMapper_*
	uint AgXLook;                 // AgXLook_*
	uint Flags;                   // FinalFlag_*
	float QuantizationLevels;     // 255 (8-bit) / 1023 (10-bit)

	float FallbackExposure;
	float ScRgbScale;             // paper white / 80 nits
	float Pad0;
	float Pad1;
};
FRT_GPU_STATIC_ASSERT_SIZE(SPostFinalConstants, 144);

FRT_GPU_NAMESPACE_END

#ifndef __cplusplus
// Exposure state from the persistent exposure buffer (uniform index).
SExposureState LoadExposureState (uint ExposureBuffer)
{
	ByteAddressBuffer buffer = ResourceDescriptorHeap[ExposureBuffer];
	return buffer.Load<SExposureState>(0u);
}

// Exposure multiplier, or Fallback until the buffer has been written once.
float LoadExposure (uint ExposureBuffer, float Fallback)
{
	if (ExposureBuffer == FRT_INVALID_INDEX)
	{
		return Fallback;
	}
	const SExposureState state = LoadExposureState(ExposureBuffer);
	// isfinite() is an explicit DXIL op: DXC emits 'fast' compares, so '> 0' alone may not
	// reject NaN once the driver applies no-NaN optimisations.
	const bool bValid = state.Valid == ExposureStateValidMagic && isfinite(state.Exposure) && state.Exposure > 0.0f;
	return bValid ? state.Exposure : Fallback;
}
#endif

#endif // FRT_POST_PROCESS_DATA_H
