// CDenoiser pass 2: history fix for disocclusions (one thread per pixel, 8x8 groups).
//
// A pixel whose history is shorter than HistoryFixFrames (disocclusion, screen edge, camera cut)
// is replaced by an edge-aware blur of the accumulated signal around it (ReLAX "HistoryFix").
// The radius shrinks to zero as the history grows. Taps are weighted by plane distance, normal
// (and roughness / lobe for specular) and by their own history length (1 + length), so converged
// neighbours of the same surface dominate the fill. None of the weights depends on the radiance,
// so the fill is unbiased even for the sparse signals this denoiser sees (most 1-spp samples
// exactly zero). A rotated Vogel disk (golden-angle spiral, per-pixel random rotation) turns the
// sparse kernel's structure into noise the a-trous pass removes. Specular radius is limited to
// the screen footprint of the reflection blur (sharp reflections are not smeared).
//
// Only young pixels are written (Fixed*): a-trous iteration 0 reads Fixed* for them and the
// temporal result (Accum*) for every other pixel. Tiles without young pixels (flagged by the
// temporal pass) return after one fetch per pixel of a tiny tile texture. Without a-trous iterations (DenoiserFlag_NoAtrous) this pass writes the history and the
// outputs of every pixel instead.

#include "Denoiser/DenoiserCommon.hlsli"

static const uint kFixTaps = 16u;
static const float kGoldenAngle = 2.39996323f;

float3 HistoryFixDiffuse (int2 Pixel, SDnGuide G, float3 X, float4 Centre, float Radius, float Rotation)
{
	float w0 = 1.0f + Centre.a;
	float3 sum = Centre.rgb * w0;
	float weightSum = w0;
	// Disocclusion fill is allowed to be looser than the a-trous edge stopping.
	const float invDepthTolerance = 1.0f / max(2.0f * g_Pass.PhiDepth * G.ViewZ, 1e-6f);
	const float normalPower = max(g_Pass.PhiNormal * 0.125f, 1.0f);
	float s, c;
	sincos(Rotation, s, c);
	Texture2D<float4> accum = Tex2D(g_Pass.AccumDiff);
	[unroll]
	for (uint k = 0u; k < kFixTaps; ++k)
	{
		const float r = sqrt((float(k) + 0.5f) / float(kFixTaps));
		const float2 b = float2(cos(float(k) * kGoldenAngle), sin(float(k) * kGoldenAngle)) * r;
		const float2 offset = float2(b.x * c - b.y * s, b.x * s + b.y * c) * Radius;
		const int2 q = Pixel + int2(round(offset));
		if (!DnInScreen(q) || all(q == Pixel))
		{
			continue;
		}
		const SDnGuide gq = DnUnpackGuide(DnTex2DUint2(g_Pass.CurGuide)[q]);
		// (A reflected floor seen in a PSR mirror lies on the real floor's plane: never mix them.)
		if (DnIsSky(gq.ViewZ) || ViewZSeenInMirror(gq.ViewZ) != ViewZSeenInMirror(G.ViewZ))
		{
			continue;
		}
		const float wg = saturate(1.0f - abs(dot(G.N, DnCurPosition(q, gq.ViewZ) - X)) * invDepthTolerance);
		const float wn = pow(saturate(dot(G.N, gq.N)), normalPower);
		float w = wg * wn;
		if (w <= 0.0f)
		{
			continue;
		}
		const float4 tap = accum[q];
		w *= 1.0f + tap.a;
		sum += w * tap.rgb;
		weightSum += w;
	}
	return sum / weightSum;
}

float3 HistoryFixSpecular (int2 Pixel, SDnGuide G, float3 X, float4 Centre, float Radius, float Rotation)
{
	float w0 = 1.0f + Centre.a;
	float3 sum = Centre.rgb * w0;
	float weightSum = w0;
	const float invDepthTolerance = 1.0f / max(2.0f * g_Pass.PhiDepth * G.ViewZ, 1e-6f);
	const float lobeAngle = DnSpecularLobeHalfAngle(G.Roughness) * max(2.0f * g_Pass.SpecularLobeFraction, 0.05f) + 0.01f;
	float s, c;
	sincos(Rotation, s, c);
	Texture2D<float4> accum = Tex2D(g_Pass.AccumSpec);
	[unroll]
	for (uint k = 0u; k < kFixTaps; ++k)
	{
		const float r = sqrt((float(k) + 0.5f) / float(kFixTaps));
		const float2 b = float2(cos(float(k) * kGoldenAngle), sin(float(k) * kGoldenAngle)) * r;
		const float2 offset = float2(b.x * c - b.y * s, b.x * s + b.y * c) * Radius;
		const int2 q = Pixel + int2(round(offset));
		if (!DnInScreen(q) || all(q == Pixel))
		{
			continue;
		}
		const SDnGuide gq = DnUnpackGuide(DnTex2DUint2(g_Pass.CurGuide)[q]);
		// (A reflected floor seen in a PSR mirror lies on the real floor's plane: never mix them.)
		if (DnIsSky(gq.ViewZ) || ViewZSeenInMirror(gq.ViewZ) != ViewZSeenInMirror(G.ViewZ))
		{
			continue;
		}
		const float wg = saturate(1.0f - abs(dot(G.N, DnCurPosition(q, gq.ViewZ) - X)) * invDepthTolerance);
		float w = wg * DnLobeNormalWeight(dot(G.N, gq.N), lobeAngle) * DnRoughnessWeight(G.Roughness, gq.Roughness);
		if (w <= 0.0f)
		{
			continue;
		}
		const float4 tap = accum[q];
		w *= 1.0f + tap.a;
		sum += w * tap.rgb;
		weightSum += w;
	}
	return sum / weightSum;
}

// Both signals with one radius (the common case for rough surfaces, and every pixel after a
// camera cut): the taps share the guide fetch and the plane test.
void HistoryFixBoth (int2 Pixel, SDnGuide G, float3 X, float4 CentreD, float4 CentreS, float Radius, float Rotation,
	out float3 OutD, out float3 OutS)
{
	const float w0D = 1.0f + CentreD.a;
	const float w0S = 1.0f + CentreS.a;
	float3 sumD = CentreD.rgb * w0D;
	float3 sumS = CentreS.rgb * w0S;
	float weightD = w0D;
	float weightS = w0S;
	const float invDepthTolerance = 1.0f / max(2.0f * g_Pass.PhiDepth * G.ViewZ, 1e-6f);
	const float normalPower = max(g_Pass.PhiNormal * 0.125f, 1.0f);
	const float lobeAngle = DnSpecularLobeHalfAngle(G.Roughness) * max(2.0f * g_Pass.SpecularLobeFraction, 0.05f) + 0.01f;
	float s, c;
	sincos(Rotation, s, c);
	Texture2D<float4> accumD = Tex2D(g_Pass.AccumDiff);
	Texture2D<float4> accumS = Tex2D(g_Pass.AccumSpec);
	[unroll]
	for (uint k = 0u; k < kFixTaps; ++k)
	{
		const float r = sqrt((float(k) + 0.5f) / float(kFixTaps));
		const float2 b = float2(cos(float(k) * kGoldenAngle), sin(float(k) * kGoldenAngle)) * r;
		const float2 offset = float2(b.x * c - b.y * s, b.x * s + b.y * c) * Radius;
		const int2 q = Pixel + int2(round(offset));
		if (!DnInScreen(q) || all(q == Pixel))
		{
			continue;
		}
		const SDnGuide gq = DnUnpackGuide(DnTex2DUint2(g_Pass.CurGuide)[q]);
		// (A reflected floor seen in a PSR mirror lies on the real floor's plane: never mix them.)
		if (DnIsSky(gq.ViewZ) || ViewZSeenInMirror(gq.ViewZ) != ViewZSeenInMirror(G.ViewZ))
		{
			continue;
		}
		const float wg = saturate(1.0f - abs(dot(G.N, DnCurPosition(q, gq.ViewZ) - X)) * invDepthTolerance);
		if (wg <= 0.0f)
		{
			continue;
		}
		const float cosN = dot(G.N, gq.N);
		float wD = wg * pow(saturate(cosN), normalPower);
		if (wD > 0.0f)
		{
			const float4 tap = accumD[q];
			wD *= 1.0f + tap.a;
			sumD += wD * tap.rgb;
			weightD += wD;
		}
		float wS = wg * DnLobeNormalWeight(cosN, lobeAngle) * DnRoughnessWeight(G.Roughness, gq.Roughness);
		if (wS > 0.0f)
		{
			const float4 tap = accumS[q];
			wS *= 1.0f + tap.a;
			sumS += wS * tap.rgb;
			weightS += wS;
		}
	}
	OutD = sumD / weightD;
	OutS = sumS / weightS;
}

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Pass.Size))
	{
		return;
	}
	const int2 pixel = int2(Id.xy);
	const bool bNoAtrous = DnFlag(DenoiserFlag_NoAtrous);
	const bool bDiff = DnFlag(DenoiserFlag_Diffuse);
	const bool bSpec = DnFlag(DenoiserFlag_Specular);
	const bool bFix = DnFlag(DenoiserFlag_HistoryFix) && g_Pass.HistoryFixFrames > 1.0f;
	Texture2D<uint> youngTiles = ResourceDescriptorHeap[g_Pass.YoungTiles];
	if (!bNoAtrous && youngTiles[Id.xy / 8u] == 0u)
	{
		return;
	}

	const float4 diff = Tex2D(g_Pass.AccumDiff)[pixel];
	const float4 spec = Tex2D(g_Pass.AccumSpec)[pixel];
	const bool bYoungD = bFix && bDiff && diff.a < g_Pass.HistoryFixFrames;
	const bool bYoungS = bFix && bSpec && spec.a < g_Pass.HistoryFixFrames;
	if (!bNoAtrous && !bYoungD && !bYoungS)
	{
		return;
	}

	const SDnGuide g = DnUnpackGuide(DnTex2DUint2(g_Pass.CurGuide)[pixel]);
	if (DnIsSky(g.ViewZ))
	{
		if (bNoAtrous)
		{
			// Sky passes through untouched.
			RwTex2D(g_Pass.OutDiffHistory)[pixel] = 0.0f;
			RwTex2D(g_Pass.OutSpecHistory)[pixel] = 0.0f;
			RwTex2D(g_Pass.OutMomentsHistory)[pixel] = 0.0f;
			RwTex2D(g_Pass.OutDiffuse)[pixel] = DnPassThrough(Tex2D(g_Pass.InDiffuse)[pixel]);
			RwTex2D(g_Pass.OutSpecular)[pixel] = DnPassThrough(Tex2D(g_Pass.InSpecular)[pixel]);
		}
		return;
	}

	const float3 X = DnCurPosition(pixel, g.ViewZ);
	const float rotation = DnHashToUnit(uint2(pixel), g_Pass.FrameIndex) * kTwoPi;
	const float4 fast = Tex2D(g_Pass.CurFast)[pixel];
	const float specHit = fast.w;

	float radiusD = 0.0f;
	float radiusS = 0.0f;
	if (bYoungD)
	{
		radiusD = g_Pass.HistoryFixRadius * (1.0f - saturate((diff.a - 1.0f) / (g_Pass.HistoryFixFrames - 1.0f)));
	}
	if (bYoungS)
	{
		// Limited to the reflection's own blur footprint, but never below a quarter of the diffuse
		// radius: a fresh mirror pixel is noisy too (its reflected point is path traced).
		const float footprint = DnSpecularFootprintPixels(g.Roughness, specHit, g.ViewZ) * g_Pass.SpecularFootprintScale;
		radiusS = clamp(footprint, 0.25f * g_Pass.HistoryFixRadius, g_Pass.HistoryFixRadius) *
			(1.0f - saturate((spec.a - 1.0f) / (g_Pass.HistoryFixFrames - 1.0f)));
	}
	float3 colorD = diff.rgb;
	float3 colorS = spec.rgb;
	if (radiusD >= 1.0f && radiusS >= 1.0f && abs(radiusD - radiusS) < 0.5f)
	{
		HistoryFixBoth(pixel, g, X, diff, spec, radiusD, rotation, colorD, colorS);
	}
	else
	{
		if (radiusD >= 1.0f)
		{
			colorD = HistoryFixDiffuse(pixel, g, X, diff, radiusD, rotation);
		}
		if (radiusS >= 1.0f)
		{
			colorS = HistoryFixSpecular(pixel, g, X, spec, radiusS, rotation + 1.0f);
		}
	}

	// Unbiased rounding to half precision (DnRoundToHalf): the fixed values become history. Pixels
	// that were not fixed are already exact half values.
	const float4 historyD = DnRoundColorToHalf(colorD, diff.a, DnRoundingSeed(pixel, 4u));
	const float4 historyS = DnRoundColorToHalf(colorS, spec.a, DnRoundingSeed(pixel, 5u));
	if (bNoAtrous)
	{
		RwTex2D(g_Pass.OutDiffHistory)[pixel] = historyD;
		RwTex2D(g_Pass.OutSpecHistory)[pixel] = historyS;
		RwTex2D(g_Pass.OutMomentsHistory)[pixel] = Tex2D(g_Pass.CurMoments)[pixel];
		RwTex2D(g_Pass.OutDiffuse)[pixel] = bDiff ? float4(historyD.rgb, fast.y) : DnPassThrough(Tex2D(g_Pass.InDiffuse)[pixel]);
		RwTex2D(g_Pass.OutSpecular)[pixel] = bSpec ? float4(historyS.rgb, fast.w) : DnPassThrough(Tex2D(g_Pass.InSpecular)[pixel]);
		return;
	}
	// Both signals are written whenever either is young: iteration 0 picks per signal.
	RwTex2D(g_Pass.FixedDiffUav)[pixel] = historyD;
	RwTex2D(g_Pass.FixedSpecUav)[pixel] = historyS;
}
