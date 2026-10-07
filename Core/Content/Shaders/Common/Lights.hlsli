#ifndef FRT_LIGHTS_HLSLI
#define FRT_LIGHTS_HLSLI

#include "Common/Bindings.hlsli"
#include "Common/Sampling.hlsli"
#include "Common/Scene.hlsli"
#include "Common/Sky.hlsli"

// Unified light indexing: [0, LightCount) analytic lights in LightBuffer,
// [LightCount, TotalLightCount) emissive triangles. Selection is a Walker alias table over all
// of them; NEE then refines the choice with RIS (see PathTracer.hlsl).

struct SLightSample
{
	float3 Direction;       // from the shading point toward the light (unit)
	float Distance;         // to the sampled point (kFloatMax for distant lights)
	float3 Radiance;        // Le arriving along Direction (no visibility, no BSDF)
	float Pdf;              // solid-angle pdf of the position sample (1 for delta lights)
	bool bDelta;
	bool bValid;
};

uint LightSelectAlias (float U, out float Pmf)
{
	const uint count = g_Frame.TotalLightCount;
	const float scaled = U * count;
	const uint index = min(uint(scaled), count - 1u);
	const float frac01 = scaled - index;
	const SAliasEntry e = LoadLightAlias(index);
	const uint chosen = frac01 < e.Threshold ? index : e.Alias;
	Pmf = LoadLightAlias(chosen).Pmf;
	return chosen;
}

float LightSelectPmf (uint LightIndex)
{
	return LoadLightAlias(LightIndex).Pmf;
}

// ---------------------------------------------------------------------------------------------
// Sky with optional portal guiding. Mixture pdf: q * p_portal + (1 - q) * p_env.
// ---------------------------------------------------------------------------------------------

static const float kPortalGuidingProbability = 0.75f;

bool IntersectPortal (SPortalData Portal, float3 Origin, float3 Dir, out float T)
{
	T = 0.0f;
	const float denom = dot(Dir, Portal.Normal);
	if (abs(denom) < 1e-6f)
	{
		return false;
	}
	T = dot(Portal.Center - Origin, Portal.Normal) / denom;
	if (T <= 0.0f)
	{
		return false;
	}
	const float3 local = Origin + Dir * T - Portal.Center;
	const float u = dot(local, Portal.AxisU) / max(dot(Portal.AxisU, Portal.AxisU), 1e-12f);
	const float v = dot(local, Portal.AxisV) / max(dot(Portal.AxisV, Portal.AxisV), 1e-12f);
	return (Portal.Flags & PortalFlag_Ellipse) != 0u ? (u * u + v * v <= 1.0f) : (abs(u) <= 1.0f && abs(v) <= 1.0f);
}

bool PortalGuidingActive ()
{
	return g_Frame.PortalCount > 0u && g_Frame.PortalBuffer != FRT_INVALID_INDEX;
}

// Approximate solid angle used only to choose a portal (its own pdf is exact).
float PortalSelectionWeight (SPortalData Portal, float3 P)
{
	const float3 d = Portal.Center - P;
	const float dist2 = max(dot(d, d), 1e-6f);
	const float cosPortal = abs(dot(Portal.Normal, d)) * rsqrt(dist2);
	// Points on the outside of a portal still see it; the sky through it is just occluded.
	return Portal.Area * max(cosPortal, 0.02f) / dist2;
}

float PdfPortalDirection (float3 P, float3 Dir)
{
	float totalWeight = 0.0f;
	for (uint i = 0u; i < g_Frame.PortalCount; ++i)
	{
		totalWeight += PortalSelectionWeight(LoadPortal(i), P);
	}
	if (totalWeight <= 0.0f)
	{
		return 0.0f;
	}
	float pdf = 0.0f;
	for (uint j = 0u; j < g_Frame.PortalCount; ++j)
	{
		const SPortalData portal = LoadPortal(j);
		float t;
		if (!IntersectPortal(portal, P, Dir, t))
		{
			continue;
		}
		const SSphericalRect rect = InitSphericalRect(P, portal.Center - portal.AxisU - portal.AxisV, 2.0f * portal.AxisU, 2.0f * portal.AxisV);
		if (rect.bValid)
		{
			pdf += (PortalSelectionWeight(portal, P) / totalWeight) / rect.SolidAngle;
		}
	}
	return pdf;
}

float PdfSkyDirection (float3 P, float3 Dir)
{
	const float pEnv = PdfSkyEnvironment(Dir);
	if (!PortalGuidingActive())
	{
		return pEnv;
	}
	return kPortalGuidingProbability * PdfPortalDirection(P, Dir) + (1.0f - kPortalGuidingProbability) * pEnv;
}

float3 SampleSkyDirection (float3 P, float3 U)
{
	if (PortalGuidingActive() && U.z < kPortalGuidingProbability)
	{
		float totalWeight = 0.0f;
		for (uint i = 0u; i < g_Frame.PortalCount; ++i)
		{
			totalWeight += PortalSelectionWeight(LoadPortal(i), P);
		}
		float target = (U.z / kPortalGuidingProbability) * totalWeight;
		uint chosen = g_Frame.PortalCount - 1u;
		for (uint j = 0u; j < g_Frame.PortalCount; ++j)
		{
			const float w = PortalSelectionWeight(LoadPortal(j), P);
			if (target < w)
			{
				chosen = j;
				break;
			}
			target -= w;
		}
		const SPortalData portal = LoadPortal(chosen);
		const SSphericalRect rect = InitSphericalRect(P, portal.Center - portal.AxisU - portal.AxisV, 2.0f * portal.AxisU, 2.0f * portal.AxisV);
		if (rect.bValid)
		{
			const float3 q = SampleSphericalRect(rect, P, U.xy);
			return normalize(q - P);
		}
		// Degenerate view of the portal: fall through to the environment strategy.
	}
	float envPdf;
	return SampleSkyEnvironment(U.xy, envPdf);
}

// ---------------------------------------------------------------------------------------------
// Emissive triangles
// ---------------------------------------------------------------------------------------------

struct SEmissiveTriangleWorld
{
	float3 P0, P1, P2;
	float2 Uv0, Uv1, Uv2;
	uint MaterialIndex;
};

SEmissiveTriangleWorld LoadEmissiveTriangleWorld (SEmissiveTriangle E)
{
	const SInstanceData instance = LoadInstance(E.InstanceIndex);
	const SGeometryData geometry = LoadGeometry(E.GeometryIndex);
	const STriangleVertices tri = LoadTriangle(instance, geometry, E.PrimitiveIndex);
	SEmissiveTriangleWorld w;
	w.P0 = tri.P[0];
	w.P1 = tri.P[1];
	w.P2 = tri.P[2];
	w.Uv0 = tri.Uv[0];
	w.Uv1 = tri.Uv[1];
	w.Uv2 = tri.Uv[2];
	w.MaterialIndex = geometry.MaterialIndex;
	return w;
}

float3 EvalEmissiveTriangleRadiance (SEmissiveTriangleWorld T, float3 Bary)
{
	const SMaterialData m = LoadMaterial(T.MaterialIndex);
	float3 emission = m.EmissiveFactor;
	if (m.EmissiveTexture != FRT_INVALID_INDEX)
	{
		const float2 uv = T.Uv0 * Bary.x + T.Uv1 * Bary.y + T.Uv2 * Bary.z;
		// Light sampling does not know the receiver's footprint: a mid-level mip is a good
		// trade between aliasing and blurring the emitter pattern.
		emission *= SampleMaterialTexture(m.EmissiveTexture, uv, 2.0f).rgb;
	}
	return emission;
}

// ---------------------------------------------------------------------------------------------
// Light sampling / pdf evaluation
// ---------------------------------------------------------------------------------------------

SLightSample SampleLight (uint LightIndex, float3 P, float3 U)
{
	SLightSample s = (SLightSample)0;

	if (LightIndex >= g_Frame.LightCount)
	{
		const SEmissiveTriangle e = LoadEmissiveTriangle(LightIndex - g_Frame.LightCount);
		const SEmissiveTriangleWorld tri = LoadEmissiveTriangleWorld(e);
		const float3 b = SampleUniformTriangle(U.xy);
		const float3 q = tri.P0 * b.x + tri.P1 * b.y + tri.P2 * b.z;
		const float3 nRaw = cross(tri.P1 - tri.P0, tri.P2 - tri.P0);
		const float area = 0.5f * length(nRaw);
		const float3 toLight = q - P;
		const float dist2 = dot(toLight, toLight);
		if (area <= 0.0f || dist2 <= 1e-12f)
		{
			return s;
		}
		const float dist = sqrt(dist2);
		s.Direction = toLight / dist;
		s.Distance = dist;
		const float cosLight = abs(dot(nRaw, s.Direction)) / (2.0f * area);
		if (cosLight <= 1e-6f)
		{
			return s;
		}
		s.Pdf = dist2 / (area * cosLight);
		s.Radiance = EvalEmissiveTriangleRadiance(tri, b);
		s.bValid = true;
		return s;
	}

	const SLightData light = LoadLight(LightIndex);
	switch (light.Type)
	{
		case LightType_Sun:
		{
			float3 t, bt;
			BuildOrthonormalBasis(light.AxisU, t, bt);
			const float3 local = SampleUniformCone(U.xy, light.Radius);
			s.Direction = normalize(ToWorld(local, t, bt, light.AxisU));
			s.Distance = kFloatMax;
			s.Pdf = 1.0f / max(light.SolidAngle, 1e-12f);
			s.Radiance = light.Radiance;
			s.bValid = true;
			break;
		}
		case LightType_Sky:
		{
			s.Direction = SampleSkyDirection(P, U);
			s.Distance = kFloatMax;
			s.Pdf = PdfSkyDirection(P, s.Direction);
			s.Radiance = EvalSkyEnvironment(s.Direction);
			s.bValid = s.Pdf > 0.0f;
			break;
		}
		case LightType_Sphere:
		case LightType_Spot:
		{
			const float3 toCenter = light.Position - P;
			const float dist2 = dot(toCenter, toCenter);
			const float dist = sqrt(dist2);
			if (dist <= 1e-5f)
			{
				break;
			}
			const float3 w = toCenter / dist;
			float spot = 1.0f;
			if (light.Type == LightType_Spot)
			{
				spot = smoothstep(light.SpotCosOuter, light.SpotCosInner, dot(-w, light.AxisV));
				if (spot <= 0.0f)
				{
					break;
				}
			}
			if (light.Radius <= 0.0f || dist <= light.Radius * 1.001f)
			{
				// Point light (delta). Radiance carries intensity / d^2.
				s.Direction = w;
				s.Distance = dist;
				s.Pdf = 1.0f;
				s.bDelta = true;
				s.Radiance = light.Radiance * spot / dist2;
				s.bValid = true;
				break;
			}
			const float sinMax2 = saturate(light.Radius * light.Radius / dist2);
			const float cosMax = SafeSqrt(1.0f - sinMax2);
			float3 t, bt;
			BuildOrthonormalBasis(w, t, bt);
			const float3 local = SampleUniformCone(U.xy, cosMax);
			s.Direction = normalize(ToWorld(local, t, bt, w));
			// Distance to the sphere surface along the sampled direction.
			const float b = dot(toCenter, s.Direction);
			const float c = dist2 - light.Radius * light.Radius;
			const float disc = max(b * b - c, 0.0f);
			s.Distance = max(b - sqrt(disc), 0.0f);
			s.Pdf = 1.0f / (kTwoPi * max(1.0f - cosMax, 1e-9f));
			s.Radiance = light.Radiance * spot;
			s.bValid = true;
			break;
		}
		case LightType_Rect:
		{
			const float3 corner = light.Position - light.AxisU - light.AxisV;
			const float3 n = normalize(cross(light.AxisU, light.AxisV));
			const bool bTwoSided = (light.Flags & LightFlag_TwoSided) != 0u;
			if (!bTwoSided && dot(P - light.Position, n) <= 0.0f)
			{
				break; // behind a one-sided emitter
			}
			const SSphericalRect rect = InitSphericalRect(P, corner, 2.0f * light.AxisU, 2.0f * light.AxisV);
			float3 q;
			if (rect.bValid)
			{
				q = SampleSphericalRect(rect, P, U.xy);
			}
			else
			{
				q = corner + U.x * 2.0f * light.AxisU + U.y * 2.0f * light.AxisV;
			}
			const float3 toLight = q - P;
			const float dist2 = dot(toLight, toLight);
			if (dist2 <= 1e-12f)
			{
				break;
			}
			const float dist = sqrt(dist2);
			s.Direction = toLight / dist;
			s.Distance = dist;
			if (rect.bValid)
			{
				s.Pdf = 1.0f / rect.SolidAngle;
			}
			else
			{
				const float cosLight = abs(dot(n, s.Direction));
				const float area = 4.0f * length(light.AxisU) * length(light.AxisV);
				s.Pdf = dist2 / max(area * cosLight, 1e-12f);
			}
			s.Radiance = light.Radiance;
			s.bValid = true;
			break;
		}
		default:
			break;
	}
	return s;
}

// Solid-angle pdf that SampleLight would assign to a direction that hit the light (for the
// BSDF-side MIS weight). HitDistance / HitNormal describe where the BSDF ray hit it.
float PdfLightDirection (uint LightIndex, float3 P, float3 Dir, float HitDistance, float3 HitPoint)
{
	if (LightIndex >= g_Frame.LightCount)
	{
		const SEmissiveTriangle e = LoadEmissiveTriangle(LightIndex - g_Frame.LightCount);
		const SEmissiveTriangleWorld tri = LoadEmissiveTriangleWorld(e);
		const float3 nRaw = cross(tri.P1 - tri.P0, tri.P2 - tri.P0);
		const float area = 0.5f * length(nRaw);
		const float cosLight = abs(dot(nRaw, Dir)) / max(2.0f * area, 1e-30f);
		return cosLight > 1e-6f ? HitDistance * HitDistance / (area * cosLight) : 0.0f;
	}

	const SLightData light = LoadLight(LightIndex);
	switch (light.Type)
	{
		case LightType_Sun:
			return dot(Dir, light.AxisU) >= light.Radius ? 1.0f / max(light.SolidAngle, 1e-12f) : 0.0f;
		case LightType_Sky:
			return PdfSkyDirection(P, Dir);
		case LightType_Sphere:
		case LightType_Spot:
		{
			if (light.Radius <= 0.0f)
			{
				return 0.0f; // delta: never hit by BSDF rays
			}
			const float3 toCenter = light.Position - P;
			const float dist2 = dot(toCenter, toCenter);
			const float sinMax2 = saturate(light.Radius * light.Radius / dist2);
			const float cosMax = SafeSqrt(1.0f - sinMax2);
			return 1.0f / (kTwoPi * max(1.0f - cosMax, 1e-9f));
		}
		case LightType_Rect:
		{
			const float3 corner = light.Position - light.AxisU - light.AxisV;
			const SSphericalRect rect = InitSphericalRect(P, corner, 2.0f * light.AxisU, 2.0f * light.AxisV);
			if (rect.bValid)
			{
				return 1.0f / rect.SolidAngle;
			}
			const float3 n = normalize(cross(light.AxisU, light.AxisV));
			const float cosLight = abs(dot(n, Dir));
			const float area = 4.0f * length(light.AxisU) * length(light.AxisV);
			return HitDistance * HitDistance / max(area * cosLight, 1e-12f);
		}
		default:
			return 0.0f;
	}
}

// Analytic lights that are not geometry in the TLAS must still be found by BSDF rays for MIS
// to be complete. Sphere and rect lights are intersected analytically against the ray segment.
bool IntersectAnalyticLight (SLightData Light, float3 Origin, float3 Dir, float TMax, out float T, out float3 Radiance)
{
	T = 0.0f;
	Radiance = 0.0f;
	if (Light.Type == LightType_Sphere || Light.Type == LightType_Spot)
	{
		if (Light.Radius <= 0.0f)
		{
			return false;
		}
		const float3 oc = Origin - Light.Position;
		const float b = dot(oc, Dir);
		const float c = dot(oc, oc) - Light.Radius * Light.Radius;
		const float disc = b * b - c;
		if (disc < 0.0f || c < 0.0f)
		{
			return false;
		}
		T = -b - sqrt(disc);
		if (T <= 0.0f || T >= TMax)
		{
			return false;
		}
		float spot = 1.0f;
		if (Light.Type == LightType_Spot)
		{
			spot = smoothstep(Light.SpotCosOuter, Light.SpotCosInner, dot(Dir, Light.AxisV));
		}
		Radiance = Light.Radiance * spot;
		return spot > 0.0f;
	}
	if (Light.Type == LightType_Rect)
	{
		const float3 n = normalize(cross(Light.AxisU, Light.AxisV));
		const float denom = dot(Dir, n);
		const bool bTwoSided = (Light.Flags & LightFlag_TwoSided) != 0u;
		if (abs(denom) < 1e-8f || (!bTwoSided && denom >= 0.0f))
		{
			return false;
		}
		T = dot(Light.Position - Origin, n) / denom;
		if (T <= 0.0f || T >= TMax)
		{
			return false;
		}
		const float3 local = Origin + Dir * T - Light.Position;
		const float u = dot(local, Light.AxisU) / dot(Light.AxisU, Light.AxisU);
		const float v = dot(local, Light.AxisV) / dot(Light.AxisV, Light.AxisV);
		if (abs(u) > 1.0f || abs(v) > 1.0f)
		{
			return false;
		}
		Radiance = Light.Radiance;
		return true;
	}
	return false;
}

#endif
