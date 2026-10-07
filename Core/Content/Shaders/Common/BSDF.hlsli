#ifndef FRT_BSDF_HLSLI
#define FRT_BSDF_HLSLI

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Sampling.hlsli"

// glTF metallic-roughness BSDF:
//   specular: GGX microfacet, height-correlated Smith G2, Schlick Fresnel, multiple-scattering
//             energy compensation (Turquin 2019; E(mu, alpha) from a LUT generated at startup);
//   diffuse:  Lambert scaled by (1 - E_dielectric) so the layer conserves energy. The glTF model
//             is mix(dielectric, metal, metallic); only the dielectric part has a diffuse base, so
//             it is attenuated by the DIELECTRIC specular albedo, not by the mixed one (that would
//             lose energy on every partially metallic texel).
// Sampling: one-sample mixture of the specular lobe (bounded VNDF, Eto & Tokuyoshi 2023) and
// a cosine-weighted diffuse lobe; the lobe probability follows the directional albedos.
// All directions are in the local frame (N = +Z), V and L point away from the surface.

static const float kMinAlpha = 0.002f;     // below this the lobe is treated as a perfect mirror
static const float kDeltaLobeAlpha = 0.0f;

struct SBsdfParams
{
	float3 DiffuseAlbedo;     // (1 - metallic) * baseColor
	float3 F0;                // specular reflectance at normal incidence (dielectric/metal mix)
	float DielectricF0;       // F0 of the dielectric part alone (0.04 * specular factor)
	float Alpha;              // GGX alpha = roughness^2 (already clamped/regularised)
	float3 EnergyScale;       // 1 + Favg * (1 - E) / E per channel (multi-scatter compensation)
	float SpecularAlbedoE;    // E(mu_o, alpha): single-scatter directional albedo with F = 1
	float SpecularAlbedoB;    // Schlick split-sum bias: Fresnel-weighted albedo = F0 * (E - B) + B
	float DielectricSpecularAlbedo; // Fresnel-weighted, compensated albedo of the dielectric coat along V
};

float3 FresnelSchlick (float3 F0, float CosTheta)
{
	const float t = 1.0f - saturate(CosTheta);
	const float t2 = t * t;
	return F0 + (1.0f - F0) * (t2 * t2 * t);
}

float D_GGX (float NoH, float Alpha)
{
	const float a2 = Alpha * Alpha;
	const float d = (NoH * a2 - NoH) * NoH + 1.0f;
	return a2 / max(kPi * d * d, 1e-30f);
}

float SmithLambda (float CosTheta, float Alpha)
{
	const float c2 = CosTheta * CosTheta;
	const float t2 = max(1.0f - c2, 0.0f) / max(c2, 1e-12f);
	return 0.5f * (-1.0f + sqrt(1.0f + Alpha * Alpha * t2));
}

float SmithG1 (float CosTheta, float Alpha)
{
	return 1.0f / (1.0f + SmithLambda(CosTheta, Alpha));
}

float SmithG2HeightCorrelated (float NoV, float NoL, float Alpha)
{
	return 1.0f / (1.0f + SmithLambda(NoV, Alpha) + SmithLambda(NoL, Alpha));
}

// Bounded VNDF sampling (Eto & Tokuyoshi 2023). Returns the microfacet normal.
float3 SampleGgxBoundedVndf (float3 V, float Alpha, float2 U)
{
	const float3 wiStd = normalize(float3(Alpha * V.xy, V.z));
	const float phi = kTwoPi * U.y;
	const float s = 1.0f + length(V.xy) * (Alpha < 1.0f ? 1.0f : (Alpha > 1.0f ? -1.0f : 0.0f));
	const float a2 = Alpha * Alpha;
	const float s2 = s * s;
	const float k = (1.0f - a2) * s2 / (s2 + a2 * V.z * V.z);
	const float b = V.z > 0.0f ? k * wiStd.z : wiStd.z;
	const float z = mad(-b, U.x, 1.0f - U.x);
	const float sinTheta = SafeSqrt(1.0f - z * z);
	const float3 c = float3(sinTheta * cos(phi), sinTheta * sin(phi), z);
	const float3 wmStd = c + wiStd;
	return normalize(float3(Alpha * wmStd.xy, wmStd.z));
}

// pdf of the REFLECTED direction (solid angle) produced by SampleGgxBoundedVndf.
float PdfGgxBoundedVndfReflection (float3 V, float3 H, float Alpha)
{
	const float ndf = D_GGX(max(H.z, 0.0f), Alpha);
	const float2 ai = Alpha * V.xy;
	const float len2 = dot(ai, ai);
	const float t = sqrt(len2 + V.z * V.z);
	if (V.z >= 0.0f)
	{
		const float s = 1.0f + length(V.xy) * (Alpha < 1.0f ? 1.0f : (Alpha > 1.0f ? -1.0f : 0.0f));
		const float a2 = Alpha * Alpha;
		const float s2 = s * s;
		const float k = (1.0f - a2) * s2 / (s2 + a2 * V.z * V.z);
		return ndf / (2.0f * (k * V.z + t));
	}
	return ndf * (t - V.z) / (2.0f * max(len2, 1e-20f));
}

// Directional albedo E(mu, alpha) of single-scatter GGX with F = 1, and its cosine-weighted
// average Eavg(alpha), read from the LUT the renderer bakes at startup (RG16F, x = mu, y = alpha).
float2 SampleEnergyLut (Texture2D<float2> Lut, SamplerState Sampler, float Mu, float Alpha)
{
	// Texel centres cover [0,1]; remap to avoid edge extrapolation.
	const float2 uv = float2(saturate(Mu), saturate(sqrt(Alpha))) * (31.0f / 32.0f) + (0.5f / 32.0f);
	return Lut.SampleLevel(Sampler, uv, 0.0f);
}

// Fills the energy terms. E is the single-scatter albedo along V; the missing energy
// (1 - E) is re-injected as a scaled specular lobe (Turquin 2019 / Fdez-Agüera 2019).
// Lut = (E, B) from the LUT, or EnergyTermsMirror() for a delta lobe.
void EvalEnergyTerms (inout SBsdfParams P, float NoV, float2 Lut)
{
	const float E = max(Lut.x, 1e-3f);
	P.SpecularAlbedoE = E;
	P.SpecularAlbedoB = min(Lut.y, E);
	// Per channel (Turquin 2019): coloured metals get the saturation of their multiple bounces.
	const float3 favg = P.F0 + (1.0f - P.F0) / 21.0f;   // hemispherical average of Schlick
	P.EnergyScale = 1.0f + favg * (1.0f - E) / E;
	const float favgD = P.DielectricF0 + (1.0f - P.DielectricF0) / 21.0f;
	const float singleD = P.DielectricF0 * (E - P.SpecularAlbedoB) + P.SpecularAlbedoB;
	P.DielectricSpecularAlbedo = saturate(singleD * (1.0f + favgD * (1.0f - E) / E));
}

// Energy terms of a perfect mirror (no LUT): E = 1, B = (1 - NoV)^5 (VoH = NoV).
float2 EnergyTermsMirror (float NoV)
{
	const float c = 1.0f - saturate(NoV);
	return float2(1.0f, (c * c) * (c * c) * c);
}

// Directional albedo of the specular layer along V (Fresnel-weighted, multiple scattering
// included), used for demodulation and lobe selection. Exact for Schlick single scattering.
float3 SpecularDirectionalAlbedo (SBsdfParams P, float NoV)
{
	const float3 single = P.F0 * (P.SpecularAlbedoE - P.SpecularAlbedoB) + P.SpecularAlbedoB;
	return saturate(single * P.EnergyScale);
}

float3 EvalDiffuse (SBsdfParams P, float NoV, float NoL)
{
	// Energy-preserving Lambert: what the dielectric coat reflects is not available to diffuse.
	return P.DiffuseAlbedo * (kInvPi * (1.0f - P.DielectricSpecularAlbedo)) * NoL;
}

// Returns f * NoL for the specular lobe.
float3 EvalSpecular (SBsdfParams P, float3 V, float3 L, float3 H)
{
	const float NoV = V.z;
	const float NoL = L.z;
	if (P.Alpha < kMinAlpha || NoV <= 0.0f || NoL <= 0.0f)
	{
		return 0.0f;
	}
	const float NoH = saturate(H.z);
	const float VoH = saturate(dot(V, H));
	const float D = D_GGX(NoH, P.Alpha);
	const float G = SmithG2HeightCorrelated(NoV, NoL, P.Alpha);
	const float3 F = FresnelSchlick(P.F0, VoH);
	return F * (D * G * P.EnergyScale / (4.0f * NoV));
}

float SpecularLobeProbability (SBsdfParams P, float NoV)
{
	const float spec = Luminance(SpecularDirectionalAlbedo(P, NoV));
	const float diff = Luminance(P.DiffuseAlbedo) * (1.0f - P.DielectricSpecularAlbedo);
	const float total = spec + diff;
	if (total <= 0.0f)
	{
		return 0.5f;
	}
	if (diff <= 0.0f)
	{
		return 1.0f; // pure metal: no diffuse lobe to sample
	}
	// Floor both lobes so neither is starved (keeps MIS weights and the denoiser fed).
	return clamp(spec / total, 0.1f, 0.9f);
}

struct SBsdfEval
{
	float3 Diffuse;      // f_d * NoL
	float3 Specular;     // f_s * NoL
	float Pdf;           // solid-angle pdf of the one-sample mixture (0 for delta)
};

SBsdfEval EvalBsdf (SBsdfParams P, float3 V, float3 L)
{
	SBsdfEval e = (SBsdfEval)0;
	if (V.z <= 0.0f || L.z <= 0.0f)
	{
		return e;
	}
	const float3 H = normalize(V + L);
	e.Diffuse = EvalDiffuse(P, V.z, L.z);
	e.Specular = EvalSpecular(P, V, L, H);

	const float pSpec = SpecularLobeProbability(P, V.z);
	const float pdfDiffuse = L.z * kInvPi;
	const float pdfSpec = P.Alpha >= kMinAlpha ? PdfGgxBoundedVndfReflection(V, H, P.Alpha) : 0.0f;
	e.Pdf = pSpec * pdfSpec + (1.0f - pSpec) * pdfDiffuse;
	return e;
}

struct SBsdfSample
{
	float3 L;
	float3 Weight;       // f * NoL / pdf
	float Pdf;           // 0 => delta (perfect mirror)
	bool bSpecular;      // lobe that generated the sample
	bool bValid;
};

SBsdfSample SampleBsdf (SBsdfParams P, float3 V, float3 U)
{
	SBsdfSample s = (SBsdfSample)0;
	if (V.z <= 0.0f)
	{
		return s;
	}

	const float pSpec = SpecularLobeProbability(P, V.z);
	if (U.z < pSpec)
	{
		s.bSpecular = true;
		if (P.Alpha < kMinAlpha)
		{
			// Perfect mirror: delta lobe, pdf is a Dirac.
			s.L = float3(-V.x, -V.y, V.z);
			const float3 F = FresnelSchlick(P.F0, V.z);
			s.Weight = F * P.EnergyScale / pSpec;
			s.Pdf = 0.0f;
			s.bValid = true;
			return s;
		}
		const float3 H = SampleGgxBoundedVndf(V, P.Alpha, U.xy);
		s.L = reflect(-V, H);
	}
	else
	{
		s.bSpecular = false;
		s.L = SampleCosineHemisphere(U.xy);
	}

	if (s.L.z <= 0.0f)
	{
		return s;
	}

	const SBsdfEval e = EvalBsdf(P, V, s.L);
	if (e.Pdf <= 0.0f)
	{
		return s;
	}
	s.Weight = (e.Diffuse + e.Specular) / e.Pdf;
	s.Pdf = e.Pdf;
	s.bValid = true;
	return s;
}

#endif
