// Temporal anti-aliasing / upscaling resolve (CTemporalUpscaler), one thread per DISPLAY pixel.
//
// Reconstruction follows the jittered-sample formulation of temporal upsampling (Yang et al.
// 2009, "Amortized Supersampling"; UE4 TAAU, Karis 2014; AMD FSR 2): every frame each output
// pixel gathers the 3x3 render-resolution samples around its position, weights them with a
// reconstruction kernel evaluated at the true (jittered) sample positions, and adds the result
// to a display-resolution history in proportion to the kernel weight that landed on it
// ("confidence"). Over a jitter cycle the history converges to the kernel-filtered
// supersampled image. Per pixel:
//   1. 3x3 neighbourhood from a groupshared tile: linear colour (filtering, accumulation) and
//      working-space colour (YCoCg of exposure-scaled max-Reinhard; statistics only) ->
//      moments, min/max, closest depth.
//   2. Motion vector of the closest-depth sample (dilation keeps foreground edges' history).
//   3. History validity: on-screen test + depth reprojection test against last frame's ViewZ.
//   4. 5-tap Catmull-Rom fetch of both histories (colour, temporal moments).
//   5. Lanczos-2 / Blackman-Harris spatial reconstruction IN LINEAR HDR, kernel narrowed with
//      the upscale ratio on accumulated pixels and widened on new ones (FSR 2 kernel bias).
//   6. Rectification: the history's working-space mean is compared with this frame's
//      neighbourhood and value; the tolerance adds the temporal spread of this pixel's own
//      input (second history). Rejection shortens the history.
//   7. Confidence-weighted LINEAR accumulation: alpha = c / (W + c), W = accumulated confidence.
//
// Energy. A path-traced input is sparse: most of its energy sits in few, bright samples (the
// Sponza atrium at 64 spp still has 80% black pixels), and sub-pixel highlights are hit by
// some jitter phases only. Everything that averages values here therefore works on linear
// radiance: filtering or blending tonemapped values, or weighting them by 1 / (1 + L)
// (Karis anti-flicker), returns roughly the harmonic mean (Jensen's inequality). Measured on
// the default view: a 19.2 emissive strip came out at 0.1 and the image lost 20% of its
// energy. The tonemapped working space is kept where robustness matters and nothing is
// averaged into the output: the rectification statistics.
//
// Rectification. Classic variance clipping (Salvi 2016) compares the history with the current
// 3x3 neighbourhood, which assumes the frame is a noise-free sample of the same image. Under
// Monte Carlo noise or jitter aliasing it is not: a pixel whose 3x3 is black this frame
// (probability ~0.8 here) clips a correct history to black, every few frames. So the test is
// done in one consistent domain. A second history stores the temporal mean and variance of
// this pixel's own working-space value (E[w], Var[w.Y]). The history mean E[w] is clipped
// toward this frame's w into the hull of
//   neighbourhood mean +- (VarianceGamma * sigma_spatial + TemporalGamma * sigma_temporal)
//   current value      +- TemporalGamma * sigma_temporal,
// so a value the history has seen often (a flickering firefly, a highlight that half the
// jitter phases miss) is no evidence of change, while a ghost (seen never) is. The clip factor
// t moves both histories toward the current frame by the same fraction and scales the
// accumulated confidence, so a rejected pixel re-converges as a running mean.

#include "Common/Bindings.hlsli"
#include "Shared/UpscalerData.h"
#include "Upscaler/UpscalerCommon.hlsli"

ConstantBuffer<STemporalUpscalerConstants> g_Pass : register(b2);

#define GROUP_SIZE 8
#define GROUP_THREADS (GROUP_SIZE * GROUP_SIZE)
// Render-resolution footprint of an 8x8 output group plus the 3x3 halo. For scale <= 1 an
// output span of 8 pixels maps to at most 8 render pixels (7 steps + rounding) + 2 halo = 10.
#define TILE_DIM 10
#define TILE_TEXELS (TILE_DIM * TILE_DIM)

// rgb = sanitized linear colour, w = sanitized linear view depth; and the working-space colour.
groupshared float4 gs_Linear[TILE_TEXELS];
groupshared float3 gs_Working[TILE_TEXELS];

bool HasFlag (uint Flag) { return (g_Pass.Flags & Flag) != 0u; }

// Absolute tolerance of the rectification box, working-space units (see the use).
static const float kRectificationFloor = 1.0f / 512.0f;

struct SRenderTap
{
	float3 Linear;
	float Z;
	float3 Working;
};

SRenderTap LoadRenderTap (int2 Texel)
{
	SRenderTap tap;
	tap.Linear = SanitizeHdr(Tex2D(g_Pass.InColor)[Texel].rgb);
	tap.Z = SanitizeViewZ(Tex2DFloat(g_Pass.InViewZ)[Texel]);
	tap.Working = ToWorkingSpace(tap.Linear, g_Pass.Exposure);
	return tap;
}

SRenderTap FetchTap (bool bTile, int2 Texel, int2 TileOrigin, int2 RenderMax)
{
	if (bTile)
	{
		const int2 local = clamp(Texel - TileOrigin, 0, TILE_DIM - 1);
		const uint index = local.y * TILE_DIM + local.x;
		SRenderTap tap;
		tap.Linear = gs_Linear[index].rgb;
		tap.Z = gs_Linear[index].w;
		tap.Working = gs_Working[index];
		return tap;
	}
	return LoadRenderTap(clamp(Texel, 0, RenderMax));
}

// Reconstruction kernels; X2 = squared distance in kernel units. Must stay in sync with
// EvaluateReconstructionKernel in Upscaler.cpp.
float LanczosKernel (float X2)
{
	// Lanczos-2: sinc(x) sinc(x/2) = sin(pi x) sin(pi x/2) / (pi^2 x^2 / 2). With
	// s = sin(pi x/2), c = cos(pi x/2): sin(pi x) = 2 s c, so L2(x) = 4 s^2 c / (pi^2 x^2).
	// Evaluated exactly (one sincos) rather than with the EASU polynomial, whose negative
	// lobe is ~60% too deep at the 3x3 corners. Branch-free: clamping x^2 to 4 lands on
	// the zero at x = 2, and at x^2 = 1e-6 the expression is 1 to float precision.
	const float x2 = clamp(X2, 1e-6f, 4.0f);
	float s, c;
	sincos(kHalfPi * sqrt(x2), s, c);
	return 4.0f * s * s * c / (kPi * kPi * x2);
}

// Blackman-Harris (radius ~1.5 kernel units), Gaussian fit from UE4 TAAU: exp(-2.29 x^2).
float BlackmanHarrisKernel (float X2)
{
	return exp(-2.29f * X2);
}

// Largest t in [0, 1] for which Center + t (History - Center) lies inside [Lo, Hi]. Clipping
// along the segment to the centre (instead of a per-channel clamp) keeps the result on the
// history -> current line, which avoids the hue shifts of a clamp (Playdead INSIDE, Pedersen
// 2016). The caller guarantees Lo <= Center <= Hi.
float ClipFactor (float3 History, float3 Center, float3 Lo, float3 Hi)
{
	const float3 v = History - Center;
	const float3 extent = select(v > 0.0f, Hi - Center, Center - Lo);
	const float3 av = abs(v);
	const float3 ratio = select(av > extent, extent / max(av, 1e-7f), 1.0f);
	return saturate(min(min(ratio.x, ratio.y), ratio.z));
}

// 5-tap Catmull-Rom history fetch: the 4x4 bicubic is folded into bilinear taps and the four
// corner taps (weights ~0) are dropped (Jimenez 2016, "Filmic SMAA"; Djonov / Pettineo). The
// sharp kernel keeps the history from blurring under repeated reprojection. Both histories are
// fetched with the same taps.
struct SHistoryTaps
{
	float2 Uv[5];
	float Weight[5];
	uint Count;
};

SHistoryTaps ComputeHistoryTaps (float2 Uv)
{
	SHistoryTaps taps;
	if (!HasFlag(UpscalerFlag_CatmullRom))
	{
		taps.Count = 1u;
		taps.Uv[0] = Uv;
		taps.Weight[0] = 1.0f;
		[unroll]
		for (uint i = 1u; i < 5u; ++i)
		{
			taps.Uv[i] = Uv;
			taps.Weight[i] = 0.0f;
		}
		return taps;
	}

	const float2 samplePos = Uv * g_Pass.DisplaySize;
	const float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
	const float2 f = samplePos - texPos1;
	const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	const float2 w3 = f * f * (-0.5f + 0.5f * f);
	const float2 w12 = w1 + w2;
	const float2 uv0 = (texPos1 - 1.0f) * g_Pass.InvDisplaySize;
	const float2 uv12 = (texPos1 + w2 / w12) * g_Pass.InvDisplaySize;
	const float2 uv3 = (texPos1 + 2.0f) * g_Pass.InvDisplaySize;

	taps.Count = 5u;
	taps.Uv[0] = float2(uv12.x, uv0.y);  taps.Weight[0] = w12.x * w0.y;
	taps.Uv[1] = float2(uv0.x, uv12.y);  taps.Weight[1] = w0.x * w12.y;
	taps.Uv[2] = uv12;                   taps.Weight[2] = w12.x * w12.y;
	taps.Uv[3] = float2(uv3.x, uv12.y);  taps.Weight[3] = w3.x * w12.y;
	taps.Uv[4] = float2(uv12.x, uv3.y);  taps.Weight[4] = w12.x * w3.y;
	const float norm = rcp(taps.Weight[0] + taps.Weight[1] + taps.Weight[2] + taps.Weight[3] + taps.Weight[4]);
	[unroll]
	for (uint i = 0u; i < 5u; ++i)
	{
		taps.Weight[i] *= norm;
	}
	return taps;
}

float4 SampleHistory (Texture2D<float4> History, SHistoryTaps Taps)
{
	float4 sum = History.SampleLevel(s_LinearClamp, Taps.Uv[0], 0.0f) * Taps.Weight[0];
	if (Taps.Count > 1u)
	{
		[unroll]
		for (uint i = 1u; i < 5u; ++i)
		{
			sum += History.SampleLevel(s_LinearClamp, Taps.Uv[i], 0.0f) * Taps.Weight[i];
		}
	}
	return sum;
}

float3 ReconstructWorldPosition (float2 RenderPixelPosition, float ViewZ)
{
	// Same ray as GeneratePrimaryRay() in PathTracer.hlsl (unjittered inverse; the jitter is
	// already part of the pixel position).
	const float2 ndc = RenderPixelPosition * g_Pass.InvRenderSize * 2.0f - 1.0f;
	const float4 nearPoint = mul(g_Frame.InvViewProjNoJitter, float4(ndc.x, -ndc.y, 0.5f, 1.0f));
	const float3 direction = normalize(nearPoint.xyz / nearPoint.w - g_Frame.CameraPosition);
	return g_Frame.CameraPosition + direction * (ViewZ / max(dot(direction, g_Frame.CameraForward), 1e-4f));
}

// 1 when the closest-depth surface of this pixel was visible at its reprojected position last
// frame, ramping to 0 at twice the tolerance. The expected previous depth assumes a static
// point (w of the previous unjittered clip position = previous view depth for a D3D
// perspective matrix); the tolerance absorbs moderate object motion along the view axis.
float DepthReprojectionValidity (int2 ClosestTexel, float ClosestZ, float2 Motion, float RelativeSlope)
{
	// Last frame's samples sat at j + 0.5 + PrevJitter. The 4x4 of those around the reprojected
	// position of the closest sample is searched (four gathers). A single 2x2 was not enough:
	// with a jitter step above half a pixel it can miss the texel that saw this surface, and on
	// a static camera 0.6% of the pixels (all on depth edges, where anti-aliasing matters) lost
	// their history every frame.
	const float2 samplePos = float2(ClosestTexel) + 0.5f + g_Pass.Jitter;
	const float2 prevPos = samplePos + Motion;
	const float2 gatherUv = (prevPos - g_Pass.PrevJitter) * g_Pass.InvRenderSize;
	const float2 halfTexel = 0.5f * g_Pass.InvRenderSize;
	Texture2D<float> prevViewZ = Tex2DFloat(g_Pass.InPrevViewZ);
	float4 prevZ[4];
	prevZ[0] = prevViewZ.GatherRed(s_PointClamp, gatherUv + float2(-halfTexel.x, -halfTexel.y));
	prevZ[1] = prevViewZ.GatherRed(s_PointClamp, gatherUv + float2(halfTexel.x, -halfTexel.y));
	prevZ[2] = prevViewZ.GatherRed(s_PointClamp, gatherUv + float2(-halfTexel.x, halfTexel.y));
	prevZ[3] = prevViewZ.GatherRed(s_PointClamp, gatherUv + float2(halfTexel.x, halfTexel.y));
	[unroll]
	for (uint i = 0u; i < 4u; ++i)
	{
		prevZ[i] = select(or(isnan(prevZ[i]), prevZ[i] <= 0.0f), kInvalidViewZ, prevZ[i]);
	}

	if (ClosestZ >= kSkyViewZ)
	{
		// Sky has no depth to compare: valid as long as sky was visible there.
		return any(max(max(prevZ[0], prevZ[1]), max(prevZ[2], prevZ[3])) >= kSkyViewZ) ? 1.0f : 0.0f;
	}

	const float3 world = ReconstructWorldPosition(samplePos, ClosestZ);
	const float expectedZ = mul(g_Frame.PrevViewProjNoJitter, float4(world, 1.0f)).w;
	if (expectedZ <= 1e-4f)
	{
		return 0.0f;   // behind the previous camera
	}
	// Path tracer only: a surface seen in a PSR mirror (flag in the ViewZ mantissa) must have been
	// seen in the mirror last frame too. The reflected floor lies at the real floor's depths, so the
	// depth test alone takes the real floor's history for it (and back) along the mirror's edges.
	const uint seenInMirror = ViewZSeenInMirror(ClosestZ);
	[unroll]
	for (uint j = 0u; j < 4u; ++j)
	{
		prevZ[j] = select((asuint(prevZ[j]) & 1u) == seenInMirror, prevZ[j], kInvalidViewZ);
	}
	const float invExpectedZ = rcp(expectedZ);
	const float4 relError = min(min(abs(prevZ[0] - expectedZ), abs(prevZ[1] - expectedZ)),
		min(abs(prevZ[2] - expectedZ), abs(prevZ[3] - expectedZ))) * invExpectedZ;
	const float minError = min(min(relError.x, relError.y), min(relError.z, relError.w));
	// Slope term: on a surface seen at a grazing angle the depth changes by RelativeSlope per
	// render pixel, and the nearest previous sample can be up to ~1 pixel away.
	const float tolerance = g_Pass.DepthTolerance + RelativeSlope;
	return saturate(2.0f - minError / max(tolerance, 1e-6f));
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main (uint3 GroupId : SV_GroupID, uint3 DispatchThreadId : SV_DispatchThreadID, uint GroupIndex : SV_GroupIndex)
{
	// Render px per display px. Passed in rather than RenderSize * InvDisplaySize, whose
	// rounding (1 +- 1 ulp at native) would push some tiles one texel over TILE_DIM.
	const float2 scale = g_Pass.RenderScale;
	const float2 jitter = g_Pass.Jitter;
	const int2 renderMax = int2(g_Pass.RenderSize) - 1;

	// --- Groupshared render-res tile (group-uniform bounds) -------------------------------
	// Render sample k sits at k + 0.5 + jitter, so the nearest sample to render position x is
	// floor(x - jitter). The tile spans the nearest samples of the group's first and last
	// pixel, plus one texel of halo.
	// The bounds are clamped the same way the per-pixel centre is. Unclamped, a right/bottom
	// edge group whose first pixel already maps past the last render texel (odd sizes, e.g.
	// 1921 -> 961 with negative jitter) had its origin at renderMax while the clamped centre
	// asked for renderMax - 1, so FetchTap silently returned the wrong texel.
	const uint2 groupBase = GroupId.xy * GROUP_SIZE;
	const int2 tileOrigin = clamp(int2(floor((float2(groupBase) + 0.5f) * scale - jitter)), 0, renderMax) - 1;
	const int2 tileLast = clamp(int2(floor((float2(groupBase + (GROUP_SIZE - 1u)) + 0.5f) * scale - jitter)), 0, renderMax) + 1;
	const int2 tileExtent = tileLast - tileOrigin + 1;
	// Render res above display res (supersampling) does not fit: fall back to direct loads.
	const bool bTile = all(tileExtent <= TILE_DIM);
	if (bTile)
	{
		const uint tileWidth = uint(tileExtent.x);
		const uint count = tileWidth * uint(tileExtent.y);
		for (uint i = GroupIndex; i < count; i += GROUP_THREADS)
		{
			const uint2 local = uint2(i % tileWidth, i / tileWidth);
			const SRenderTap tap = LoadRenderTap(clamp(tileOrigin + int2(local), 0, renderMax));
			const uint index = local.y * TILE_DIM + local.x;
			gs_Linear[index] = float4(tap.Linear, tap.Z);
			gs_Working[index] = tap.Working;
		}
	}
	GroupMemoryBarrierWithGroupSync();

	const uint2 pixel = DispatchThreadId.xy;
	if (any(pixel >= uint2(g_Pass.DisplaySize)))
	{
		return;
	}

	// --- 3x3 neighbourhood ----------------------------------------------------------------
	const float2 inputPos = (float2(pixel) + 0.5f) * scale;
	int2 center = clamp(int2(floor(inputPos - jitter)), 0, renderMax);
	if (bTile)
	{
		// Mathematically a no-op. It protects against a 1-ulp difference between this
		// expression and the tile-bound one (mad contraction), which would otherwise read a
		// clamped tile texel under another texel's kernel offset.
		center = clamp(center, tileOrigin + 1, tileLast - 1);
	}

	SRenderTap taps[9];
	float2 offsets[9];
	bool inside[9];
	float3 m1 = 0.0f;
	float3 m2 = 0.0f;
	float3 boxMin = 1e30f;
	float3 boxMax = -1e30f;
	float3 linearMin = 1e30f;
	float3 linearMax = 0.0f;
	float count = 0.0f;
	[unroll]
	for (uint t = 0u; t < 9u; ++t)
	{
		const int2 k = center + int2(int(t % 3u) - 1, int(t / 3u) - 1);
		taps[t] = FetchTap(bTile, k, tileOrigin, renderMax);
		offsets[t] = (float2(k) + 0.5f + jitter) - inputPos;   // render px, sample - output centre
		inside[t] = all(k >= 0) && all(k <= renderMax);
		if (inside[t])
		{
			const float3 w = taps[t].Working;
			m1 += w;
			m2 += w * w;
			boxMin = min(boxMin, w);
			boxMax = max(boxMax, w);
			linearMin = min(linearMin, taps[t].Linear);
			linearMax = max(linearMax, taps[t].Linear);
			count += 1.0f;
		}
	}

	// Closest depth for motion dilation; the centre wins ties (sky everywhere, flat regions).
	float closestZ = taps[4].Z;
	int2 closestTexel = center;
	[unroll]
	for (uint t2 = 0u; t2 < 9u; ++t2)
	{
		if (inside[t2] && taps[t2].Z < closestZ)
		{
			closestZ = taps[t2].Z;
			closestTexel = center + int2(int(t2 % 3u) - 1, int(t2 / 3u) - 1);
		}
	}

	// Relative depth slope around the centre. Per axis the smaller one-sided difference is
	// used, so a depth discontinuity (where disocclusion actually happens) does not inflate
	// the tolerance but a tilted plane does.
	const float zc = taps[4].Z;
	const float slopeX = min(abs(taps[5].Z - zc), abs(zc - taps[3].Z));
	const float slopeY = min(abs(taps[7].Z - zc), abs(zc - taps[1].Z));
	// Capped: a one-pixel-wide feature against the sky would otherwise disable the test.
	const float relativeSlope = zc < kSkyViewZ ? min(max(slopeX, slopeY) / zc, 0.5f) : 0.0f;

	// --- Reprojection ---------------------------------------------------------------------
	float2 motion = Tex2DFloat2(g_Pass.InMotionVectors)[closestTexel];
	// A non-finite vector means we do not know where the surface was, so its history is
	// rejected. Substituting zero motion would ghost.
	const bool bMotionValid = all(isfinite(motion));
	motion = bMotionValid ? clamp(motion, -4.0f * g_Pass.RenderSize, 4.0f * g_Pass.RenderSize) : 0.0f;
	const float2 uv = (float2(pixel) + 0.5f) * g_Pass.InvDisplaySize;
	const float2 prevUv = uv + motion * g_Pass.InvRenderSize;
	const float speed = length(motion * g_Pass.DisplaySize * g_Pass.InvRenderSize);   // display px / frame

	float validity = (HasFlag(UpscalerFlag_HistoryValid) && bMotionValid && all(prevUv >= 0.0f) && all(prevUv <= 1.0f)) ? 1.0f : 0.0f;
	if (validity > 0.0f && HasFlag(UpscalerFlag_DepthTest) && HasFlag(UpscalerFlag_PrevDepthValid))
	{
		validity *= DepthReprojectionValidity(closestTexel, closestZ, motion, relativeSlope);
	}

	float4 history = 0.0f;
	float4 moments = 0.0f;   // working space: rgb = E[w] (YCoCg, chroma signed), a = Var[w.Y]
	if (validity > 0.0f)
	{
		const SHistoryTaps historyTaps = ComputeHistoryTaps(prevUv);
		history = SampleHistory(Tex2D(g_Pass.InHistory), historyTaps);
		moments = SampleHistory(Tex2D(g_Pass.InHistoryMoments), historyTaps);
		if (!all(isfinite(history)) || !all(isfinite(moments)))
		{
			validity = 0.0f;
			history = 0.0f;
			moments = 0.0f;
		}
	}
	history.rgb = clamp(history.rgb, 0.0f, kUpscalerMaxHdr);   // Catmull-Rom undershoot
	moments.a = max(moments.a, 0.0f);   // Catmull-Rom undershoot; the YCoCg chroma means are signed

	// Kernel choice. Lanczos-2 is the sharper reconstruction but rings, and its dering clamp is
	// biased on sparse input: an isolated bright sample under a negative lobe gives a negative
	// result that the clamp lifts to the neighbourhood minimum, so the negative half of its
	// footprint is lost (+25% energy in the atrium at 16 spp, Blackman-Harris +-0%). The
	// non-negative Blackman-Harris needs no clamp. So the kernel moves to Blackman-Harris with
	// the relative temporal noise of this pixel's history. That statistic comes from earlier
	// frames, so the choice is independent of this frame's samples and the blend stays an
	// unbiased linear filter.
	float lanczosAmount = 0.0f;
	if (HasFlag(UpscalerFlag_LanczosKernel))
	{
		const float noise = validity > 0.0f
			? sqrt(moments.a) * rcp(max(moments.r, 0.0f) + 0.02f)
			: 1.0f;   // unknown: no history, assume the worst
		lanczosAmount = saturate(2.0f - 4.0f * noise);   // 1 below 0.25, 0 above 0.5
	}

	// Accumulated confidence, capped lower under fast motion (every reprojection resamples the
	// history; shorter memory bounds the accumulated blur and lag). The cap follows the kernel:
	// both caps give the same mean blend factor (see ComputeMeanConfidence).
	const float motionT = saturate(speed * g_Pass.InvMotionSpeedRange);
	const float kernelMaxWeight = lerp(g_Pass.MaxHistoryWeightGaussian, g_Pass.MaxHistoryWeight, lanczosAmount);
	const float maxWeight = kernelMaxWeight * lerp(1.0f, g_Pass.MotionHistoryScale, motionT);
	float historyWeight = clamp(history.a, 0.0f, maxWeight) * validity;

	// --- Spatial reconstruction (linear HDR) ----------------------------------------------
	// Pixels with (almost) no history use the wide new-sample kernel (radius 2 render px) so
	// the first frame after a disocclusion is a smooth spatial upscale; accumulated pixels use
	// the kernel narrowed by the upscale ratio so each frame only contributes near its samples.
	// "Accumulated" is measured against min(cap, 1). Otherwise a cap below 1 (high
	// HistoryBlend at 2x: (1/0.3 - 1) * 0.235 = 0.55) would never narrow the kernel fully, and
	// the CPU-side E[c] the cap was derived from (at full KernelScale) would no longer match.
	const float narrowT = saturate(historyWeight * rcp(max(min(maxWeight, 1.0f), 1e-3f)));
	const float2 kernelScale = lerp(1.0f, g_Pass.KernelScale, narrowT);
	float3 weightedSum = 0.0f;
	float weightSum = 0.0f;
	[unroll]
	for (uint t3 = 0u; t3 < 9u; ++t3)
	{
		const float2 x = offsets[t3] * kernelScale;
		const float x2 = dot(x, x);
		float w = BlackmanHarrisKernel(x2);
		if (lanczosAmount > 0.0f)
		{
			w = lerp(w, LanczosKernel(x2), lanczosAmount);
		}
		w = inside[t3] ? w : 0.0f;
		weightedSum += taps[t3].Linear * w;
		weightSum += w;
	}
	float3 currentLinear;
	float confidence;
	if (weightSum > 1e-4f)
	{
		// Normalised kernel average of linear radiance, so the time average over the jitter
		// cycle preserves energy for any input. Clamped to the neighbourhood range: removes the
		// Lanczos ringing (FSR 2 deringing), which on HDR edges can go negative.
		currentLinear = clamp(weightedSum / weightSum, linearMin, linearMax);
		confidence = saturate(weightSum);
	}
	else
	{
		currentLinear = taps[4].Linear;
		confidence = 0.0f;
	}
	const float3 currentWorking = ToWorkingSpace(currentLinear, g_Pass.Exposure);

	// --- History rectification ------------------------------------------------------------
	const float invCount = rcp(max(count, 1.0f));
	const float3 mean = m1 * invCount;
	const float3 sigmaSpatial = sqrt(max(m2 * invCount - mean * mean, 0.0f));
	float rectification = 1.0f;
	if (historyWeight > 0.0f)
	{
		// Temporal spread of this pixel's input, plus a prior from the spatial spread that
		// fades as the history fills (a freshly disoccluded pixel has seen one or two samples;
		// without a prior its first noisy frames would reject each other).
		const float temporalVariance = moments.a + sigmaSpatial.x * sigmaSpatial.x * rcp(1.0f + historyWeight);
		// Fades with motion. Once the history is resampled every frame, its difference from the
		// current frame is mostly reprojection blur, which the temporal variance would learn and
		// then tolerate: blur accumulated (gradient energy 0.59 of the input after 90 frames of
		// strafing, 0.90 with classic clipping) and an edge left a trail. At rest the slack is
		// complete; at 1 display px / frame it is a third.
		// The floor (1/512 in tonemapped units, below one 8-bit display step) absorbs the fp16
		// rounding of the stored mean: on a smooth, static gradient (the mirror quad in raster
		// preview) the box is ~1e-4 wide and 7% of the pixels were clipped every frame.
		const float slackScale = g_Pass.TemporalGamma * rcp(1.0f + 2.0f * speed);
		const float3 temporalSlack = slackScale * sqrt(temporalVariance) + kRectificationFloor;
		float3 lo;
		float3 hi;
		if (HasFlag(UpscalerFlag_VarianceClip))
		{
			// Hull of two windows, intersected with the neighbourhood's min/max box widened by
			// the temporal slack:
			//  - the neighbourhood mean +- (spatial + temporal) spread. Centred on the mean, not
			//    on this pixel's value: a firefly landing here moves the 3x3 mean by 1/9 only,
			//    so it cannot drag the history toward itself through the clip. (Centred on the
			//    current value, every firefly frame clipped the history up and every black frame
			//    could not clip it down: +118% in the interior after 700 frames at 16 spp.)
			//  - this pixel's current value +- its temporal spread, so a thin feature the
			//    history has converged on is not pulled toward its darker 3x3 mean (classic
			//    variance clipping dims one-pixel highlights).
			const float3 extent = g_Pass.VarianceGamma * sigmaSpatial + temporalSlack;
			lo = min(mean - extent, currentWorking - temporalSlack);
			hi = max(mean + extent, currentWorking + temporalSlack);
			lo = max(lo, boxMin - temporalSlack);
			hi = min(hi, boxMax + temporalSlack);
		}
		else
		{
			lo = boxMin - temporalSlack;
			hi = boxMax + temporalSlack;
		}
		// The deringed current value can sit a hair outside the YCoCg box (the clamp above is
		// per RGB channel); keep it inside so ClipFactor's segment is well defined.
		lo = min(lo, currentWorking);
		hi = max(hi, currentWorking);
		rectification = ClipFactor(moments.rgb, currentWorking, lo, hi);

		// The mean moves with the colour. The variance only shrinks with the confidence: the gap
		// that caused the clip is a change, not noise (lerping raw second moments would add
		// t (1 - t) gap^2 to it, and a ghost would widen its own tolerance).
		history.rgb = lerp(currentLinear, history.rgb, rectification);
		moments.rgb = lerp(currentWorking, moments.rgb, rectification);
		moments.a *= rectification;
		historyWeight *= rectification;
	}

	// --- Accumulation ---------------------------------------------------------------------
	const float totalWeight = historyWeight + confidence;
	// Running confidence-weighted mean: low-confidence frames (no sample near this pixel)
	// barely move a converged history; with no history the current frame is taken as is.
	const float alpha = totalWeight > 1e-4f ? confidence / totalWeight : 1.0f;
	float3 result;
	if (HasFlag(UpscalerFlag_AntiFlicker))
	{
		// Optional Karis 2014 weighting by 1 / (1 + L) of the exposure-scaled luminance:
		// suppresses sporadic bright samples at the price of a downward bias (see header).
		const float wc = alpha * rcp(1.0f + Luminance(currentLinear * g_Pass.Exposure));
		const float wh = (1.0f - alpha) * rcp(1.0f + Luminance(history.rgb * g_Pass.Exposure));
		result = (currentLinear * wc + history.rgb * wh) * rcp(max(wc + wh, 1e-8f));
	}
	else
	{
		result = lerp(history.rgb, currentLinear, alpha);
	}
	result = SanitizeHdr(result);
	// Exponentially weighted mean and variance (West 1979 incremental form):
	// mean' = mean + alpha d, var' = (1 - alpha) (var + alpha d^2), d = x - mean. Variance rather
	// than the raw second moment is stored, so reprojection, which interpolates between pixels,
	// does not mistake the difference of two neighbours' means for noise.
	const float delta = currentWorking.x - moments.r;
	float4 newMoments;
	newMoments.rgb = lerp(moments.rgb, currentWorking, alpha);
	newMoments.a = (1.0f - alpha) * (moments.a + alpha * delta * delta);
	newMoments = all(isfinite(newMoments)) ? clamp(newMoments, -kUpscalerMaxHdr, kUpscalerMaxHdr) : 0.0f;
	const float newWeight = min(totalWeight, maxWeight);

	RwTex2D(g_Pass.OutHistory)[pixel] = float4(result, newWeight);
	RwTex2D(g_Pass.OutHistoryMoments)[pixel] = newMoments;

	if (g_Pass.OutColor != FRT_INVALID_INDEX)
	{
		float3 output = result;
		if (g_Pass.DebugMode == UpscalerDebug_HistoryWeight)
		{
			output = (newWeight / max(g_Pass.MaxHistoryWeight, 1e-4f)) * g_Pass.InvExposure;
		}
		else if (g_Pass.DebugMode == UpscalerDebug_Disocclusion)
		{
			output = float3(1.0f - validity, confidence, 1.0f - rectification) * g_Pass.InvExposure;
		}
		else if (g_Pass.DebugMode == UpscalerDebug_MotionVectors)
		{
			output = float3(abs(motion * g_Pass.DisplaySize * g_Pass.InvRenderSize) * 0.1f, 0.0f) * g_Pass.InvExposure;
		}
		RwTex2D(g_Pass.OutColor)[pixel] = float4(output, 1.0f);
	}
}
