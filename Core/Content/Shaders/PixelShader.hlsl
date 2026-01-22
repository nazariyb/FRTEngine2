#include "CoreTypes.hlsli"

Texture2D texture : register(t0);
SamplerState BilinearSampler : register(s0);

PSOutput main(PSInput input)
{
	PSOutput result;

#if defined(FRT_HAS_BASE_COLOR_TEXTURE)
	float4 baseColor = texture.Sample(BilinearSampler, input.uv);
#else
	float4 baseColor = float4(1.f, 1.f, 1.f, 1.f);
#endif

	result.color = baseColor * gDiffuseAlbedo;
	// if (result.color.a < 1.f)
	{
		// Currently, nothing uses alpha chanel, so if it's zero, it's most likely a bug, so we make it noticeable
		// result.color = float4(1.f, 0.f, .6f, 1.f);
		return result;
		// discard;
	}

	result.color = float4(input.uv, 0.f, 1.f);
	return result;
}
