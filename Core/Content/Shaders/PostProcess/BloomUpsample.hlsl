// Bloom, upsample chain: Up[i] = lerp(Tent3x3(Up[i + 1]), Down[i], CurrentWeight)   (Up[N - 1] = Down[N - 1]),
// CurrentWeight = w_i / (w_i + ... + w_{N-1}) for the level weights w_i = Falloff^i of the
// point-spread function (CPostProcess::Execute). The derivation below is for equal weights
// (Falloff 1); with weights it is the same with S[i] = w_i Down[i] + Tent(S[i + 1]),
// normalised by the weight total instead of N.
//
// Jimenez 2014: the 3x3 tent of bilinear taps approximates a Gaussian when applied
// progressively, and the radius scale widens it without extra taps. The reference chain sums
// S[i] = Down[i] + Tent(S[i + 1]), so S[0] holds N copies of the image energy and is scaled by
// 1/N at the end. Storing that sum in RGBA16F overflows: every level may reach the exposed clamp
// (MaxExposedValue), so S[N - 2] alone can exceed 65504 and turn into +Inf (and a bilinear tap with
// a zero weight on an Inf texel then yields NaN). Instead each level stores the running mean
// A[i] = S[i] / (N - i), which by linearity of the tent is
//   A[i] = Down[i] / (N - i) + (N - i - 1) / (N - i) * Tent(A[i + 1]) = lerp(Tent(A[i + 1]), Down[i], CurrentWeight).
// Every texel is then a convex combination of the clamped input (never above MaxExposedValue),
// and A[0] is already the energy-normalised bloom the display pass mixes by intensity.

#include "Common/Bindings.hlsli"
#include "PostProcess/PostCommon.hlsli"
#include "Shared/PostProcessData.h"

ConstantBuffer<SBloomPassConstants> g_Pass : register(b2);

[numthreads(8, 8, 1)]
void main (uint3 DispatchThreadId : SV_DispatchThreadID)
{
	const uint2 pixel = DispatchThreadId.xy;
	if (any(pixel >= g_Pass.OutputSize))
	{
		return;
	}

	const float2 uv = (float2(pixel) + 0.5f) * g_Pass.InvOutputSize;
	const float3 lower = Tent3x3(Tex2D(g_Pass.SourceTexture), uv, g_Pass.SourceTexelSize * g_Pass.FilterRadius);
	const float3 current = Tex2D(g_Pass.CurrentTexture).Load(int3(pixel, 0)).rgb;

	RwTex2D(g_Pass.OutputTexture)[pixel] = float4(lerp(lower, current, g_Pass.CurrentWeight), 1.0f);
}
