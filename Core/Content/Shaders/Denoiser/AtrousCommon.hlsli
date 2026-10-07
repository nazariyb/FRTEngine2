#ifndef FRT_DENOISER_ATROUS_COMMON_HLSLI
#define FRT_DENOISER_ATROUS_COMMON_HLSLI

// Edge-stopping a-trous tap weights of the CDenoiser spatial filter (Atrous.hlsl), kept separate
// so a groupshared-memory variant of an iteration can reuse exactly the same math.
//
// Edge-avoiding a-trous wavelet transform (Dammertz et al. 2010) with SVGF's deviation-guided
// luminance edge stopping (Schied et al. 2017): a 3x3 B-spline kernel [1/4 1/2 1/4]^2 with taps
// spaced by StepSize = 2^iteration, so five iterations cover a 63x63 footprint with 45 taps.
//
// Energy conservation. The usual normalised form sum(w x) / sum(w) is not energy conserving once
// the weights vary: an isolated bright pixel in a dark neighbourhood keeps most of its value
// (few accepted taps) while its dark neighbours dilute what they take from it (many accepted
// taps), so energy is lost every pass - and with the first iteration fed back as history, every
// frame (measured on the demo scene: 64x64 block means at 0.86x the reference with luminance
// weights, 0.41x with SVGF's centre-only sigma). Here every tap weight k(p, q) = h(p, q) w(p, q)
// is symmetric in the pair and the remainder of the kernel stays with the centre:
//     out(p) = x(p) + sum_q k(p, q) (x(q) - x(p)),
// i.e. an explicit anisotropic-diffusion step: whatever p takes from q, q gives to p, so the image
// sum is preserved exactly and edge stopping only decides where energy may flow.
//
// Symmetric per-pair weights:
//   geometry  : the larger of the two tangent-plane distances relative to the mean view depth
//               (PhiDepth), linear falloff (ReLAX geometry weight);
//   normal    : diffuse pow(dot(n, n'), PhiNormal) (SVGF); specular a linear falloff in angle up
//               to SpecularLobeFraction x the lobe half-angle of the rougher of the two (ReLAX
//               lobe weight), plus a roughness similarity weight;
//   luminance : exp(-|l - l'| / (PhiL * sqrt(sigma^2 + sigma'^2))), sigma = standard deviation of
//               each pixel's own accumulated value (incl. the LuminanceFloor term and the
//               young-history inflation, see Temporal.hlsl);
//   footprint : specular taps further away than the pair's larger specular extent (reflection
//               blur footprint, widened by the noise radius - DnSpecularExtent) are rejected: a
//               rare bright sample next to pixels that have seen nothing must be able to spread
//               (demo scene, static: relMSE 0.131 with the smaller extent, 0.120 with the larger;
//               it is symmetric either way). Inside it, taps
//               must see the reflected scene at a similar distance (hit-distance weight), so
//               reflections keep their geometric edges.
// The deviation is propagated as sqrt((1 - sum k)^2 sigma^2 + sum(k^2 sigma'^2)) so later
// iterations stop on the residual noise of the already filtered signal.

#include "Denoiser/DenoiserCommon.hlsli"

static const float kAtrousKernel1D[2] = { 0.5f, 0.25f };
static const float kAtrousLobeSlack = 0.01f;     // radians (~0.6 deg) of normal tolerance on any lobe
static const float kAtrousHitRoughness = 0.4f;   // the hit-distance weight fades out up to this roughness

struct SAtrousCentre
{
	float3 X;
	float3 N;
	float ViewZ;
	float Roughness;
	float LumD;
	float LumS;
	float VarD;           // sigma^2 of the centre (luminance)
	float VarS;
	float Footprint;      // specular footprint (pixels)
	float SpecHit;        // specular hit distance (0 = unknown)
	bool bDiff;
	bool bSpecFilter;
};

struct SAtrousSums
{
	float3 SumD;          // sum(k x)
	float WeightD;        // sum(k)
	float VarD;           // sum(k^2 sigma^2)
	float3 SumS;
	float WeightS;
	float VarS;
};

// Similarity of the reflected points (hit distance 0 = lobe not sampled, no information). Rough
// lobes gather from everywhere, so their hit distances carry no edge.
float AtrousHitDistanceWeight (float HitC, float HitQ, float Roughness)
{
	if (!(HitC > 0.0f) || !(HitQ > 0.0f))
	{
		return 1.0f;
	}
	const float w = saturate(1.0f - abs(HitC - HitQ) / (g_Pass.SpecularHitTolerance * min(HitC, HitQ) + 1e-4f));
	return lerp(w, 1.0f, saturate(Roughness / kAtrousHitRoughness));
}

float AtrousFootprintWeight (float Footprint, float StepSize)
{
	return saturate(2.0f - StepSize / max(Footprint, 1e-3f));
}

SAtrousCentre MakeAtrousCentre (int2 Pixel, SDnGuide G, float2 Deviation, float3 ColourD, float3 ColourS, float Footprint, float SpecHit,
	float StepSize)
{
	SAtrousCentre c;
	c.X = DnCurPosition(Pixel, G.ViewZ);
	c.N = G.N;
	c.ViewZ = G.ViewZ;
	c.Roughness = G.Roughness;
	c.LumD = Luminance(ColourD);
	c.LumS = Luminance(ColourS);
	c.VarD = Sq(Deviation.x);
	c.VarS = Sq(Deviation.y);
	c.Footprint = Footprint;
	c.SpecHit = SpecHit;
	c.bDiff = DnFlag(DenoiserFlag_Diffuse);
	c.bSpecFilter = DnFlag(DenoiserFlag_Specular);
	return c;
}

SAtrousSums BeginAtrousSums ()
{
	SAtrousSums s;
	s.SumD = 0.0f;
	s.WeightD = 0.0f;
	s.VarD = 0.0f;
	s.SumS = 0.0f;
	s.WeightS = 0.0f;
	s.VarS = 0.0f;
	return s;
}

// Geometry part of a tap (shared by both signals): x = symmetric plane-distance weight, y = normal
// cosine.
float2 AtrousTapGeometry (SAtrousCentre C, int2 Tap, SDnGuide G)
{
	const float3 d = DnCurPosition(Tap, G.ViewZ) - C.X;
	const float planeDistance = max(abs(dot(C.N, d)), abs(dot(G.N, d)));
	const float tolerance = g_Pass.PhiDepth * 0.5f * (C.ViewZ + G.ViewZ);
	// A surface seen in a PSR mirror and a real one never exchange energy, even where the reflected
	// plane coincides with the real one (floor and ceiling perpendicular to the mirror).
	const float sameSide = ViewZSeenInMirror(C.ViewZ) == ViewZSeenInMirror(G.ViewZ) ? 1.0f : 0.0f;
	return float2(sameSide * saturate(1.0f - planeDistance / max(tolerance, 1e-6f)), dot(C.N, G.N));
}

// Symmetric luminance weight of a pair (see the header).
float AtrousLuminanceWeight (float LumC, float VarC, float LumQ, float VarQ, float Phi)
{
	const float tolerance = Phi * sqrt(VarC + VarQ) + 1e-4f * max(LumC, LumQ) + 1e-12f;
	return exp(-abs(LumC - LumQ) / tolerance);
}

// Returns the tap weight k (0 = rejected).
float AddAtrousDiffuse (inout SAtrousSums S, SAtrousCentre C, float H, float GeometryWeight, float CosN, float3 Colour, float Deviation)
{
	const float wn = g_Pass.PhiNormal > 0.0f ? pow(saturate(CosN), g_Pass.PhiNormal) : 1.0f;
	const float var = Deviation * Deviation;
	const float wl = AtrousLuminanceWeight(C.LumD, C.VarD, Luminance(Colour), var, g_Pass.PhiLuminanceDiffuse);
	const float k = H * GeometryWeight * wn * wl;
	S.SumD += k * Colour;
	S.WeightD += k;
	S.VarD += k * k * var;
	return k;
}

float AddAtrousSpecular (inout SAtrousSums S, SAtrousCentre C, float H, float GeometryWeight, float CosN, float TapRoughness,
	float TapHit, float3 Colour, float Deviation, float FootprintWeight)
{
	const float roughness = max(C.Roughness, TapRoughness);
	const float lobe = DnSpecularLobeHalfAngleFast(roughness) * g_Pass.SpecularLobeFraction + kAtrousLobeSlack;
	const float tolerance = 0.02f + g_Pass.RoughnessFraction * roughness;
	const float wr = saturate(1.0f - abs(C.Roughness - TapRoughness) / tolerance);
	const float wn = DnLobeNormalWeight(CosN, lobe) * wr * AtrousHitDistanceWeight(C.SpecHit, TapHit, roughness);
	const float var = Deviation * Deviation;
	const float wl = AtrousLuminanceWeight(C.LumS, C.VarS, Luminance(Colour), var, g_Pass.PhiLuminanceSpecular);
	const float k = H * GeometryWeight * wn * wl * FootprintWeight;
	S.SumS += k * Colour;
	S.WeightS += k;
	S.VarS += k * k * var;
	return k;
}

#endif // FRT_DENOISER_ATROUS_COMMON_HLSLI
