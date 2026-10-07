#ifndef FRT_SKY_HLSLI
#define FRT_SKY_HLSLI

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "Common/Packing.hlsli"
#include "Common/Sampling.hlsli"

// Procedural sky model (the parameters the editor exposes). The solar disk is NOT part of this
// function: the sun is its own light (cone) and is added by EvalSunDisk where needed.
float3 EvalProceduralSky (float3 Dir)
{
	const float softness = max(g_Frame.HorizonSoftness, 1e-4f);
	const float skyT = smoothstep(-softness, softness, Dir.y);
	const float zenithT = saturate(Dir.y);
	const float3 skyColor = lerp(g_Frame.SkyHorizonColor.rgb, g_Frame.SkyZenithColor.rgb, zenithT);
	return lerp(g_Frame.GroundColor.rgb, skyColor * g_Frame.SkyIntensity, skyT);
}

// Radiance of the solar disk along Dir (0 outside the disk). Mild limb darkening.
float3 EvalSunDisk (float3 Dir)
{
	if (!HasFrameFlag(FrameFlag_SunEnabled))
	{
		return 0.0f;
	}
	const float cosTheta = dot(Dir, g_Frame.SunDirection);
	if (cosTheta < g_Frame.SunCosAngularRadius)
	{
		return 0.0f;
	}
	return g_Frame.SunRadiance;
}

// Baked environment (equal-area octahedral), bilinear.
float3 EvalSkyEnvironment (float3 Dir)
{
	if (!HasFrameFlag(FrameFlag_SkyEnabled))
	{
		return 0.0f;
	}
	if (g_Frame.SkyEnvironmentMap == FRT_INVALID_INDEX)
	{
		return EvalProceduralSky(Dir);
	}
	const float2 uv = EqualAreaSphereToSquare(Dir);
	return Tex2D(g_Frame.SkyEnvironmentMap).SampleLevel(s_LinearClamp, uv, 0.0f).rgb;
}

// ------------------------------------------------------------------------------------------
// Environment importance sampling by hierarchical descent of a luminance pyramid.
// Pyramid mip k stores the AVERAGE of its 2x2 children; the top (1x1) is the mean luminance.
// Texels of the equal-area map all subtend 4pi/N^2, so pdf(w) = L(texel) / (mean * 4pi).
// ------------------------------------------------------------------------------------------

float PdfSkyEnvironment (float3 Dir)
{
	if (g_Frame.SkyLuminancePyramid == FRT_INVALID_INDEX || !HasFrameFlag(FrameFlag_SkyEnabled))
	{
		return kInvFourPi;
	}
	Texture2D<float> pyramid = ResourceDescriptorHeap[g_Frame.SkyLuminancePyramid];
	const uint size = g_Frame.SkyEnvironmentSize;
	const float mean = pyramid.Load(int3(0, 0, g_Frame.SkyPyramidMips - 1u));
	if (mean <= 0.0f)
	{
		return kInvFourPi;
	}
	const float2 uv = EqualAreaSphereToSquare(Dir);
	const uint2 texel = min(uint2(uv * size), uint2(size - 1u, size - 1u));
	const float lum = pyramid.Load(int3(texel, 0));
	return lum / (mean * 4.0f * kPi);
}

float3 SampleSkyEnvironment (float2 U, out float Pdf)
{
	Pdf = 0.0f;
	if (g_Frame.SkyLuminancePyramid == FRT_INVALID_INDEX)
	{
		const float3 d = SampleUniformSphere(U);
		Pdf = kInvFourPi;
		return d;
	}
	Texture2D<float> pyramid = ResourceDescriptorHeap[g_Frame.SkyLuminancePyramid];
	const uint mips = g_Frame.SkyPyramidMips;
	uint2 texel = uint2(0, 0);
	float2 u = U;
	for (int mip = int(mips) - 2; mip >= 0; --mip)
	{
		texel *= 2u;
		const float l00 = pyramid.Load(int3(texel + uint2(0, 0), mip));
		const float l10 = pyramid.Load(int3(texel + uint2(1, 0), mip));
		const float l01 = pyramid.Load(int3(texel + uint2(0, 1), mip));
		const float l11 = pyramid.Load(int3(texel + uint2(1, 1), mip));
		const float left = l00 + l01;
		const float right = l10 + l11;
		const float total = left + right;
		if (total <= 0.0f)
		{
			// Degenerate (all black) subtree: stay uniform.
			texel += uint2(u.x < 0.5f ? 0u : 1u, u.y < 0.5f ? 0u : 1u);
			u = frac(u * 2.0f);
			continue;
		}
		const float pLeft = left / total;
		uint2 offset;
		if (u.x < pLeft)
		{
			offset.x = 0u;
			u.x = u.x / pLeft;
			const float pTop = l00 / max(left, 1e-30f);
			if (u.y < pTop) { offset.y = 0u; u.y = u.y / pTop; }
			else            { offset.y = 1u; u.y = (u.y - pTop) / max(1.0f - pTop, 1e-30f); }
		}
		else
		{
			offset.x = 1u;
			u.x = (u.x - pLeft) / max(1.0f - pLeft, 1e-30f);
			const float pTop = l10 / max(right, 1e-30f);
			if (u.y < pTop) { offset.y = 0u; u.y = u.y / pTop; }
			else            { offset.y = 1u; u.y = (u.y - pTop) / max(1.0f - pTop, 1e-30f); }
		}
		texel += offset;
		u = saturate(u);
	}

	const float size = float(g_Frame.SkyEnvironmentSize);
	const float2 uv = (float2(texel) + min(u, 0.99999f)) / size;
	const float3 dir = EqualAreaSquareToSphere(uv);
	const float mean = pyramid.Load(int3(0, 0, mips - 1u));
	const float lum = pyramid.Load(int3(texel, 0));
	Pdf = mean > 0.0f ? lum / (mean * 4.0f * kPi) : kInvFourPi;
	return dir;
}

// Everything a ray that escapes the scene sees (sky + optionally the solar disk).
float3 EvalEnvironment (float3 Dir, bool bIncludeSunDisk)
{
	float3 radiance = EvalSkyEnvironment(Dir);
	if (bIncludeSunDisk)
	{
		radiance += EvalSunDisk(Dir);
	}
	return radiance;
}

#endif
