// Bakes the procedural sky into an equal-area octahedral environment map and builds the
// luminance pyramid used for importance sampling (see Common/Sky.hlsli).

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "Common/Packing.hlsli"
#include "Shared/SkyBakeData.h"

ConstantBuffer<SSkyBakeConstants> g_Bake : register(b2);

float3 ProceduralSky (float3 Dir)
{
	const float softness = max(g_Bake.HorizonSoftness, 1e-4f);
	const float skyT = smoothstep(-softness, softness, Dir.y);
	const float zenithT = saturate(Dir.y);
	const float3 skyColor = lerp(g_Bake.Horizon.rgb, g_Bake.Zenith.rgb, zenithT);
	return lerp(g_Bake.Ground.rgb, skyColor * g_Bake.Intensity, skyT);
}

[numthreads(8, 8, 1)]
void BakeEnvironment (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Bake.Size))
	{
		return;
	}
	// Supersample each texel 4x4 so the map is a proper box-filtered radiance average, which
	// keeps the importance pyramid consistent with what bilinear lookups return.
	float3 radiance = 0.0f;
	[unroll]
	for (uint sy = 0; sy < 4; ++sy)
	{
		[unroll]
		for (uint sx = 0; sx < 4; ++sx)
		{
			const float2 uv = (float2(Id.xy) + (float2(sx, sy) + 0.5f) / 4.0f) / float(g_Bake.Size);
			radiance += ProceduralSky(EqualAreaSquareToSphere(uv));
		}
	}
	radiance /= 16.0f;
	RwTex2D(g_Bake.OutEnvironment)[Id.xy] = float4(radiance, 1.0f);
	RwTex2DFloat(g_Bake.OutLuminance)[Id.xy] = max(Luminance(radiance), 0.0f) + g_Bake.LuminanceFloor;
}

[numthreads(8, 8, 1)]
void BuildPyramid (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Bake.Size))
	{
		return;
	}
	RWTexture2D<float> src = RwTex2DFloat(g_Bake.InLuminance);
	const uint2 p = Id.xy * 2u;
	const float avg = 0.25f * (src[p] + src[p + uint2(1, 0)] + src[p + uint2(0, 1)] + src[p + uint2(1, 1)]);
	RwTex2DFloat(g_Bake.OutLuminance)[Id.xy] = avg;
}
