#ifndef FRT_DENOISER_COMMON_HLSLI
#define FRT_DENOISER_COMMON_HLSLI

// Shared helpers of the CDenoiser passes (Temporal, HistoryFix, Atrous, Passthrough).
//
// Conventions:
//  - positions are camera relative: X = viewZ * Ray(uv), with Ray() scaled to unit view depth,
//    so plane distances stay precise far from the world origin;
//  - pixel p was sampled at p + 0.5 + Jitter (the path tracer's primary ray position);
//  - "length" is the per-pixel history length in frames, accumulation weight = 1 / length;
//  - all signals are demodulated radiance; luminance statistics use Rec.709 weights.

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Packing.hlsli"
#include "Shared/DenoiserData.h"

ConstantBuffer<SDenoiserConstants> g_Pass : register(b2);

// Largest radiance kept in RGBA16F targets (half max is 65504; keep a margin so filtering can
// never round up to +Inf).
static const float kDnMaxRadiance = 65000.0f;
static const float kDnMaxHitDistance = 65000.0f;
// Written into the guide for pixels without a surface.
static const float kDnSkyMarker = 1e6f;

static const int2 kDnQuadOffsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

bool DnFlag (uint Flag) { return (g_Pass.Flags & Flag) != 0u; }

Texture2D<uint2> DnTex2DUint2 (uint Index) { return ResourceDescriptorHeap[Index]; }
RWTexture2D<uint2> DnRwTex2DUint2 (uint Index) { return ResourceDescriptorHeap[Index]; }

bool DnInScreen (int2 Texel)
{
	return all(Texel >= 0) && all(Texel < int2(g_Pass.Size));
}

// ---------------------------------------------------------------------------------------------
// Sanitisation
// ---------------------------------------------------------------------------------------------

// NaN / Inf / negative -> 0, then clamp to the half range. +Inf becomes 0 rather than the
// maximum: a 65000 firefly would be accumulated and smeared by every following pass.
float3 DnSanitizeRadiance (float3 C)
{
	return all(isfinite(C)) ? clamp(C, 0.0f, kDnMaxRadiance) : 0.0f;
}

float DnSanitizeHitDistance (float H)
{
	return isfinite(H) ? clamp(H, 0.0f, kDnMaxHitDistance) : 0.0f;
}

// Pass-through (sky, disabled signal): finite values stay bit-exact, NaN / Inf become 0.
float4 DnPassThrough (float4 V)
{
	return float4(all(isfinite(V.rgb)) ? V.rgb : 0.0f, isfinite(V.a) ? V.a : 0.0f);
}

bool DnIsSky (float ViewZ)
{
	// Written so NaN lands on the sky side.
	return !(ViewZ < DenoiserSkyViewZ) || !(ViewZ > 0.0f);
}

// ---------------------------------------------------------------------------------------------
// Guide: normal + roughness + view depth in one 8-byte texel (one fetch per filter tap).
// ---------------------------------------------------------------------------------------------

struct SDnGuide
{
	float3 N;
	float Roughness;
	float ViewZ;
};

// 12:12 octahedral normal (~0.03 deg max error, well below the tightest lobe tolerance of the
// specular weights), 8-bit linear roughness, fp32 view depth.
uint2 DnPackGuide (float3 N, float Roughness, float ViewZ)
{
	const float2 e = saturate(EncodeOctahedral(N) * 0.5f + 0.5f);
	const uint2 q = uint2(round(e * 4095.0f));
	const uint r = uint(round(saturate(Roughness) * 255.0f));
	return uint2(q.x | (q.y << 12u) | (r << 24u), asuint(ViewZ));
}

SDnGuide DnUnpackGuide (uint2 P)
{
	SDnGuide g;
	const float2 e = float2(float(P.x & 0xFFFu), float((P.x >> 12u) & 0xFFFu)) * (2.0f / 4095.0f) - 1.0f;
	g.N = DecodeOctahedral(e);
	g.Roughness = float(P.x >> 24u) * (1.0f / 255.0f);
	g.ViewZ = asfloat(P.y);
	return g;
}

float DnGuideViewZ (uint2 P)
{
	return asfloat(P.y);
}

// ---------------------------------------------------------------------------------------------
// Camera reconstruction
// ---------------------------------------------------------------------------------------------

float3 DnCurRay (float2 Uv)
{
	return g_Pass.CurRayC + Uv.x * g_Pass.CurRayX + Uv.y * g_Pass.CurRayY;
}

float3 DnPrevRay (float2 Uv)
{
	return g_Pass.PrevRayC + Uv.x * g_Pass.PrevRayX + Uv.y * g_Pass.PrevRayY;
}

// Position (relative to the current camera) of the surface seen by texel Pixel this frame.
float3 DnCurPosition (int2 Pixel, float ViewZ)
{
	return ViewZ * DnCurRay((float2(Pixel) + 0.5f + g_Pass.Jitter) * g_Pass.InvSize);
}

// Position (relative to the previous camera) of the surface seen by history texel Pixel.
float3 DnPrevPosition (int2 Pixel, float ViewZ)
{
	return ViewZ * DnPrevRay((float2(Pixel) + 0.5f + g_Pass.PrevJitter) * g_Pass.InvSize);
}

// Previous-camera-relative position -> (previous screen uv, previous view depth).
float3 DnProjectPrev (float3 P)
{
	const float3 q = float3(dot(g_Pass.PrevProj0, P), dot(g_Pass.PrevProj1, P), dot(g_Pass.PrevProj2, P));
	return float3(q.xy / (abs(q.z) > 1e-20f ? q.z : 1e-20f), q.z);
}

// ---------------------------------------------------------------------------------------------
// Specular lobe helpers
// ---------------------------------------------------------------------------------------------

// acos for x in [0, 1]. Exact at both ends (important for the sub-degree tolerances of glossy
// lobes), ~0.5 deg max error in between (Lagarde 2014, "Inverse trigonometric functions GPU
// optimization for AMD GCN architecture").
float DnAcosPositive (float X)
{
	const float a = saturate(X);
	return (kHalfPi - 0.156583f * a) * sqrt(1.0f - a);
}

// Fraction of the way the GGX dominant direction moves from N to the mirror direction
// (fit from NRD, "GetSpecularDominantFactor", G2 mode; linear roughness).
float DnSpecularDominantFactor (float NoV, float LinearRoughness)
{
	const float r = saturate(LinearRoughness);
	const float a = 0.298475f * log(39.4115f - 39.0029f * r);
	return saturate(pow(saturate(1.0f - NoV), 10.8649f) * (1.0f - a) + a);
}

// Half-angle of the reflected GGX lobe containing ~75% of its energy. For the GGX slope
// distribution P(tan(theta_h) < t) = t^2 / (alpha^2 + t^2), so the 75% half-vector cone has
// tan(theta_h) = alpha * sqrt(3); reflection doubles the angle (alpha = roughness^2).
float DnSpecularLobeHalfAngle (float LinearRoughness)
{
	const float alpha = Sq(saturate(LinearRoughness));
	return 2.0f * atan(alpha * 1.7320508f);
}

// Same, with a rational atan (|error| < 0.01 rad) for per-tap use.
float DnSpecularLobeHalfAngleFast (float LinearRoughness)
{
	const float x = Sq(saturate(LinearRoughness)) * 1.7320508f;
	const float atanX = x < 1.0f ? x / (1.0f + 0.28f * x * x) : kHalfPi - x / (x * x + 0.28f);
	return 2.0f * atanX;
}

// Screen-space radius (pixels) of the blur a glossy reflection actually has: the lobe cone
// opened over the hit distance, seen from the camera over viewZ + hitT (the distance to the
// reflected image). Hit distance 0 = unknown -> treat the reflected scene as distant.
float DnSpecularFootprintPixels (float LinearRoughness, float HitDistance, float ViewZ)
{
	const float tanLobe = tan(min(DnSpecularLobeHalfAngle(LinearRoughness), 1.4f));
	const float angular = HitDistance > 0.0f ? tanLobe * HitDistance / (ViewZ + HitDistance) : tanLobe;
	return angular / max(g_Pass.PixelAngle, 1e-6f);
}

// Linear falloff in angle (ReLAX-style lobe test): 1 for identical normals, 0 at MaxAngle.
float DnLobeNormalWeight (float CosAngle, float MaxAngle)
{
	return saturate(1.0f - DnAcosPositive(CosAngle) / max(MaxAngle, 1e-4f));
}

// Roughness similarity, tolerance grows with roughness (rough lobes tolerate more variation).
float DnRoughnessWeight (float Roughness, float Other)
{
	const float tolerance = 0.02f + g_Pass.RoughnessFraction * Roughness;
	return saturate(1.0f - abs(Roughness - Other) / tolerance);
}

// ---------------------------------------------------------------------------------------------
// Moments: stored as (m1, sqrt(m2)) so the second moment cannot overflow half precision
// (sqrt(m2) has the range of the radiance itself).
// ---------------------------------------------------------------------------------------------

float2 DnDecodeMoments (float2 Stored)
{
	return float2(Stored.x, Stored.y * Stored.y);
}

float2 DnEncodeMoments (float2 Moments)
{
	return float2(Moments.x, sqrt(max(Moments.y, 0.0f)));
}

float DnMomentsVariance (float2 Moments)
{
	return max(Moments.y - Moments.x * Moments.x, 0.0f);
}

// 0 for a fresh history, 1 once it is older than the history-fix window.
float DnHistoryMaturity (float Length)
{
	return saturate((Length - 1.0f) / max(g_Pass.HistoryFixFrames - 1.0f, 1.0f));
}

// Spatial extent (pixels) the specular filter may use around a pixel: the screen footprint of the
// reflection's own blur, extended by the radius the noise needs. A glossy or mirror reflection
// of a sparse path-traced scene is as noisy as the scene itself, and a lobe-sized kernel of a
// few pixels leaves it a field of sparkles; ReBLUR likewise widens its specular blur with the
// noise (history length). The noise radius is the one whose area holds enough samples to bring
// the relative deviation of the accumulated mean (relVar = var / (length m1^2), from the moments)
// down to SpecularNoiseTarget; the hit-distance weight keeps the reflected image's geometric
// edges while it is wider than the lobe.
float DnSpecularExtent (SDnGuide G, float SpecHit, float LengthS, float2 Moments)
{
	const float maturity = DnHistoryMaturity(LengthS);
	const float lobe = DnSpecularFootprintPixels(G.Roughness, SpecHit, G.ViewZ) * g_Pass.SpecularFootprintScale;
	const float relativeVariance = Moments.x > 0.0f ? DnMomentsVariance(Moments) / (max(LengthS, 1.0f) * Moments.x * Moments.x) : kFloatMax;
	const float noiseRadius = min(sqrt(relativeVariance / (kPi * Sq(max(g_Pass.SpecularNoiseTarget, 1e-3f)))), g_Pass.SpecularNoiseRadiusMax);
	return max(max(lobe * lerp(2.0f, 1.0f, maturity), noiseRadius), 0.25f * g_Pass.HistoryFixRadius * (1.0f - maturity));
}

// ---------------------------------------------------------------------------------------------
// Hashing (per-pixel rotation of sparse kernels)
// ---------------------------------------------------------------------------------------------

uint DnHash (uint V)
{
	// PCG hash (Jarzynski & Olano 2020).
	const uint state = V * 747796405u + 2891336453u;
	const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float DnHashToUnit (uint2 Pixel, uint Frame)
{
	const uint h = DnHash(Pixel.x * 0x8da6b343u ^ Pixel.y * 0xd8163841u ^ DnHash(Frame));
	return float(h >> 8u) * (1.0f / 16777216.0f);
}

// ---------------------------------------------------------------------------------------------
// Unbiased half-precision stores
// ---------------------------------------------------------------------------------------------
// Typed UAV stores to float16 formats lose ~0.035 % (half an ulp) per store on average on the
// development GPU (RTX 4080), i.e. they truncate. That is nothing once, but a recursive history
// loses it every frame and its running mean settles at x / (1 + length * 0.00035). Measured with
// temporal accumulation only (pose 0,1.6,0 / yaw -1.57, 16 spp, 300 static frames; a single
// 64-spp frame gives 0.955 of the reference): MaxHistoryLength 1 -> 0.955, 16 -> 0.948,
// 64 -> 0.931, 128 -> 0.913, 255 -> 0.900, a uniform loss over the whole image. With the rounding
// below: 64 -> 0.953, 255 -> 0.951.
// Values that are carried to the next frame are therefore rounded to a neighbouring half value
// stochastically (probability proportional to the distance, i.e. exact in expectation) before they
// are stored; the hardware conversion of an exactly representable value is exact in any rounding
// mode.
// Random supplies 13 uniform bits (the fp32 mantissa bits a half drops).
float DnRoundToHalf (float X, uint Random)
{
	const uint bits = Random & 0x1FFFu;
	if (abs(X) >= 6.103515625e-05f)
	{
		// Normal half range (2^-14 and up): a random carry into the 13 dropped mantissa bits.
		return asfloat((asuint(X) + bits) & ~0x1FFFu);
	}
	// Half subnormals are multiples of 2^-24.
	return floor(X * 16777216.0f + float(bits) * (1.0f / 8192.0f)) * (1.0f / 16777216.0f);
}

// Random bits for the stores of one pixel; Stream separates the textures written by one thread.
uint DnRoundingSeed (int2 Pixel, uint Stream)
{
	return DnHash(uint(Pixel.x) * 0x8da6b343u ^ uint(Pixel.y) * 0xd8163841u ^ DnHash(g_Pass.FrameIndex * 16u + Stream));
}

float4 DnRoundToHalf4 (float4 X, uint Seed)
{
	const uint h1 = DnHash(Seed);
	return float4(DnRoundToHalf(X.x, Seed), DnRoundToHalf(X.y, Seed >> 13u), DnRoundToHalf(X.z, h1), DnRoundToHalf(X.w, h1 >> 13u));
}

// Colour + an exact value (history length, hit distance, deviation): only rgb is rounded.
float4 DnRoundColorToHalf (float3 C, float A, uint Seed)
{
	const uint h1 = DnHash(Seed);
	return float4(DnRoundToHalf(C.r, Seed), DnRoundToHalf(C.g, Seed >> 13u), DnRoundToHalf(C.b, h1), A);
}

#endif // FRT_DENOISER_COMMON_HLSLI
