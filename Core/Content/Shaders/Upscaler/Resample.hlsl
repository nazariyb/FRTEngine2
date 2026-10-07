// Passthrough "upscaler" (CPassthroughUpscaler): no temporal accumulation, just a spatial
// resample of the render-resolution HDR colour to the display resolution. Exact copy when the
// sizes match, bilinear otherwise. Output is sanitized like the temporal path, so the post
// chain never sees NaN/Inf from the path tracer.

#include "Common/Bindings.hlsli"
#include "Shared/UpscalerData.h"
#include "Upscaler/UpscalerCommon.hlsli"

ConstantBuffer<SUpscalerResampleConstants> g_Resample : register(b0);

[numthreads(8, 8, 1)]
void main (uint3 DispatchThreadId : SV_DispatchThreadID)
{
	const uint2 pixel = DispatchThreadId.xy;
	if (any(pixel >= g_Resample.OutputSize))
	{
		return;
	}
	Texture2D<float4> source = Tex2D(g_Resample.Input);
	float3 color;
	if (all(g_Resample.InputSize == g_Resample.OutputSize))
	{
		color = source[pixel].rgb;
	}
	else
	{
		const float2 uv = (float2(pixel) + 0.5f) * g_Resample.InvOutputSize;
		color = source.SampleLevel(s_LinearClamp, uv, 0.0f).rgb;
	}
	RwTex2D(g_Resample.Output)[pixel] = float4(SanitizeHdr(color), 1.0f);
}
