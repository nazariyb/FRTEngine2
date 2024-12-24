#include "CoreTypes.hlsli"

Texture2D texture : register(t0);
SamplerState BilinearSampler : register(s0);

PSOutput main(PSInput input)
{
	PSOutput result;

	result.color = texture.Sample(BilinearSampler, input.uv);
	if (result.color.a == 0.f)
	{
		discard;
	}

	// result.color = float4(input.uv, 0.f, 1.f);
	// result.color = float4(1.f, 1.f, 1.f, 1.f);
	return result;
}
