#ifndef FRT_UPSCALER_COMMON_HLSLI
#define FRT_UPSCALER_COMMON_HLSLI

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"

// Largest value written to RGBA16F targets (half max is 65504; keep a margin so filtering
// can never round up to +Inf).
static const float kUpscalerMaxHdr = 65000.0f;

// ViewZ at or beyond this is sky (the path tracer writes 1e6 for escaped primary rays).
static const float kSkyViewZ = 1e5f;
static const float kInvalidViewZ = 1e6f;

// NaN / Inf / negative -> 0, then clamp to the half range. Explicit test first: relying on
// min/max NaN semantics alone would turn +Inf into a 65000 firefly that TAA then smears.
// Vector isfinite() instead of IsFiniteFloat3(): its scalar && short-circuits into branches.
float3 SanitizeHdr (float3 C)
{
	return all(isfinite(C)) ? clamp(C, 0.0f, kUpscalerMaxHdr) : 0.0f;
}

float4 SanitizeHdr4 (float4 C)
{
	return all(isfinite(C)) ? clamp(C, 0.0f, kUpscalerMaxHdr) : 0.0f;
}

float SanitizeViewZ (float Z)
{
	return (isfinite(Z) && Z > 0.0f) ? Z : kInvalidViewZ;
}

// Hue-preserving, exactly invertible Reinhard on the max channel ("tonemap before resolve",
// Karis 2014, "High Quality Temporal Supersampling"). Neighbourhood filtering, variance
// statistics and sharpening run on these [0, 1) values so a single HDR highlight cannot
// dominate a 3x3 filter or inflate the clipping box.
float3 TonemapMax (float3 C)
{
	return C * rcp(1.0f + MaxComponent(C));
}

// Floor of 1 - max(t) in the inverse. It caps the representable exposure-scaled value at
// 1 / kTonemapEpsilon (~1e6). 1/65536 capped it at 65536, which cut a sun disk (half-float max
// ~65000) seen from an interior with exposure 10 down to 6.5e3, i.e. 10x less bloom. Below
// ~1e6 the float32 rounding of t costs < 6% of the value.
static const float kTonemapEpsilon = 1.0f / 1048576.0f;

float3 InverseTonemapMax (float3 C)
{
	// t = c / (1 + max(c))  =>  max(t) = m / (1 + m)  =>  c = t / (1 - max(t)).
	// Singular as max(t) -> 1, so the callers must keep t inside the hull of real samples
	// (each strictly < 1). A value interpolated or sharpened toward 1 gets amplified without
	// bound: 0.999 already decodes to 999.
	return C * rcp(max(1.0f - MaxComponent(C), kTonemapEpsilon));
}

// Working space of the temporal resolve: YCoCg of the exposure-scaled, max-Reinhard colour.
// YCoCg decorrelates luma from chroma, which makes the variance box much tighter around the
// colour distribution than an RGB box (Karis 2014; Salvi 2016).
float3 ToWorkingSpace (float3 LinearRgb, float Exposure)
{
	return RgbToYCoCg(TonemapMax(LinearRgb * Exposure));
}

// Tonemapped RGB (already range-limited by the caller) back to linear HDR.
float3 FromTonemapped (float3 TonemappedRgb, float InvExposure)
{
	return InverseTonemapMax(max(TonemappedRgb, 0.0f)) * InvExposure;
}

#endif
