#ifndef FRT_TONE_MAPPING_HLSLI
#define FRT_TONE_MAPPING_HLSLI

// Display rendering transforms. Every operator takes exposed, scene-linear Rec.709 radiance
// and returns display-linear Rec.709 in [0, 1]; the caller applies the output encoding.

#include "Common/Color.hlsli"
#include "Shared/PostProcessData.h"

// ITU-R BT.2087 linear Rec.709 <-> Rec.2020 (D65 both sides), rows for mul(M, v).
static const float3x3 kRec709ToRec2020 = float3x3(
	0.627403895934699f, 0.329283038377884f, 0.043313065687417f,
	0.069097289358232f, 0.919540395075459f, 0.011362315566309f,
	0.016391438875150f, 0.088013307877226f, 0.895595253247624f);
static const float3x3 kRec2020ToRec709 = float3x3(
	1.660491002108435f, -0.587641138788550f, -0.072849863319885f,
	-0.124550474521591f, 1.132899897125960f, -0.008349422604369f,
	-0.018150763354905f, -0.100578898008007f, 1.118729661362913f);

// ---------------------------------------------------------------------------------------------
// AgX (Troy Sobotka), as shipped by Blender 4 / Filament / three.js: Rec.2020 working space,
// inset matrix (primaries pulled toward the achromatic axis so bright saturated colours path to
// white instead of clipping to a hue-skewed primary), log2 encoding over [-12.47, 4.03] EV
// around 0.18 (= 2^-10 .. 2^6.5 x middle grey), the 6th-order polynomial fit of the AgX base
// contrast sigmoid (B. Wrensch 2023, mean squared error 3.67e-6), optional ASC CDL look, outset
// matrix (restores part of the chroma removed by the inset - not the inverse), and the 2.2
// power that linearises AgX's display-encoded output.
// Rows below are the transposes of the column-major GLSL / Filament literals.
// ---------------------------------------------------------------------------------------------
static const float3x3 kAgXInset = float3x3(
	0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f,
	0.137318972929847f, 0.761241990602591f, 0.101439036467562f,
	0.11189821299995f, 0.0767994186031903f, 0.811302368396859f);
static const float3x3 kAgXOutset = float3x3(
	1.1271005818144368f, -0.11060664309660323f, -0.016493938717834573f,
	-0.1413297634984383f, 1.157823702216272f, -0.016493938717834257f,
	-0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f);

float3 AgXDefaultContrastApprox (float3 X)
{
	const float3 x2 = X * X;
	const float3 x4 = x2 * x2;
	return 15.5f * x4 * x2
		- 40.14f * x4 * X
		+ 31.96f * x4
		- 6.868f * x2 * X
		+ 0.4298f * x2
		+ 0.1191f * X
		- 0.00232f;
}

// ASC CDL looks from Blender's AgX OCIO config, applied in AgX's encoded space (Filament order:
// luma is taken before the CDL).
float3 AgXLook (float3 V, uint Look)
{
	if (Look == AgXLook_Base)
	{
		return V;
	}
	float3 slope = 1.0f;
	float3 power = 1.0f;
	float saturation = 1.0f;
	if (Look == AgXLook_Golden)
	{
		slope = float3(1.0f, 0.9f, 0.5f);
		power = 0.8f;
		saturation = 1.3f;
	}
	else // Punchy
	{
		power = 1.35f;
		saturation = 1.4f;
	}
	const float luma = dot(V, float3(0.2126f, 0.7152f, 0.0722f));
	// The sigmoid fit dips to -0.0023 at 0: clamp before the fractional power.
	V = pow(max(V * slope, 0.0f), power);
	return luma + saturation * (V - luma);
}

float3 ToneMapAgX (float3 Color, uint Look)
{
	const float kMinEv = -12.47393f;   // log2(2^-10 * 0.18)
	const float kMaxEv = 4.026069f;    // log2(2^6.5 * 0.18)

	float3 v = mul(kRec709ToRec2020, max(Color, 0.0f));
	v = mul(kAgXInset, v);
	v = log2(max(v, 1e-10f));
	v = saturate((v - kMinEv) * (1.0f / (kMaxEv - kMinEv)));
	v = AgXDefaultContrastApprox(v);
	v = AgXLook(v, Look);
	v = mul(kAgXOutset, v);
	v = pow(max(v, 0.0f), 2.2f);
	v = mul(kRec2020ToRec709, v);
	// Gamut mapping: plain clip (the inset already keeps most values inside Rec.709).
	return saturate(v);
}

// ---------------------------------------------------------------------------------------------
// ACES (RRT + sRGB ODT), Stephen Hill's fit (BakingLab, 2016): sRGB -> AP1 with the RRT
// saturation folded in, rational fit of RRT+ODT, ODT saturation -> sRGB.
// ---------------------------------------------------------------------------------------------
static const float3x3 kAcesInput = float3x3(
	0.59719f, 0.35458f, 0.04823f,
	0.07600f, 0.90834f, 0.01566f,
	0.02840f, 0.13383f, 0.83777f);
static const float3x3 kAcesOutput = float3x3(
	1.60475f, -0.53108f, -0.07367f,
	-0.10208f, 1.10813f, -0.00605f,
	-0.00327f, -0.07276f, 1.07602f);

float3 RrtAndOdtFit (float3 V)
{
	const float3 a = V * (V + 0.0245786f) - 0.000090537f;
	const float3 b = V * (0.983729f * V + 0.4329510f) + 0.238081f;
	return a / b;
}

float3 ToneMapAcesFitted (float3 Color)
{
	float3 v = mul(kAcesInput, max(Color, 0.0f));
	v = RrtAndOdtFit(v);
	v = mul(kAcesOutput, v);
	return saturate(v);
}

// ---------------------------------------------------------------------------------------------
// Khronos PBR Neutral (Khronos 3D Commerce, 2024) - exact reference implementation: identity
// below 0.76 apart from a small toe offset, hyperbolic highlight compression to 1, then a
// desaturation toward white proportional to the compression.
// ---------------------------------------------------------------------------------------------
float3 ToneMapKhronosNeutral (float3 Color)
{
	const float startCompression = 0.8f - 0.04f;
	const float desaturation = 0.15f;

	Color = max(Color, 0.0f);
	const float x = min(Color.r, min(Color.g, Color.b));
	const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
	Color -= offset;

	const float peak = max(Color.r, max(Color.g, Color.b));
	if (peak < startCompression)
	{
		return saturate(Color);
	}

	const float d = 1.0f - startCompression;
	const float newPeak = 1.0f - d * d / (peak + d - startCompression);
	Color *= newPeak / peak;

	const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
	return saturate(lerp(Color, newPeak.xxx, g));
}

// ---------------------------------------------------------------------------------------------
// Reinhard on luminance (Reinhard et al. 2002, "Photographic Tone Reproduction", eq. 3; with
// WhitePoint > 0 the extended eq. 4 that maps L_white to 1). Scaling RGB by Ld / L keeps the
// chromaticity; channels pushed above 1 are clipped.
// ---------------------------------------------------------------------------------------------
float3 ToneMapReinhard (float3 Color, float WhitePoint)
{
	Color = max(Color, 0.0f);
	const float l = Luminance(Color);
	const float lw2 = WhitePoint > 0.0f ? WhitePoint * WhitePoint : 0.0f;
	const float ld = lw2 > 0.0f ? l * (1.0f + l / lw2) / (1.0f + l) : l / (1.0f + l);
	return saturate(Color * (ld / max(l, 1e-8f)));
}

float3 ApplyToneMapper (float3 Color, uint ToneMapper, uint Look, float ReinhardWhitePoint)
{
	// Exposure (up to 2^40) x contrast (power up to 4) can overflow to +Inf, and the rational
	// operators then evaluate Inf / Inf = NaN (ACES fit, Reinhard, Khronos' peak rescale) -> black
	// pixels. 1e9 is far beyond every operator's white point; fmax / fmin also flush NaN to 0.
	Color = clamp(Color, 0.0f, 1e9f);
	switch (ToneMapper)
	{
		case ToneMapper_AgX: return ToneMapAgX(Color, Look);
		case ToneMapper_AcesFitted: return ToneMapAcesFitted(Color);
		case ToneMapper_KhronosNeutral: return ToneMapKhronosNeutral(Color);
		case ToneMapper_Reinhard: return ToneMapReinhard(Color, ReinhardWhitePoint);
		default: return saturate(Color);
	}
}

#endif
