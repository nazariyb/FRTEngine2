#ifndef FRT_POST_COMMON_HLSLI
#define FRT_POST_COMMON_HLSLI

#include "Common/Bindings.hlsli"

// 1 / g_Frame.PreExposure, guarded: HDR inputs carry PreExposure, metering and display divide it out.
float GetInvPreExposure ()
{
	const float preExposure = g_Frame.PreExposure;
	return (isfinite(preExposure) && preExposure > 0.0f) ? 1.0f / preExposure : 1.0f;
}

// NaN -> 0, negatives -> 0, +Inf -> half max (an upstream Inf is a very bright pixel, not a hole).
float3 SanitizeHdr (float3 C)
{
	C = select(isnan(C), 0.0f, C);
	return clamp(C, 0.0f, 65504.0f);
}

// 3x3 tent of bilinear taps (1 2 1 / 2 4 2 / 1 2 1, / 16): Jimenez 2014 bloom upsample filter.
// Offset is the tap spacing in UV (radius x source texel size).
float3 Tent3x3 (Texture2D<float4> Source, float2 Uv, float2 Offset)
{
	float3 sum = Source.SampleLevel(s_LinearClamp, Uv, 0.0f).rgb * 4.0f;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(0.0f, -Offset.y), 0.0f).rgb * 2.0f;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(-Offset.x, 0.0f), 0.0f).rgb * 2.0f;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(Offset.x, 0.0f), 0.0f).rgb * 2.0f;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(0.0f, Offset.y), 0.0f).rgb * 2.0f;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(-Offset.x, -Offset.y), 0.0f).rgb;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(Offset.x, -Offset.y), 0.0f).rgb;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(-Offset.x, Offset.y), 0.0f).rgb;
	sum += Source.SampleLevel(s_LinearClamp, Uv + float2(Offset.x, Offset.y), 0.0f).rgb;
	return sum * (1.0f / 16.0f);
}

#endif
