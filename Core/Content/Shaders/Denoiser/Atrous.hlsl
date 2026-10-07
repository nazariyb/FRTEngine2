// CDenoiser a-trous iteration (one dispatch per iteration, diffuse + specular together); the tap
// weights and the energy-conserving update are in AtrousCommon.hlsli.
//
// Iteration 0 (permutation ATROUS_FIRST) reads the temporal result (rgb, length; the history-fix
// result for young pixels) and the noise texture (Temporal.hlsl: deviations, specular extent and
// hit distance) and
//  - writes next frame's history: the unfiltered history candidate, or with DenoiserFlag_Feedback
//    its own filtered output (recurrent filtering as in SVGF / ReBLUR; with
//    DenoiserFlag_FeedbackMoments the moments get the same weights). The history length is
//    shortened where the pooled anti-lag test detects a lighting change;
//  - writes the per-pixel aux texel (specular extent and hit distance) the later iterations read
//    for the centre and every specular tap.
// Later iterations read and write (rgb, luminance deviation) and rotate their taps (see main).
// The last one writes the final outputs (rgb, denoised hit distance) and passes sky pixels
// through untouched.

#include "Denoiser/AtrousCommon.hlsli"

#ifndef ATROUS_FIRST
#define ATROUS_FIRST 0
#endif

ConstantBuffer<SDenoiserAtrousRootConstants> g_Atrous : register(b0);

#if ATROUS_FIRST
// --- Anti-lag ------------------------------------------------------------------------------------------
// The temporal pass leaves per-8x8-tile sums of the slow and the fast history luminance and of the
// variance of the fast mean. Each group (= one tile) pools the 5x5 tiles around it (40x40 pixels:
// a sparse 1-spp signal has too few non-zero samples in one tile to tell a lighting change from
// noise - measured: an 8x8 test never fired on a 4x input step) and tests the change of the mean
// against the standard error of the fast mean. Only a change that is both significant
// (AntilagSigma standard errors) and large (a fraction of the mean) shortens the histories written
// for the next frame towards the fast length. Per-pixel neighbourhood clamping (ReLAX) reads
// "zero" for such a signal most of the time and resets converged histories every frame (measured
// with the previous clamp: 64x64 block means at 0.04x the reference).
static const float kAntilagMinRelativeChange = 0.2f;

groupshared float4 gs_TileD[25];
groupshared float4 gs_TileS[25];
groupshared float2 gs_Antilag;

float AntilagAmount (float4 Sums)
{
	if (Sums.w < 1.0f)
	{
		return 0.0f;
	}
	const float slow = Sums.x / Sums.w;
	const float fast = Sums.y / Sums.w;
	const float standardError = sqrt(max(Sums.z, 0.0f)) / Sums.w;
	const float change = abs(fast - slow);
	const float significance = change / (g_Pass.AntilagSigma * standardError + 1e-12f);
	const float relative = change / (kAntilagMinRelativeChange * max(slow, fast) + 1e-12f);
	return saturate((min(significance, relative) - 1.0f) * g_Pass.AntilagSensitivity);
}

float2 GroupAntilag (uint2 GroupId, uint GroupIndex)
{
	if (GroupIndex < 25u)
	{
		Texture2D<float4> tilesD = Tex2D(g_Pass.AntilagTilesD);
		Texture2D<float4> tilesS = Tex2D(g_Pass.AntilagTilesS);
		const int2 tile = int2(GroupId) + int2(GroupIndex % 5u, GroupIndex / 5u) - 2;
		const int2 tileCount = int2((g_Pass.Size + 7u) / 8u);
		const bool bValid = all(tile >= 0) && all(tile < tileCount);
		gs_TileD[GroupIndex] = bValid ? tilesD[tile] : 0.0f;
		gs_TileS[GroupIndex] = bValid ? tilesS[tile] : 0.0f;
	}
	GroupMemoryBarrierWithGroupSync();
	if (GroupIndex == 0u)
	{
		float4 sumD = 0.0f;
		float4 sumS = 0.0f;
		for (uint i = 0u; i < 25u; ++i)
		{
			sumD += gs_TileD[i];
			sumS += gs_TileS[i];
		}
		gs_Antilag = float2(AntilagAmount(sumD), AntilagAmount(sumS));
	}
	GroupMemoryBarrierWithGroupSync();
	return gs_Antilag;
}
#endif

float AntilagLength (float Length, float Amount)
{
	return lerp(Length, min(Length, g_Pass.MaxFastHistoryLength), Amount);
}

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID, uint3 GroupId : SV_GroupID, uint GroupIndex : SV_GroupIndex)
{
	float2 antilag = 0.0f;
#if ATROUS_FIRST
	// Before any early return: every thread takes part (the flag is uniform).
	if (DnFlag(DenoiserFlag_AntiLag))
	{
		antilag = GroupAntilag(GroupId.xy, GroupIndex);
	}
#endif
	if (any(Id.xy >= g_Pass.Size))
	{
		return;
	}
	const int2 pixel = int2(Id.xy);
	// Iteration 0 is a separate permutation (ATROUS_FIRST): its extra inputs stay out of the
	// register budget of the later iterations.
	const bool bFirst = ATROUS_FIRST != 0;
	const bool bLast = (g_Atrous.IterationFlags & DenoiserAtrous_Last) != 0u;
	Texture2D<uint2> guide = DnTex2DUint2(g_Pass.CurGuide);
	const SDnGuide g = DnUnpackGuide(guide[pixel]);

	if (DnIsSky(g.ViewZ))
	{
		if (bFirst)
		{
			RwTex2D(g_Pass.OutDiffHistory)[pixel] = 0.0f;
			RwTex2D(g_Pass.OutSpecHistory)[pixel] = 0.0f;
			RwTex2D(g_Pass.OutMomentsHistory)[pixel] = 0.0f;
			RwTex2D(g_Pass.AtrousAuxUav)[pixel] = 0.0f;
		}
		// Sky passes through untouched.
		RwTex2D(g_Atrous.OutDiffuse)[pixel] = bLast ? DnPassThrough(Tex2D(g_Pass.InDiffuse)[pixel]) : 0.0f;
		RwTex2D(g_Atrous.OutSpecular)[pixel] = bLast ? DnPassThrough(Tex2D(g_Pass.InSpecular)[pixel]) : 0.0f;
		return;
	}

	Texture2D<float4> inDiff = Tex2D(g_Atrous.InDiffuse);
	Texture2D<float4> inSpec = Tex2D(g_Atrous.InSpecular);
	Texture2D<float4> noiseTex = Tex2D(g_Pass.Variance);
	Texture2D<float4> momentsTex = Tex2D(g_Pass.CurMoments);
	// Iteration 0 reads the temporal result, and the history-fix result for young pixels (the fix
	// pass writes only those).
	const bool bFixed = bFirst && DnFlag(DenoiserFlag_HistoryFix) && g_Pass.HistoryFixFrames > 1.0f;
	Texture2D<float4> fixedDiff = Tex2D(g_Pass.FixedDiff);
	Texture2D<float4> fixedSpec = Tex2D(g_Pass.FixedSpec);
	float4 cd = inDiff[pixel];
	float4 cs = inSpec[pixel];
	if (bFixed && (cd.a < g_Pass.HistoryFixFrames || cs.a < g_Pass.HistoryFixFrames))
	{
		cd = DnFlag(DenoiserFlag_Diffuse) && cd.a < g_Pass.HistoryFixFrames ? fixedDiff[pixel] : cd;
		cs = DnFlag(DenoiserFlag_Specular) && cs.a < g_Pass.HistoryFixFrames ? fixedSpec[pixel] : cs;
	}

	// Lengths only matter to iteration 0 (later ones see them folded into the deviations).
	const float lengthD = cd.a;
	const float lengthS = cs.a;
	Texture2D<float4> auxTex = Tex2D(g_Pass.AtrousAux);
	float footprint, specHit;
	float2 deviation;
	if (bFirst)
	{
		const float4 noise = noiseTex[pixel];
		deviation = noise.xy;
		footprint = noise.z;
		specHit = noise.w;
		// Footprints and hit distances are similarity inputs: the 4-byte float format (6/6-bit
		// mantissas) is plenty.
		RwTex2D(g_Pass.AtrousAuxUav)[pixel] = float4(min(footprint, 60000.0f), min(specHit, 60000.0f), 0.0f, 0.0f);
	}
	else
	{
		const float3 aux = auxTex[pixel].rgb;
		footprint = aux.x;
		specHit = aux.y;
		deviation = float2(cd.a, cs.a);
	}

	const float stepSize = float(g_Atrous.StepSize);
	const SAtrousCentre c = MakeAtrousCentre(pixel, g, deviation, cd.rgb, cs.rgb, footprint, specHit, stepSize);
	SAtrousSums sums = BeginAtrousSums();

	// Iteration 0 also filters the moments (decoded: m1, m2) with the colour weights.
	const bool bFilterMoments = bFirst && DnFlag(DenoiserFlag_FeedbackMoments);
	const float4 centreMoments = bFirst ? momentsTex[pixel] : 0.0f;
	const float2 centreMomentsD = DnDecodeMoments(centreMoments.xy);
	const float2 centreMomentsS = DnDecodeMoments(centreMoments.zw);
	float2 momentsD = 0.0f;
	float2 momentsS = 0.0f;

	const int step = int(g_Atrous.StepSize);
	// Later iterations rotate their 3x3 pattern per 8x8 pixel block, frame and iteration: on a
	// sparse signal the regular a-trous lattice turns every rare bright sample into a grid of
	// copies ("+" stars, checker patterns in young regions); rotated taps turn that structure into
	// noise the history averages out. One rotation per block keeps the taps of a wave together in
	// memory (per-pixel rotation measured +0.55 ms at 1902x1033 for steps 8 and 16). Iteration 0
	// keeps the regular pattern - it feeds back, and only the regular lattice is exactly symmetric
	// (energy conserving, AtrousCommon.hlsli).
	const bool bRotate = !bFirst && DnFlag(DenoiserFlag_AtrousRotation);
	float2x2 rotation = float2x2(1.0f, 0.0f, 0.0f, 1.0f);
	if (bRotate)
	{
		float sn, cs;
		sincos(DnHashToUnit(uint2(pixel) >> 3u, g_Pass.FrameIndex * 8u + g_Atrous.StepSize) * kTwoPi, sn, cs);
		rotation = float2x2(cs, -sn, sn, cs);
	}
	// All fetches of a tap are issued together (out-of-screen loads return 0, which unpacks as
	// sky), so each iteration waits for memory once instead of once per dependent test.
	const bool bSpecHitTaps = !bFirst && c.bSpecFilter && g.Roughness < kAtrousHitRoughness;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			if (x == 0 && y == 0)
			{
				continue;
			}
			const int2 q = pixel + (bRotate ? int2(round(mul(rotation, float2(x, y)) * float(step))) : int2(x, y) * step);
			const uint2 packedGuide = guide[q];
			float4 tapD = inDiff[q];
			float4 tapS = inSpec[q];
			// First iteration: the tap's deviation, footprint and hit distance come from the noise
			// texture; later ones read footprint and hit distance from the aux texture - except for
			// rough centres, whose hit-distance weight is 1 and whose wide footprint only matters
			// next to a much smoother tap the roughness weight rejects anyway.
			float4 tapNoise = float4(0.0f, 0.0f, footprint, 0.0f);
			float4 tapMoments = 0.0f;
			if (bFirst)
			{
				tapNoise = noiseTex[q];
				if (bFilterMoments)
				{
					tapMoments = momentsTex[q];
				}
			}
			else if (bSpecHitTaps)
			{
				tapNoise.zw = auxTex[q].xy;
			}
			const SDnGuide gq = DnUnpackGuide(packedGuide);
			if (DnIsSky(gq.ViewZ))
			{
				continue;
			}
			const float h = kAtrousKernel1D[abs(x)] * kAtrousKernel1D[abs(y)];
			const float2 geometry = AtrousTapGeometry(c, q, gq);
			if (geometry.x <= 0.0f)
			{
				continue;
			}
			if (bFixed)
			{
				// Young taps: the history-fix result (rare, so a divergent dependent fetch is fine).
				tapD = tapD.a < g_Pass.HistoryFixFrames ? fixedDiff[q] : tapD;
				tapS = tapS.a < g_Pass.HistoryFixFrames ? fixedSpec[q] : tapS;
			}
			if (c.bDiff)
			{
				const float k = AddAtrousDiffuse(sums, c, h, geometry.x, geometry.y, tapD.rgb, bFirst ? tapNoise.x : tapD.a);
				momentsD += k * DnDecodeMoments(tapMoments.xy);
			}
			if (c.bSpecFilter)
			{
				// The pair's larger extent (symmetric, see AtrousCommon.hlsli).
				const float footprintWeight = AtrousFootprintWeight(max(footprint, tapNoise.z), stepSize);
				const float k = AddAtrousSpecular(sums, c, h, geometry.x, geometry.y, gq.Roughness, tapNoise.w, tapS.rgb,
					bFirst ? tapNoise.y : tapS.a, footprintWeight);
				momentsS += k * DnDecodeMoments(tapMoments.zw);
			}
		}
	}

	// Energy-conserving update: the kernel weight of rejected taps stays with the centre.
	const bool bDiff = DnFlag(DenoiserFlag_Diffuse);
	const bool bSpec = DnFlag(DenoiserFlag_Specular);
	const float selfD = 1.0f - sums.WeightD;
	const float selfS = 1.0f - sums.WeightS;
	const float3 outD = bDiff ? cd.rgb * selfD + sums.SumD : cd.rgb;
	const float3 outS = bSpec ? cs.rgb * selfS + sums.SumS : cs.rgb;

	// Stores that feed back into the next frame are rounded without bias (DnRoundToHalf): a
	// truncating fp16 store per history frame adds up to a visible loss. The one-shot stores of the
	// iterations (~0.035 % each) are left to the hardware conversion.
	if (bFirst)
	{
		// Without feedback the history candidate is already an exact half value (Temporal / HistoryFix).
		const bool bFeedback = DnFlag(DenoiserFlag_Feedback);
		const uint seed = DnRoundingSeed(pixel, 6u);
		RwTex2D(g_Pass.OutDiffHistory)[pixel] = bFeedback ? DnRoundColorToHalf(outD, AntilagLength(lengthD, antilag.x), seed) :
			float4(cd.rgb, AntilagLength(lengthD, antilag.x));
		RwTex2D(g_Pass.OutSpecHistory)[pixel] = bFeedback ? DnRoundColorToHalf(outS, AntilagLength(lengthS, antilag.y), DnHash(seed)) :
			float4(cs.rgb, AntilagLength(lengthS, antilag.y));
		float4 filteredMoments = centreMoments;
		if (bFilterMoments)
		{
			filteredMoments.xy = bDiff ? DnEncodeMoments(centreMomentsD * selfD + momentsD) : centreMoments.xy;
			filteredMoments.zw = bSpec ? DnEncodeMoments(centreMomentsS * selfS + momentsS) : centreMoments.zw;
			filteredMoments = DnRoundToHalf4(filteredMoments, DnRoundingSeed(pixel, 7u));
		}
		RwTex2D(g_Pass.OutMomentsHistory)[pixel] = filteredMoments;
	}
	if (bLast)
	{
		const float4 fast = Tex2D(g_Pass.CurFast)[pixel];
		float4 finalD = bDiff ? float4(outD, fast.y) : DnPassThrough(Tex2D(g_Pass.InDiffuse)[pixel]);
		float4 finalS = bSpec ? float4(outS, fast.w) : DnPassThrough(Tex2D(g_Pass.InSpecular)[pixel]);
		finalD.rgb = DnFlag(DenoiserFlag_OutputSpecularOnly) ? 0.0f : finalD.rgb;
		finalS.rgb = DnFlag(DenoiserFlag_OutputDiffuseOnly) ? 0.0f : finalS.rgb;
		if (DnFlag(DenoiserFlag_OutputDiffuseOnly) && DnFlag(DenoiserFlag_OutputSpecularOnly))
		{
			// Debug: the history-length texture (length / max of the signal HistoryDebugSignal picks;
			// needs DenoiserFlag_HistoryLengthDebug) as a grey diffuse signal.
			finalD.rgb = DnFlag(DenoiserFlag_HistoryLengthDebug) ? RwTex2DFloat(g_Pass.HistoryLengthUav)[pixel] : 1.0f;
			finalS.rgb = 0.0f;
		}
		RwTex2D(g_Atrous.OutDiffuse)[pixel] = finalD;
		RwTex2D(g_Atrous.OutSpecular)[pixel] = finalS;
	}
	else
	{
		const float outDevD = bDiff ? min(sqrt(selfD * selfD * c.VarD + sums.VarD), kDnMaxRadiance) : 0.0f;
		const float outDevS = bSpec ? min(sqrt(selfS * selfS * c.VarS + sums.VarS), kDnMaxRadiance) : 0.0f;
		RwTex2D(g_Atrous.OutDiffuse)[pixel] = float4(outD, outDevD);
		RwTex2D(g_Atrous.OutSpecular)[pixel] = float4(outS, outDevS);
	}
}
