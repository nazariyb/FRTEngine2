// Bloom, levels 1..N-1: 13-tap downsample (Jimenez 2014) with hardware bilinear taps.
//
// The source level is already exposed, sanitised and firefly-suppressed by BloomPrefilter, so
// plain bilinear fetches are safe here. Weights: centre 4x4 box (D, E, I, J) 0.5, the four
// corner boxes 0.125 each; expanded per tap that is G 0.125, B/F/H/L 0.0625, A/C/K/M 0.03125,
// D/E/I/J 0.125 (sums to 1, energy preserving).
// Taps are placed in UV space from the output pixel centre, which also handles odd source
// sizes (the 2:1 ratio is then only approximate and the filter adapts smoothly).

#include "Common/Bindings.hlsli"
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

	Texture2D<float4> source = Tex2D(g_Pass.SourceTexture);
	const float2 uv = (float2(pixel) + 0.5f) * g_Pass.InvOutputSize;
	const float2 ts = g_Pass.SourceTexelSize;

	const float3 a = source.SampleLevel(s_LinearClamp, uv + ts * float2(-2.0f, -2.0f), 0.0f).rgb;
	const float3 b = source.SampleLevel(s_LinearClamp, uv + ts * float2(0.0f, -2.0f), 0.0f).rgb;
	const float3 c = source.SampleLevel(s_LinearClamp, uv + ts * float2(2.0f, -2.0f), 0.0f).rgb;
	const float3 d = source.SampleLevel(s_LinearClamp, uv + ts * float2(-1.0f, -1.0f), 0.0f).rgb;
	const float3 e = source.SampleLevel(s_LinearClamp, uv + ts * float2(1.0f, -1.0f), 0.0f).rgb;
	const float3 f = source.SampleLevel(s_LinearClamp, uv + ts * float2(-2.0f, 0.0f), 0.0f).rgb;
	const float3 g = source.SampleLevel(s_LinearClamp, uv, 0.0f).rgb;
	const float3 h = source.SampleLevel(s_LinearClamp, uv + ts * float2(2.0f, 0.0f), 0.0f).rgb;
	const float3 i = source.SampleLevel(s_LinearClamp, uv + ts * float2(-1.0f, 1.0f), 0.0f).rgb;
	const float3 j = source.SampleLevel(s_LinearClamp, uv + ts * float2(1.0f, 1.0f), 0.0f).rgb;
	const float3 k = source.SampleLevel(s_LinearClamp, uv + ts * float2(-2.0f, 2.0f), 0.0f).rgb;
	const float3 l = source.SampleLevel(s_LinearClamp, uv + ts * float2(0.0f, 2.0f), 0.0f).rgb;
	const float3 m = source.SampleLevel(s_LinearClamp, uv + ts * float2(2.0f, 2.0f), 0.0f).rgb;

	float3 result = g * 0.125f;
	result += (a + c + k + m) * 0.03125f;
	result += (b + f + h + l) * 0.0625f;
	result += (d + e + i + j) * 0.125f;

	RwTex2D(g_Pass.OutputTexture)[pixel] = float4(result, 1.0f);
}
