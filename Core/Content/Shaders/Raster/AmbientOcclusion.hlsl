// Ground-truth ambient occlusion (Jimenez et al. 2016, "Practical Real-Time Strategies for
// Accurate Indirect Occlusion"; structure after Intel's XeGTAO) for the raster preview, from the
// depth prepass alone. Two slices per pixel, horizons searched on both sides within a world-space
// radius, cosine-weighted visibility integrated analytically per slice. The slice angle and the
// step offsets turn per pixel and frame (interleaved gradient noise); a depth-aware 3x3 filter
// and the TAA integrate that noise.
//
// It supplies what a screen-space method can know: contact and medium-scale occlusion. Large-scale
// occlusion (a closed atrium) is the job of the portals and the enclosure's energy balance.

#include "Common/Bindings.hlsli"
#include "Common/Math.hlsli"

cbuffer AoConstants : register(b0)
{
	uint g_Depth;            // Texture2D<float> reversed infinite depth (near / view z)
	uint g_Input;            // filter: raw AO
	uint g_Output;           // RWTexture2D<float>
	float g_Radius;          // world units
	uint2 g_Size;
	uint g_Pad0;
	uint g_Pad1;
};

static const uint kSliceCount = 2u;
static const uint kStepsPerSide = 4u;

float LoadViewZ (int2 Pixel)
{
	const float d = Tex2DFloat(g_Depth).Load(int3(clamp(Pixel, int2(0, 0), int2(g_Size) - 1), 0));
	return d > 0.0f ? g_Frame.NearZ / d : 1e6f;
}

// View space: x right, y up, z forward (the engine's left-handed view).
float3 ViewPosition (float2 PixelCenter, float ViewZ)
{
	const float2 ndc = float2(PixelCenter.x * g_Frame.InvRenderSize.x * 2.0f - 1.0f, 1.0f - PixelCenter.y * g_Frame.InvRenderSize.y * 2.0f);
	const float tanY = g_Frame.TanHalfFovY;
	return float3(ndc.x * tanY * g_Frame.AspectRatio * ViewZ, ndc.y * tanY * ViewZ, ViewZ);
}

float InterleavedGradientNoise (float2 Pixel, uint Frame)
{
	Pixel += 5.588238f * float(Frame & 63u);
	return frac(52.9829189f * frac(dot(Pixel, float2(0.06711056f, 0.00583715f))));
}

[numthreads(8, 8, 1)]
void ComputeAo (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Size))
	{
		return;
	}
	const int2 pixel = int2(Id.xy);
	const float z = LoadViewZ(pixel);
	if (z >= 1e5f)
	{
		RwTex2DFloat(g_Output)[Id.xy] = 1.0f;
		return;
	}
	const float3 P = ViewPosition(float2(pixel) + 0.5f, z);
	const float3 V = normalize(-P);

	// Normal from the depth: on each axis the neighbour nearer in depth, so edges do not bend it.
	const float3 pl = ViewPosition(float2(pixel) + float2(-0.5f, 0.5f), LoadViewZ(pixel + int2(-1, 0)));
	const float3 pr = ViewPosition(float2(pixel) + float2(1.5f, 0.5f), LoadViewZ(pixel + int2(1, 0)));
	const float3 pu = ViewPosition(float2(pixel) + float2(0.5f, -0.5f), LoadViewZ(pixel + int2(0, -1)));
	const float3 pd = ViewPosition(float2(pixel) + float2(0.5f, 1.5f), LoadViewZ(pixel + int2(0, 1)));
	const float3 dx = abs(pr.z - z) < abs(z - pl.z) ? pr - P : P - pl;
	const float3 dy = abs(pd.z - z) < abs(z - pu.z) ? pd - P : P - pu;
	float3 N = normalize(cross(dx, dy));
	if (dot(N, V) < 0.0f)
	{
		N = -N;
	}
	// A feature one or two pixels wide (chains, wires) has no continuous neighbour on an axis:
	// its depth-derived normal is meaningless (it would lie in the view plane and occlude
	// everything), so treat it as facing the viewer.
	const float gapX = min(abs(pr.z - z), abs(z - pl.z));
	const float gapY = min(abs(pd.z - z), abs(z - pu.z));
	if (max(gapX, gapY) > 0.05f * z)
	{
		N = V;
	}

	// Screen-space radius of the world-space radius at this depth (pixels).
	const float pixelsPerUnit = 0.5f * g_Frame.RenderSize.y / (g_Frame.TanHalfFovY * z);
	const float radiusPixels = min(g_Radius * pixelsPerUnit, 256.0f);
	if (radiusPixels < 1.0f)
	{
		RwTex2DFloat(g_Output)[Id.xy] = 1.0f;
		return;
	}
	const float falloffRange = 0.615f * g_Radius;
	const float falloffMul = -1.0f / falloffRange;
	const float falloffAdd = (g_Radius - falloffRange) / falloffRange + 1.0f;

	const float noiseSlice = InterleavedGradientNoise(float2(pixel), g_Frame.FrameIndex);
	const float noiseStep = InterleavedGradientNoise(float2(pixel.yx) + 17.0f, g_Frame.FrameIndex + 7u);

	float visibility = 0.0f;
	[unroll]
	for (uint slice = 0u; slice < kSliceCount; ++slice)
	{
		const float phi = (float(slice) + noiseSlice) * (kPi / float(kSliceCount));
		const float2 omega = float2(cos(phi), -sin(phi));    // screen space, y down
		const float3 direction = float3(cos(phi), sin(phi), 0.0f);
		const float3 orthoDirection = direction - dot(direction, V) * V;
		const float3 axis = normalize(cross(orthoDirection, V));
		const float3 projectedN = N - axis * dot(N, axis);
		const float projectedLength = length(projectedN);
		const float signN = dot(orthoDirection, projectedN) >= 0.0f ? 1.0f : -1.0f;
		const float cosN = saturate(dot(projectedN, V) / max(projectedLength, 1e-6f));
		const float n = signN * SafeAcos(cosN);
		const float lowCos0 = cos(n + kHalfPi);
		const float lowCos1 = cos(n - kHalfPi);
		float horizonCos0 = lowCos0;
		float horizonCos1 = lowCos1;
		[unroll]
		for (uint s = 0u; s < kStepsPerSide; ++s)
		{
			// Quadratic step distribution: more samples near the pixel, where contact detail is.
			float t = (float(s) + noiseStep) / float(kStepsPerSide);
			t = t * t;
			const float2 offset = round(omega * (t * radiusPixels + 1.0f));
			const float2 p0 = float2(pixel) + offset;
			const float2 p1 = float2(pixel) - offset;
			const float3 s0 = ViewPosition(p0 + 0.5f, LoadViewZ(int2(p0))) - P;
			const float3 s1 = ViewPosition(p1 + 0.5f, LoadViewZ(int2(p1))) - P;
			const float d0 = length(s0);
			const float d1 = length(s1);
			const float w0 = saturate(d0 * falloffMul + falloffAdd);
			const float w1 = saturate(d1 * falloffMul + falloffAdd);
			horizonCos0 = max(horizonCos0, lerp(lowCos0, dot(s0 / max(d0, 1e-6f), V), w0));
			horizonCos1 = max(horizonCos1, lerp(lowCos1, dot(s1 / max(d1, 1e-6f), V), w1));
		}
		float h0 = -SafeAcos(horizonCos1);
		float h1 = SafeAcos(horizonCos0);
		h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
		h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
		const float arc0 = (cosN + 2.0f * h0 * sin(n) - cos(2.0f * h0 - n)) * 0.25f;
		const float arc1 = (cosN + 2.0f * h1 * sin(n) - cos(2.0f * h1 - n)) * 0.25f;
		visibility += projectedLength * (arc0 + arc1);
	}
	RwTex2DFloat(g_Output)[Id.xy] = saturate(visibility / float(kSliceCount));
}

// Depth-aware 3x3 filter of the noisy AO (weights fall off with relative depth difference).
[numthreads(8, 8, 1)]
void FilterAo (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Size))
	{
		return;
	}
	const int2 pixel = int2(Id.xy);
	const float z = LoadViewZ(pixel);
	Texture2D<float> input = Tex2DFloat(g_Input);
	float sum = 0.0f;
	float weight = 0.0f;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			const int2 q = clamp(pixel + int2(x, y), int2(0, 0), int2(g_Size) - 1);
			const float w = exp2(-abs(LoadViewZ(q) - z) / max(0.02f * z, 1e-4f)) * ((x == 0 && y == 0) ? 2.0f : 1.0f);
			sum += input.Load(int3(q, 0)) * w;
			weight += w;
		}
	}
	RwTex2DFloat(g_Output)[Id.xy] = sum / max(weight, 1e-6f);
}
