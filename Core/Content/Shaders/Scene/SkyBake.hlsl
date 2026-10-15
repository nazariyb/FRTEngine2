// Bakes the procedural sky into an equal-area octahedral environment map and builds the
// luminance pyramid used for importance sampling (see Common/Sky.hlsli).

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
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

// ------------------------------------------------------------------------------------------
// Image-based lighting for the raster preview, which cannot trace the sky:
//   PrefilterSpecular  - split-sum radiance (Karis 2013): the environment integrated against a
//                        GGX lobe around R with N = V = R, weighted by NoL, one slice of a 3D
//                        texture per perceptual roughness step;
//   ConvolveIrradiance - E(n) = integral of L(w) max(0, n.w) dw, a cosine convolution.
// Both read the baked environment (smooth: the solar disk is its own light, not in the map).
// ------------------------------------------------------------------------------------------

cbuffer FilterConstants : register(b0)
{
	uint g_FilterInput;        // Texture2D<float4>, the baked environment
	uint g_FilterOutput;       // RWTexture3D<float4> (prefilter) or RWTexture2D<float4> (irradiance)
	uint g_FilterSize;         // output width = height
	uint g_FilterSlices;       // prefilter: roughness slices
};

float3 SampleEnvironmentMap (float3 Dir)
{
	Texture2D<float4> env = ResourceDescriptorHeap[g_FilterInput];
	return env.SampleLevel(s_LinearClamp, EqualAreaSphereToSquare(Dir), 0.0f).rgb;
}

[numthreads(8, 8, 1)]
void PrefilterSpecular (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_FilterSize) || Id.z >= g_FilterSlices)
	{
		return;
	}
	RWTexture3D<float4> output = ResourceDescriptorHeap[g_FilterOutput];
	const float2 uv = (float2(Id.xy) + 0.5f) / float(g_FilterSize);
	const float3 R = EqualAreaSquareToSphere(uv);
	const float roughness = float(Id.z) / float(max(g_FilterSlices, 2u) - 1u);
	const float alpha = roughness * roughness;
	if (alpha < 1e-3f)
	{
		// Mirror slice: the environment box-filtered over this texel (2x2 source texels).
		output[Id] = float4(SampleEnvironmentMap(R), 1.0f);
		return;
	}

	float3 T, B;
	BuildOrthonormalBasis(R, T, B);
	const uint sampleCount = 128u;   // the sky is a smooth gradient: a stratified set suffices
	float3 sum = 0.0f;
	float weight = 0.0f;
	for (uint i = 0u; i < sampleCount; ++i)
	{
		// Hammersley point -> GGX normal distribution around R (V = N = R).
		const float2 u = float2((float(i) + 0.5f) / float(sampleCount), float(reversebits(i)) * 2.3283064365386963e-10f);
		const float cosTheta = sqrt((1.0f - u.x) / (1.0f + (alpha * alpha - 1.0f) * u.x));
		const float sinTheta = SafeSqrt(1.0f - cosTheta * cosTheta);
		const float phi = kTwoPi * u.y;
		const float3 H = sinTheta * cos(phi) * T + sinTheta * sin(phi) * B + cosTheta * R;
		const float3 L = 2.0f * dot(R, H) * H - R;
		const float NoL = dot(R, L);
		if (NoL > 0.0f)
		{
			sum += SampleEnvironmentMap(L) * NoL;
			weight += NoL;
		}
	}
	output[Id] = float4(weight > 0.0f ? sum / weight : SampleEnvironmentMap(R), 1.0f);
}

[numthreads(8, 8, 1)]
void ConvolveIrradiance (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_FilterSize))
	{
		return;
	}
	const float3 n = EqualAreaSquareToSphere((float2(Id.xy) + 0.5f) / float(g_FilterSize));
	// Equal-area grid: every cell subtends 4 pi / grid^2.
	const uint grid = 64u;
	float3 sum = 0.0f;
	for (uint y = 0u; y < grid; ++y)
	{
		for (uint x = 0u; x < grid; ++x)
		{
			const float2 uv = (float2(x, y) + 0.5f) / float(grid);
			const float3 w = EqualAreaSquareToSphere(uv);
			const float c = dot(n, w);
			if (c > 0.0f)
			{
				Texture2D<float4> env = ResourceDescriptorHeap[g_FilterInput];
				sum += env.SampleLevel(s_LinearClamp, uv, 0.0f).rgb * c;
			}
		}
	}
	RwTex2D(g_FilterOutput)[Id.xy] = float4(sum * (4.0f * kPi / float(grid * grid)), 1.0f);
}
