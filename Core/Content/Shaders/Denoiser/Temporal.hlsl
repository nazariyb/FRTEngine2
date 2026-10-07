// CDenoiser pass 1: temporal accumulation (one thread per pixel, 8x8 groups = anti-lag tiles).
//
// Reprojection (Schied et al. 2017 "SVGF"; NVIDIA NRD ReLAX):
//  - diffuse and surface-specular history: the surface point is followed with the motion vector.
//    The 2x2 bilinear footprint is weighted per tap by an occlusion test - distance of the
//    history surface point to the current tangent plane (relative to view depth) and the angle
//    between the normals - and renormalised. When every tap fails (thin geometry, sub-pixel
//    motion-vector error at silhouettes) the best tap of the surrounding 3x3 is used instead.
//    Optionally (DenoiserFlag_CatmullRom, off by default - see SDenoiserSettings) the slow history
//    is fetched with a 5-tap Catmull-Rom filter clamped to the 2x2 range;
//  - specular history additionally uses virtual motion: the reflected image of a planar mirror
//    lies hitT behind the surface along the view ray, X_virtual = X - V * hitT * D, with D the
//    GGX dominant-direction factor (rough lobes have no stable reflected image, D -> 0). The
//    virtual history is accepted where its taps lie on the current plane with matching normal,
//    roughness and hit distance, and blended with the surface history by D and that
//    confidence (ReLAX "virtual history amount");
//  - specular history length is capped by parallax: while the view direction sweeps through
//    the reflection lobe the old samples stop describing the current reflection, so a lobe of
//    half-angle theta tolerates about theta / dtheta frames of view change dtheta per frame.
//
// Accumulation: length' = min(length * sqrt(footprint quality) + 1, max), weight 1 / length'
// (exact running mean until the cap, then an exponential window) - unbiased for any signal, which
// is why the history keeps the raw samples: with the a-trous output fed back instead (recurrent,
// DenoiserFlag_Feedback) reflections blurred and smeared under motion without a static gain. A
// fast history with a short cap (anti-lag reference) and luminance moments for the noise estimate
// are carried along. Hit distances are accumulated over the fast window, skipping frames where
// the lobe was not sampled (hit distance 0).
//
// The pass also writes the noise texture for the a-trous pass and the per-tile anti-lag sums
// (see the sections at the end).

#include "Denoiser/DenoiserCommon.hlsli"

struct STapTest
{
	bool bValid;          // same surface (plane + normal test)
	float SpecWeight;     // roughness * lobe-normal similarity
	float PlaneDistance;
};

// SeenInMirror: ViewZSeenInMirror of the current pixel; a history texel must match it (a mirror's
// virtual floor lies on the real floor's plane and would pass the plane test).
STapTest TestHistoryTap (int2 Texel, float3 N, float Roughness, float3 Xexpected, float PlaneThreshold, float SpecAngle, uint SeenInMirror)
{
	STapTest t;
	t.bValid = false;
	t.SpecWeight = 0.0f;
	t.PlaneDistance = kFloatMax;
	if (!DnInScreen(Texel))
	{
		return t;
	}
	const SDnGuide g = DnUnpackGuide(DnTex2DUint2(g_Pass.PrevGuide)[Texel]);
	if (DnIsSky(g.ViewZ) || ViewZSeenInMirror(g.ViewZ) != SeenInMirror)
	{
		return t;
	}
	const float3 P = DnPrevPosition(Texel, g.ViewZ);
	const float cosN = dot(N, g.N);
	// Both tangent planes (as in the a-trous geometry weight): with the current one alone, any
	// history point lying on the current surface's extended plane passes - across depth edges on
	// grazing surfaces, thin pillars, banner rims. Measured on 90 frames of fast strafe + turn
	// (0.03 m + 0.004 rad / frame) against a reference at the final pose: relMSE 0.217 -> 0.106
	// (denoiser alone 0.232 -> 0.130), 1 cm / frame strafe 0.141 -> 0.130, static 0.051 -> 0.050,
	// SSIM +0.0003..0.0007 in all three.
	t.PlaneDistance = max(abs(dot(N, P - Xexpected)), abs(dot(g.N, P - Xexpected)));
	t.bValid = t.PlaneDistance <= PlaneThreshold && cosN >= g_Pass.ReprojectionNormalCos;
	t.SpecWeight = t.bValid ? DnRoughnessWeight(Roughness, g.Roughness) * DnLobeNormalWeight(cosN, SpecAngle) : 0.0f;
	return t;
}

void BilinearFootprint (float2 Position, out int2 Base, out float4 Weights)
{
	const float2 t = Position - 0.5f;
	const float2 b = floor(t);
	const float2 f = t - b;
	Base = int2(b);
	Weights = float4((1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y), (1.0f - f.x) * f.y, f.x * f.y);
}

// 5-tap Catmull-Rom (corners dropped; Jimenez 2016, "Filmic SMAA"), Position in texels.
float3 SampleCatmullRom (Texture2D<float4> Tex, float2 Position)
{
	const float2 centre = floor(Position - 0.5f) + 0.5f;
	const float2 f = Position - centre;
	const float2 f2 = f * f;
	const float2 f3 = f2 * f;
	const float2 w0 = -0.5f * f3 + f2 - 0.5f * f;
	const float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
	const float2 w2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
	const float2 w3 = 0.5f * f3 - 0.5f * f2;
	const float2 w12 = w1 + w2;
	const float2 tc0 = (centre - 1.0f) * g_Pass.InvSize;
	const float2 tc12 = (centre + w2 / w12) * g_Pass.InvSize;
	const float2 tc3 = (centre + 2.0f) * g_Pass.InvSize;
	const float wA = w12.x * w0.y;
	const float wB = w0.x * w12.y;
	const float wC = w12.x * w12.y;
	const float wD = w3.x * w12.y;
	const float wE = w12.x * w3.y;
	const float3 c =
		Tex.SampleLevel(s_LinearClamp, float2(tc12.x, tc0.y), 0.0f).rgb * wA +
		Tex.SampleLevel(s_LinearClamp, float2(tc0.x, tc12.y), 0.0f).rgb * wB +
		Tex.SampleLevel(s_LinearClamp, float2(tc12.x, tc12.y), 0.0f).rgb * wC +
		Tex.SampleLevel(s_LinearClamp, float2(tc3.x, tc12.y), 0.0f).rgb * wD +
		Tex.SampleLevel(s_LinearClamp, float2(tc12.x, tc3.y), 0.0f).rgb * wE;
	return c / (wA + wB + wC + wD + wE);
}

// Weighted history of one signal (sums, normalised by Finish()).
struct SHistory
{
	float3 Slow;
	float Length;
	float Fast;           // fast-history luminance
	float HitDistance;
	float2 Moments;       // (m1, m2), decoded
	float Weight;
	float3 SlowMin;
	float3 SlowMax;
};

SHistory EmptyHistory ()
{
	SHistory h;
	h.Slow = 0.0f;
	h.Length = 0.0f;
	h.Fast = 0.0f;
	h.HitDistance = 0.0f;
	h.Moments = 0.0f;
	h.Weight = 0.0f;
	h.SlowMin = kFloatMax;
	h.SlowMax = 0.0f;
	return h;
}

// Fast = (luminance, hit distance) of the packed fast history.
void AddHistoryTap (inout SHistory H, float W, float4 Slow, float2 Fast, float2 StoredMoments)
{
	H.Slow += W * Slow.rgb;
	H.Length += W * Slow.a;
	H.Fast += W * Fast.x;
	H.HitDistance += W * Fast.y;
	H.Moments += W * DnDecodeMoments(StoredMoments);
	H.Weight += W;
	H.SlowMin = min(H.SlowMin, Slow.rgb);
	H.SlowMax = max(H.SlowMax, Slow.rgb);
}

SHistory FinishHistory (SHistory H)
{
	if (H.Weight > 1e-6f)
	{
		const float inv = 1.0f / H.Weight;
		H.Slow *= inv;
		H.Length *= inv;
		H.Fast *= inv;
		H.HitDistance *= inv;
		H.Moments *= inv;
	}
	else
	{
		H = EmptyHistory();
	}
	return H;
}

SHistory LerpHistory (SHistory A, SHistory B, float T)
{
	SHistory h = A;
	h.Slow = lerp(A.Slow, B.Slow, T);
	h.Length = lerp(A.Length, B.Length, T);
	h.Fast = lerp(A.Fast, B.Fast, T);
	h.HitDistance = lerp(A.HitDistance, B.HitDistance, T);
	h.Moments = lerp(A.Moments, B.Moments, T);
	return h;
}

struct SAccumulated
{
	float3 Slow;
	float Length;
	float Fast;           // luminance
	float HitDistance;
	float2 Moments;
};

// Blends the current sample into the (already validated) history. Quality in [0, 1] scales the
// carried length (partial footprints converge faster), LengthCap bounds the result.
// Input firefly suppression (high threshold only): a sample brighter than FireflyThreshold x last
// frame's denoised luminance at the reprojected position (Reference - an a-trous output, i.e. a
// mean over tens of pixels) is scaled down to that limit. Rare large samples are the signal itself
// here: in the demo scene 1 % of the 1-spp samples are non-zero, their median is 2x and their 99th
// percentile 1500x the local mean, and the samples above 1000x carry 2 % of the energy - the
// limit is meant for those, which an a-trous kernel can only spread into star-shaped blotches.
// The per-pixel history mean is no reference for such a signal (it is far below the true mean
// most of the time: measured 64x64 block means at 0.49x the reference with a 1000x limit on it).
// Young histories and references of 0 (no energy seen yet) are left alone.
float3 SuppressFirefly (float3 Sample, float Reference, bool bHistory, float HistoryLength)
{
	if (!DnFlag(DenoiserFlag_Firefly) || !bHistory || HistoryLength < 4.0f || !(Reference > 0.0f))
	{
		return Sample;
	}
	const float limit = g_Pass.FireflyThreshold * Reference;
	const float L = Luminance(Sample);
	return L > limit ? Sample * (limit / L) : Sample;
}

SAccumulated Accumulate (float3 Sample, float HitDistance, SHistory H, bool bHistory, float Quality, float LengthCap,
	float FireflyReference)
{
	SAccumulated a;
	Sample = SuppressFirefly(Sample, FireflyReference, bHistory, H.Length);
	const float L = Luminance(Sample);
	const float prevLength = bHistory ? H.Length * sqrt(saturate(Quality)) : 0.0f;
	a.Length = clamp(prevLength + 1.0f, 1.0f, max(LengthCap, 1.0f));
	const float alpha = 1.0f / a.Length;
	const float fastAlpha = 1.0f / min(a.Length, g_Pass.MaxFastHistoryLength);
	const float momentAlpha = max(alpha, g_Pass.MomentsMinAlpha);
	if (bHistory)
	{
		a.Slow = lerp(H.Slow, Sample, alpha);
		a.Fast = lerp(H.Fast, L, fastAlpha);
		a.Moments = lerp(H.Moments, float2(L, L * L), momentAlpha);
		const float hHist = H.HitDistance;
		a.HitDistance = HitDistance > 0.0f ? (hHist > 0.0f ? lerp(hHist, HitDistance, fastAlpha) : HitDistance) : hHist;
	}
	else
	{
		a.Slow = Sample;
		a.Fast = L;
		a.Moments = float2(L, L * L);
		a.HitDistance = HitDistance;
	}
	return a;
}

struct STemporalPixel
{
	bool bSky;
	float3 N;
	float Roughness;
	float ViewZ;
	SAccumulated D;
	SAccumulated S;
};

SAccumulated EmptyAccumulated ()
{
	SAccumulated a;
	a.Slow = 0.0f;
	a.Length = 0.0f;
	a.Fast = 0.0f;
	a.HitDistance = 0.0f;
	a.Moments = 0.0f;
	return a;
}

// Reprojection + accumulation of one pixel (no writes).
STemporalPixel TemporalPixel (int2 pixel)
{
	STemporalPixel o;
	o.D = EmptyAccumulated();
	o.S = EmptyAccumulated();

	// --- Guide ---------------------------------------------------------------------------------
	const float viewZ = Tex2DFloat(g_Pass.InViewZ)[pixel];
	const uint seenInMirror = ViewZSeenInMirror(viewZ);
	const float4 normalRoughness = Tex2D(g_Pass.InNormalRoughness)[pixel];
	const float nLen2 = dot(normalRoughness.xyz, normalRoughness.xyz);
	const bool bSky = DnIsSky(viewZ) || !(nLen2 > 1e-8f) || !isfinite(nLen2);
	// Quantised exactly like the stored guide, so a static pixel compares equal to its own history:
	// comparing the raw values against the 12:12-bit normal / 8-bit roughness of the history made
	// the specular tap weights ~0.9 every frame, and length * sqrt(quality) + 1 then converges to
	// ~14 frames instead of MaxHistoryLength.
	const SDnGuide quantised = DnUnpackGuide(DnPackGuide(bSky ? float3(0.0f, 0.0f, 1.0f) : normalRoughness.xyz * rsqrt(nLen2),
		isfinite(normalRoughness.w) ? saturate(normalRoughness.w) : 1.0f, 1.0f));
	const float3 N = quantised.N;
	const float roughness = quantised.Roughness;
	o.bSky = bSky;
	o.N = N;
	o.Roughness = roughness;
	o.ViewZ = bSky ? kDnSkyMarker : viewZ;
	if (bSky)
	{
		return o;
	}

	const float4 diffIn = Tex2D(g_Pass.InDiffuse)[pixel];
	const float4 specIn = Tex2D(g_Pass.InSpecular)[pixel];
	const float3 diffuse = DnSanitizeRadiance(diffIn.rgb * g_Pass.InputScale);
	const float3 specular = DnSanitizeRadiance(specIn.rgb * g_Pass.InputScale);
	const float diffHit = DnSanitizeHitDistance(diffIn.a);
	const float specHit = DnSanitizeHitDistance(specIn.a);

	// --- Current surface and its expected previous position -----------------------------------
	const float3 X = DnCurPosition(pixel, viewZ);
	const float3 V = -normalize(X);
	const float NoV = saturate(dot(N, V));

	float2 mv = Tex2DFloat2(g_Pass.InMotionVectors)[pixel];
	const bool bMotionValid = all(isfinite(mv)) && all(abs(mv) < 65000.0f);
	mv = bMotionValid ? mv : 0.0f;

	// Unjittered previous screen position of the surface point (pixels).
	const float2 prevScreen = float2(pixel) + 0.5f + g_Pass.Jitter + mv;
	// History texel q holds the surface seen at q + 0.5 + PrevJitter. Without jitter compensation
	// the history is read at the motion-vector position alone, i.e. as a jitter-averaged
	// (supersampled) signal, which avoids the bilinear blur of re-aligning every frame.
	const float2 historyOffset = DnFlag(DenoiserFlag_JitterCompensation) ? -g_Pass.PrevJitter : -g_Pass.Jitter;
	const float2 historyPos = prevScreen + historyOffset;

	// The motion vector only gives the previous screen position. Its previous depth is taken as
	// the previous-camera depth of the current point: exact for static geometry and for motion
	// parallel to the image plane, a small error for motion along the view direction.
	const float3 Xw = X + g_Pass.CameraDelta;                  // relative to the previous camera
	const float3 staticProjection = DnProjectPrev(Xw);         // (uv, viewZ) if the point were static
	const float prevViewZ = staticProjection.z;
	const float3 Xexpected = prevViewZ * DnPrevRay(prevScreen * g_Pass.InvSize);

	const bool bGuideHistory = DnFlag(DenoiserFlag_GuideHistoryValid) && bMotionValid && prevViewZ > 0.0f;
	const bool bDiffEnabled = DnFlag(DenoiserFlag_Diffuse);
	const bool bSpecEnabled = DnFlag(DenoiserFlag_Specular);
	const bool bDiffHistoryAllowed = bGuideHistory && bDiffEnabled && DnFlag(DenoiserFlag_DiffuseHistoryValid);
	const bool bSpecHistoryAllowed = bGuideHistory && bSpecEnabled && DnFlag(DenoiserFlag_SpecularHistoryValid);

	// Plane tolerance relative to depth; widened at grazing angles, where shading-normal detail
	// turns sub-pixel offsets along the surface into large plane distances.
	const float planeThreshold = g_Pass.DisocclusionThreshold * viewZ / max(NoV, 0.2f);
	const float lobeAngle = DnSpecularLobeHalfAngle(roughness);
	// History taps tolerate twice the a-trous lobe fraction: the history is an average anyway.
	const float specAngle = lobeAngle * max(2.0f * g_Pass.SpecularLobeFraction, 0.05f) + 0.01f;

	Texture2D<float4> prevDiffHistory = Tex2D(g_Pass.PrevDiffHistory);
	Texture2D<float4> prevSpecHistory = Tex2D(g_Pass.PrevSpecHistory);
	Texture2D<float4> prevFast = Tex2D(g_Pass.PrevFast);
	Texture2D<float4> prevMoments = Tex2D(g_Pass.PrevMoments);

	// --- Surface reprojection (diffuse + specular) ---------------------------------------------
	SHistory diffHistory = EmptyHistory();
	SHistory specSurface = EmptyHistory();
	float diffQuality = 0.0f;
	float specQuality = 0.0f;
	bool bAllTapsValid = false;
	float minSpecTapWeight = 0.0f;
	if (bDiffHistoryAllowed || bSpecHistoryAllowed)
	{
		int2 base;
		float4 bilinear;
		BilinearFootprint(historyPos, base, bilinear);
		float4 wDiff = 0.0f;
		float4 wSpec = 0.0f;
		[unroll]
		for (uint i = 0u; i < 4u; ++i)
		{
			const STapTest t = TestHistoryTap(base + kDnQuadOffsets[i], N, roughness, Xexpected, planeThreshold, specAngle, seenInMirror);
			wDiff[i] = t.bValid ? bilinear[i] : 0.0f;
			wSpec[i] = wDiff[i] * t.SpecWeight;
		}

		if (dot(wDiff, 1.0f) > 1e-4f)
		{
			// (A texel-aligned position gives zero-weight taps: plain bilinear is exact there.)
			bAllTapsValid = all(wDiff > 0.0f);
			minSpecTapWeight = 1.0f;
			[unroll]
			for (uint j = 0u; j < 4u; ++j)
			{
				const int2 q = base + kDnQuadOffsets[j];
				if (wDiff[j] > 0.0f)
				{
					const float4 m = prevMoments[q];
					const float4 f = prevFast[q];
					if (bDiffHistoryAllowed)
					{
						AddHistoryTap(diffHistory, wDiff[j], prevDiffHistory[q], f.xy, m.xy);
					}
					if (bSpecHistoryAllowed && wSpec[j] > 0.0f)
					{
						AddHistoryTap(specSurface, wSpec[j], prevSpecHistory[q], f.zw, m.zw);
					}
					minSpecTapWeight = min(minSpecTapWeight, wSpec[j] / max(wDiff[j], 1e-6f));
				}
			}
			diffQuality = diffHistory.Weight;
			specQuality = specSurface.Weight;
		}
		else
		{
			// Every bilinear tap failed: take the best matching texel of the 3x3 around the
			// reprojected position, at reduced confidence.
			const int2 centre = int2(floor(historyPos));
			float best = kFloatMax;
			int2 bestTexel = centre;
			float bestSpec = 0.0f;
			[unroll]
			for (int y = -1; y <= 1; ++y)
			{
				[unroll]
				for (int x = -1; x <= 1; ++x)
				{
					const int2 q = centre + int2(x, y);
					const STapTest t = TestHistoryTap(q, N, roughness, Xexpected, planeThreshold, specAngle, seenInMirror);
					if (t.bValid && t.PlaneDistance < best)
					{
						best = t.PlaneDistance;
						bestTexel = q;
						bestSpec = t.SpecWeight;
					}
				}
			}
			if (best < kFloatMax)
			{
				const float4 m = prevMoments[bestTexel];
				const float4 f = prevFast[bestTexel];
				if (bDiffHistoryAllowed)
				{
					AddHistoryTap(diffHistory, 1.0f, prevDiffHistory[bestTexel], f.xy, m.xy);
					diffQuality = 0.5f;
				}
				if (bSpecHistoryAllowed && bestSpec > 0.0f)
				{
					AddHistoryTap(specSurface, bestSpec, prevSpecHistory[bestTexel], f.zw, m.zw);
					specQuality = 0.5f * bestSpec;
				}
			}
		}
		diffHistory = FinishHistory(diffHistory);
		specSurface = FinishHistory(specSurface);

		// Bicubic slow history where the whole footprint is the same surface. The result is
		// clamped to the range of the 2x2 taps: Catmull-Rom's negative lobes would otherwise ring
		// and pull in the unvalidated outer ring of texels.
		// (A texel-aligned position - static camera - is exact already.)
		const float2 subTexel = frac(historyPos - 0.5f);
		const bool bAligned = all(min(subTexel, 1.0f - subTexel) < 1e-3f);
		if (DnFlag(DenoiserFlag_CatmullRom) && bAllTapsValid && !bAligned)
		{
			if (bDiffHistoryAllowed)
			{
				diffHistory.Slow = clamp(SampleCatmullRom(prevDiffHistory, historyPos), diffHistory.SlowMin, diffHistory.SlowMax);
			}
			if (bSpecHistoryAllowed && minSpecTapWeight > 0.5f && specSurface.Weight > 0.0f)
			{
				specSurface.Slow = clamp(SampleCatmullRom(prevSpecHistory, historyPos), specSurface.SlowMin, specSurface.SlowMax);
			}
		}
	}

	// --- Specular virtual motion -----------------------------------------------------------------
	SHistory specHistory = specSurface;
	float virtualAmount = 0.0f;
	const float D = DnSpecularDominantFactor(NoV, roughness);
	if (bSpecHistoryAllowed)
	{
		// Hit distance for tracking: this frame's if the lobe was sampled, else the history's.
		const float hitTrack = specHit > 0.0f ? specHit : specSurface.HitDistance;
		SHistory specVirtual = EmptyHistory();
		float virtualQuality = 0.0f;
		if (DnFlag(DenoiserFlag_VirtualMotion) && hitTrack > 0.0f && D > 0.01f)
		{
			const float3 Xvirtual = X - V * (hitTrack * D);
			const float3 pv = DnProjectPrev(Xvirtual + g_Pass.CameraDelta);
			if (pv.z > 0.0f)
			{
				// Moving reflectors: carry the object's own screen motion over to the image.
				const float2 objectMotion = prevScreen - staticProjection.xy * float2(g_Pass.Size);
				const float2 virtualPos = pv.xy * float2(g_Pass.Size) + objectMotion + historyOffset;
				if (all(abs(virtualPos - historyPos) < 0.05f) && specQuality > 0.0f)
				{
					// Same footprint as the surface reprojection (static view, or a reflection that
					// moves with its surface): reuse it instead of fetching it twice.
					specVirtual = specSurface;
					virtualQuality = specQuality;
				}
				else
				{
					int2 base;
					float4 bilinear;
					BilinearFootprint(virtualPos, base, bilinear);
					[unroll]
					for (uint i = 0u; i < 4u; ++i)
					{
						const int2 q = base + kDnQuadOffsets[i];
						// A planar reflector keeps the virtually reprojected taps on the current plane;
						// curved ones fail the plane test and fall back to surface motion.
						const STapTest t = TestHistoryTap(q, N, roughness, Xexpected, planeThreshold * 2.0f, specAngle, seenInMirror);
						// Each tap must also have seen the reflected scene at the tracked distance: a
						// footprint straddling an edge of the reflected image (a column in front of a
						// far wall) would otherwise blend both sides and streak along the motion.
						const float4 fast = prevFast[q];
						const float hitWeight = fast.w > 0.0f ?
							saturate(1.0f - abs(fast.w - hitTrack) / (0.25f * max(fast.w, hitTrack) + 0.01f * viewZ)) : 1.0f;
						const float w = bilinear[i] * t.SpecWeight * hitWeight;
						if (w > 0.0f)
						{
							AddHistoryTap(specVirtual, w, prevSpecHistory[q], fast.zw, prevMoments[q].zw);
						}
					}
					virtualQuality = specVirtual.Weight;
					specVirtual = FinishHistory(specVirtual);
				}
			}
		}
		if (virtualQuality > 0.0f)
		{
			// The reflected image must be at the distance we tracked (NRD: hit distance confidence).
			float hitConfidence = 1.0f;
			if (specHit > 0.0f && specVirtual.HitDistance > 0.0f)
			{
				const float delta = abs(specVirtual.HitDistance - specHit);
				hitConfidence = saturate(1.0f - delta / (0.25f * max(specVirtual.HitDistance, specHit) + 0.01f * viewZ));
			}
			// Footprint quality and hit confidence decide whether the virtual history is usable, not
			// how much of the surface history to mix in: the virtual history is already renormalised
			// over its valid taps, while the surface history of a mirror-like lobe is the reflection
			// seen from the wrong place. The former soft confidence (sqrt(quality) * confidence: median
			// 0.96 on the demo's mirror under camera translation, from hit distances that differ by a
			// few percent between neighbouring texels) mixed ~4 % of that into the history every frame.
			// Both are therefore full trust above one half. Measured after CameraMove 0.01 m/frame for
			// 100 frames against a reference at the same pose: mirror region mean |log2| error of
			// 9x9-blurred luminance 0.194 -> 0.147 (a static camera at that pose gives 0.114), image
			// relMSE 0.156 -> 0.098.
			virtualAmount = specQuality > 0.0f ? D * saturate(2.0f * virtualQuality) * saturate(2.0f * hitConfidence) : 1.0f;
			specHistory = LerpHistory(specSurface, specVirtual, virtualAmount);
			specQuality = lerp(specQuality, virtualQuality, virtualAmount);
		}
	}

	// Parallax cap for the surface-reprojected part (|V - Vprev| ~ view angle change per frame).
	float specLengthCap = g_Pass.MaxHistoryLength;
	if (bSpecHistoryAllowed && specQuality > 0.0f)
	{
		// The chord |V - Vprev| keeps full precision for the tiny angles of slow camera motion.
		const float3 Vprev = -normalize(Xexpected);
		const float parallax = length(V - Vprev) * g_Pass.SpecularParallaxScale;
		// A small slack keeps glossy lobes from collapsing to one frame on sub-pixel motion. It fades
		// out towards mirrors (dominant factor D -> 1), whose reflected image does not move with the
		// surface at all: where their virtual history failed, a few frames of surface history are
		// a few frames of smear (measured as streaks along the motion in the demo's mirror). No view
		// change at all (static camera) never caps the history.
		const float slack = 0.01f * (1.0f - D) + 0.001f;
		const float surfaceCap = parallax > 1e-7f ?
			clamp((lobeAngle + slack) / parallax, 1.0f, g_Pass.MaxHistoryLength) : g_Pass.MaxHistoryLength;
		specLengthCap = lerp(surfaceCap, g_Pass.MaxHistoryLength, virtualAmount);
	}

	// --- Accumulate ------------------------------------------------------------------------------
	const bool bDiffHistory = bDiffHistoryAllowed && diffQuality > 0.0f;
	const bool bSpecHistory = bSpecHistoryAllowed && specQuality > 0.0f;
	float2 fireflyReference = 0.0f;
	if (DnFlag(DenoiserFlag_Firefly) && (bDiffHistory || bSpecHistory))
	{
		const float2 uv = historyPos * g_Pass.InvSize;
		fireflyReference.x = Luminance(Tex2D(g_Pass.PrevDiffOutput).SampleLevel(s_LinearClamp, uv, 0.0f).rgb);
		fireflyReference.y = Luminance(Tex2D(g_Pass.PrevSpecOutput).SampleLevel(s_LinearClamp, uv, 0.0f).rgb);
	}
	o.D = Accumulate(diffuse, diffHit, diffHistory, bDiffHistory, diffQuality, g_Pass.MaxHistoryLength, fireflyReference.x);
	o.S = Accumulate(specular, specHit, specHistory, bSpecHistory, specQuality, specLengthCap, fireflyReference.y);
	return o;
}

// --- Anti-lag tile sums --------------------------------------------------------------------------------
// Per 8x8 group: sums of the slow history luminance (this frame's accumulation), the fast history
// luminance (raw samples of the last MaxFastHistoryLength frames, per pixel), the variance of the
// fast mean (per-sample variance from the moments / fast length) and the pixel count. A-trous
// iteration 0 pools 5x5 tiles of them for the anti-lag test (DnAntilagAmount).
#define GROUP_THREADS 64u

groupshared float4 gs_AntilagD[8];  // per wave
groupshared float4 gs_AntilagS[8];
groupshared uint gs_YoungTile;

void GroupSums (float4 D, float4 S, uint GroupIndex, out float4 SumD, out float4 SumS)
{
	const float4 waveD = WaveActiveSum(D);
	const float4 waveS = WaveActiveSum(S);
	const uint lanes = WaveGetLaneCount();
	const uint waveCount = min((GROUP_THREADS + lanes - 1u) / lanes, 8u);
	if (WaveIsFirstLane())
	{
		gs_AntilagD[GroupIndex / lanes] = waveD;
		gs_AntilagS[GroupIndex / lanes] = waveS;
	}
	GroupMemoryBarrierWithGroupSync();
	SumD = 0.0f;
	SumS = 0.0f;
	for (uint i = 0u; i < waveCount; ++i)
	{
		SumD += gs_AntilagD[i];
		SumS += gs_AntilagS[i];
	}
}

float4 AntilagTap (SAccumulated A)
{
	const float fastLength = max(min(A.Length, g_Pass.MaxFastHistoryLength), 1.0f);
	return float4(Luminance(A.Slow), A.Fast, DnMomentsVariance(A.Moments) / fastLength, 1.0f);
}

// --- Noise estimate -------------------------------------------------------------------------------------
// Standard deviation of the accumulated mean, sqrt(var / length), from the luminance moments. The
// moments are filtered by the same a-trous iteration that produces the feedback history, so they
// are spatio-temporal statistics of the neighbourhood and do not collapse to zero for a pixel that
// has not seen a non-zero sample yet. A floor of LuminanceFloor x the mean luminance (folded into
// the deviation) keeps the a-trous luminance weights from ever separating samples that differ by
// less than that. Young histories (inside the history-fix window) get the deviation inflated by
// 1 / maturity: their moments describe a handful of samples, so they are filtered by geometry
// alone (as in ReBLUR).
float AccumulatedDeviation (float2 Moments, float Length, float PhiLuminance)
{
	const float variance = DnMomentsVariance(Moments) / max(Length, 1.0f);
	const float floorDeviation = g_Pass.LuminanceFloor * max(Moments.x, 0.0f) / max(PhiLuminance, 1e-3f);
	const float maturity = DnHistoryMaturity(Length);
	return maturity > 0.0f ? min(sqrt(variance + floorDeviation * floorDeviation) / maturity, kDnMaxRadiance) : kDnMaxRadiance;
}

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID, uint GroupIndex : SV_GroupIndex)
{
	const bool bInside = all(Id.xy < g_Pass.Size);
	const int2 pixel = int2(min(Id.xy, g_Pass.Size - 1u));
	STemporalPixel t = TemporalPixel(pixel);
	const bool bDiff = DnFlag(DenoiserFlag_Diffuse);
	const bool bSpec = DnFlag(DenoiserFlag_Specular);

	// Every thread takes part in the reduction (the flag is uniform).
	if (DnFlag(DenoiserFlag_AntiLag))
	{
		const bool bUse = bInside && !t.bSky;
		float4 sumD, sumS;
		GroupSums(bUse && bDiff ? AntilagTap(t.D) : 0.0f, bUse && bSpec ? AntilagTap(t.S) : 0.0f, GroupIndex, sumD, sumS);
		if (GroupIndex == 0u)
		{
			RWTexture2D<float4> tilesD = ResourceDescriptorHeap[g_Pass.AntilagTilesDUav];
			RWTexture2D<float4> tilesS = ResourceDescriptorHeap[g_Pass.AntilagTilesSUav];
			tilesD[Id.xy / 8u] = sumD;
			tilesS[Id.xy / 8u] = sumS;
		}
	}
	// Tile flag for the history-fix pass, which then skips tiles without young histories.
	if (GroupIndex == 0u)
	{
		gs_YoungTile = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	const bool bFix = DnFlag(DenoiserFlag_HistoryFix) && g_Pass.HistoryFixFrames > 1.0f;
	const bool bYoung = bInside && !t.bSky && bFix &&
		((bDiff && t.D.Length < g_Pass.HistoryFixFrames) || (bSpec && t.S.Length < g_Pass.HistoryFixFrames));
	if (WaveActiveAnyTrue(bYoung) && WaveIsFirstLane())
	{
		InterlockedOr(gs_YoungTile, 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	if (GroupIndex == 0u)
	{
		RWTexture2D<uint> youngTiles = ResourceDescriptorHeap[g_Pass.YoungTilesUav];
		youngTiles[Id.xy / 8u] = gs_YoungTile;
	}

	if (!bInside)
	{
		return;
	}

	DnRwTex2DUint2(g_Pass.OutGuide)[pixel] = DnPackGuide(t.N, t.Roughness, t.ViewZ);
	if (t.bSky)
	{
		RwTex2D(g_Pass.OutFast)[pixel] = 0.0f;
		RwTex2D(g_Pass.OutMoments)[pixel] = 0.0f;
		RwTex2D(g_Pass.AccumDiffUav)[pixel] = 0.0f;
		RwTex2D(g_Pass.AccumSpecUav)[pixel] = 0.0f;
		RwTex2D(g_Pass.VarianceUav)[pixel] = 0.0f;
		if (DnFlag(DenoiserFlag_HistoryLengthDebug))
		{
			RwTex2DFloat(g_Pass.HistoryLengthUav)[pixel] = 0.0f;
		}
		return;
	}
	const SAccumulated d = t.D;
	const SAccumulated s = t.S;
	// Everything that is carried to the next frame is rounded without bias (DnRoundToHalf); the
	// slow histories reach the history textures through a-trous iteration 0 unchanged.
	RwTex2D(g_Pass.OutFast)[pixel] = DnRoundToHalf4(float4(d.Fast, d.HitDistance, s.Fast, s.HitDistance), DnRoundingSeed(pixel, 0u));
	RwTex2D(g_Pass.OutMoments)[pixel] = DnRoundToHalf4(float4(DnEncodeMoments(d.Moments), DnEncodeMoments(s.Moments)), DnRoundingSeed(pixel, 1u));
	RwTex2D(g_Pass.AccumDiffUav)[pixel] = DnRoundColorToHalf(d.Slow, d.Length, DnRoundingSeed(pixel, 2u));
	RwTex2D(g_Pass.AccumSpecUav)[pixel] = DnRoundColorToHalf(s.Slow, s.Length, DnRoundingSeed(pixel, 3u));

	// Noise texture: deviations, specular filter extent and hit distance for the a-trous pass.
	SDnGuide g;
	g.N = t.N;
	g.Roughness = t.Roughness;
	g.ViewZ = t.ViewZ;
	const float deviationD = bDiff ? AccumulatedDeviation(d.Moments, d.Length, g_Pass.PhiLuminanceDiffuse) : 0.0f;
	const float deviationS = bSpec ? AccumulatedDeviation(s.Moments, s.Length, g_Pass.PhiLuminanceSpecular) : 0.0f;
	const float footprint = bSpec ? DnSpecularExtent(g, s.HitDistance, s.Length, s.Moments) : 0.0f;
	RwTex2D(g_Pass.VarianceUav)[pixel] = float4(deviationD, deviationS, footprint, s.HitDistance);
	if (DnFlag(DenoiserFlag_HistoryLengthDebug))
	{
		const float debugLength = g_Pass.HistoryDebugSignal == 0u ? d.Length : s.Length;
		RwTex2DFloat(g_Pass.HistoryLengthUav)[pixel] = saturate(debugLength / max(g_Pass.MaxHistoryLength, 1.0f));
	}
}
