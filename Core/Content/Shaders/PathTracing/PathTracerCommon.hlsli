#ifndef FRT_PATH_TRACER_COMMON_HLSLI
#define FRT_PATH_TRACER_COMMON_HLSLI

// Path tracing building blocks shared by the real-time / reference ray generation shader
// (PathTracer.hlsl) and the radiance cache update kernel (RadianceCache/Update.hlsl): pass
// constants, inline tracing, BSDF set-up, next-event estimation with RIS and the BSDF-side MIS
// weights of every kind of emitter. Both estimators must agree on these exactly, or the cache
// would converge to something the full path tracer does not compute.

#include "Common/Bindings.hlsli"
#include "Common/BSDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/Lights.hlsli"
#include "Common/RayTracing.hlsli"
#include "Common/Random.hlsli"
#include "Common/Scene.hlsli"
#include "Common/Sky.hlsli"
#include "Shared/PathTracerData.h"

ConstantBuffer<SPathTracerConstants> g_Pt : register(b2);

bool PtFlag (uint Flag) { return (g_Pt.Flags & Flag) != 0u; }

// ---------------------------------------------------------------------------------------------
// Tracing
// ---------------------------------------------------------------------------------------------

struct STraceResult
{
	bool bHit;
	uint InstanceIndex;
	uint GeometryIndex;
	uint PrimitiveIndex;
	float2 Barycentrics;
	float T;
};

// Closest hit, inline: nothing of the caller's state is spilled to the ray-tracing stack.
STraceResult TraceSceneInline (RayDesc Ray, uint Mask, uint Salt)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(g_SceneTlas, RAY_FLAG_NONE, Mask, Ray);
	while (query.Proceed())
	{
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			!IsTransparentHit(query.CandidateInstanceIndex(), query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex(),
				query.CandidateTriangleBarycentrics(), query.CandidateTriangleRayT(), Salt))
		{
			query.CommitNonOpaqueTriangleHit();
		}
	}
	STraceResult r = (STraceResult)0;
	r.bHit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
	if (r.bHit)
	{
		r.InstanceIndex = query.CommittedInstanceIndex();
		r.GeometryIndex = query.CommittedGeometryIndex();
		r.PrimitiveIndex = query.CommittedPrimitiveIndex();
		r.Barycentrics = query.CommittedTriangleBarycentrics();
		r.T = query.CommittedRayT();
	}
	return r;
}

// ---------------------------------------------------------------------------------------------
// Legacy portal pre-filter (rejection of sky NEE samples that miss every authored portal).
// Kept for comparison with portal-guided sampling; biased if portals do not cover all openings.
// ---------------------------------------------------------------------------------------------

bool PassesPortalPreFilter (float3 Origin, float3 Dir)
{
	uint tests = 0u;
	bool bPass = false;
	for (uint i = 0u; i < g_Frame.PortalCount; ++i)
	{
		++tests;
		float t;
		if (IntersectPortal(LoadPortal(i), Origin, Dir, t))
		{
			bPass = true;
			break;
		}
	}
	CounterAdd(RC_PORTAL_TESTS, tests);
	return bPass;
}

// ---------------------------------------------------------------------------------------------
// BSDF set-up
// ---------------------------------------------------------------------------------------------

SBsdfParams MakeBsdfParams (SMaterialSample M, float NoV, float AlphaFloor)
{
	SBsdfParams p;
	const float dielectricF0 = 0.04f * M.SpecularFactor;
	p.F0 = lerp(dielectricF0.xxx, M.BaseColor, M.Metallic);
	p.DielectricF0 = dielectricF0;
	p.DiffuseAlbedo = M.BaseColor * (1.0f - M.Metallic);
	const float roughness = max(M.Roughness, g_Pt.MinRoughness);
	p.Alpha = max(roughness * roughness, AlphaFloor);
	if (p.Alpha < kMinAlpha)
	{
		p.Alpha = 0.0f;
	}
	float2 lut = EnergyTermsMirror(NoV);
	if (g_Frame.EnergyCompensationLut != FRT_INVALID_INDEX && p.Alpha > 0.0f)
	{
		lut = SampleEnergyLut(Tex2DFloat2(g_Frame.EnergyCompensationLut), s_LinearClamp, NoV, p.Alpha);
	}
	EvalEnergyTerms(p, NoV, lut);
	return p;
}

// Per-lobe pdfs of the one-sample mixture, as BSDF sampling would generate L.
void BsdfLobePdfs (SBsdfParams P, float3 V, float3 L, out float PdfDiffuse, out float PdfSpecular)
{
	PdfDiffuse = 0.0f;
	PdfSpecular = 0.0f;
	if (V.z <= 0.0f || L.z <= 0.0f)
	{
		return;
	}
	const float pSpec = SpecularLobeProbability(P, V.z);
	PdfDiffuse = (1.0f - pSpec) * L.z * kInvPi;
	if (P.Alpha >= kMinAlpha)
	{
		PdfSpecular = pSpec * PdfGgxBoundedVndfReflection(V, normalize(V + L), P.Alpha);
	}
}

// Specular share of the directional albedo along V (0 = Lambertian, 1 = metal): how view
// dependent the light reflected here is.
float SpecularShare (SBsdfParams P, float NoV)
{
	const float spec = Luminance(SpecularDirectionalAlbedo(P, NoV));
	const float diff = Luminance(P.DiffuseAlbedo) * (1.0f - P.DielectricSpecularAlbedo);
	return spec / max(spec + diff, 1e-6f);
}

// Directional albedo of the whole BSDF along V: the fraction of uniform incident light that is
// reflected toward V. Outgoing radiance = DirectionalAlbedo * irradiance / pi, exact for the
// diffuse layer (radiance cache lookups).
float3 DirectionalAlbedo (SBsdfParams P, float NoV)
{
	return P.DiffuseAlbedo * (1.0f - P.DielectricSpecularAlbedo) + SpecularDirectionalAlbedo(P, NoV);
}

// Shading frame at a hit: shading normal from the material, tangent re-orthogonalised to it,
// bitangent keeping the mesh handedness. V is nudged into the hemisphere instead of terminating
// when a shading normal faces away from the viewer (avoids black pixels on normal maps).
struct SShadingFrame
{
	float3 T;
	float3 B;
	float3 N;
	float3 Vlocal;
};

SShadingFrame MakeShadingFrame (SSurfaceHit Hit, SMaterialSample Material, float3 RayDirection)
{
	SShadingFrame f;
	f.N = Material.ShadingNormal;
	float3 t = Hit.Tangent - f.N * dot(f.N, Hit.Tangent);
	f.T = dot(t, t) > 1e-10f ? normalize(t) : Hit.Tangent;
	f.B = cross(f.N, f.T);
	if (dot(f.B, Hit.Bitangent) < 0.0f)
	{
		f.B = -f.B;
	}
	f.Vlocal = ToLocal(-RayDirection, f.T, f.B, f.N);
	if (f.Vlocal.z <= 1e-4f)
	{
		f.Vlocal.z = 1e-4f;
		f.Vlocal = normalize(f.Vlocal);
	}
	return f;
}

// ---------------------------------------------------------------------------------------------
// Next-event estimation with RIS
// ---------------------------------------------------------------------------------------------

struct SNeeResult
{
	float3 Diffuse;
	float3 Specular;
	float3 Irradiance;    // same light sample without the BSDF: L * cos * W * mis (radiance cache)
};

SNeeResult EvaluateNee (
	float3 P,
	float3 Ng,
	float3 T, float3 B, float3 N,
	float3 Vlocal,
	SBsdfParams Bsdf,
	inout SSampler Sampler,
	inout SRng Rng,
	bool bLastVertex,
	bool bSplitLobes,
	uint CandidateCount,
	uint Salt)
{
	SNeeResult result = (SNeeResult)0;
	if (g_Frame.TotalLightCount == 0u || PtFlag(PathFlag_DisableNee) || PtFlag(PathFlag_WhiteFurnace))
	{
		return result;
	}

	const uint candidateCount = max(CandidateCount, 1u);
	float weightSum = 0.0f;
	float selectedTarget = 0.0f;
	float selectedSourcePdf = 0.0f;
	uint selectedLight = FRT_INVALID_INDEX;
	SLightSample selected = (SLightSample)0;
	SBsdfEval selectedEval = (SBsdfEval)0;

	for (uint c = 0u; c < candidateCount; ++c)
	{
		float4 u;
		if (c == 0u)
		{
			u = SampleNext4D(Sampler);
		}
		else
		{
			u = float4(NextFloat(Rng), NextFloat(Rng), NextFloat(Rng), NextFloat(Rng));
		}

		float pmf;
		const uint lightIndex = LightSelectAlias(u.x, pmf);
		if (pmf <= 0.0f)
		{
			continue;
		}
		SLightSample ls = SampleLight(lightIndex, P, u.yzw);
		if (!ls.bValid || ls.Pdf <= 0.0f || dot(ls.Direction, Ng) <= 0.0f)
		{
			continue;
		}
		const float3 Llocal = ToLocal(ls.Direction, T, B, N);
		if (Llocal.z <= 0.0f)
		{
			continue;
		}
		const SBsdfEval e = EvalBsdf(Bsdf, Vlocal, Llocal);
		const float target = Luminance((e.Diffuse + e.Specular) * ls.Radiance);
		const float sourcePdf = pmf * ls.Pdf;
		const float w = target / sourcePdf;
		if (!(w > 0.0f) || !IsFiniteFloat(w))
		{
			continue;
		}
		weightSum += w;
		if (NextFloat(Rng) * weightSum < w)
		{
			selectedTarget = target;
			selectedSourcePdf = sourcePdf;
			selectedLight = lightIndex;
			selected = ls;
			selectedEval = e;
		}
	}

	if (selectedLight == FRT_INVALID_INDEX || selectedTarget <= 0.0f)
	{
		return result;
	}

	const bool bSky = selectedLight == g_Frame.SkyLightIndex;
	const bool bSun = selectedLight == g_Frame.SunLightIndex;
	if (bSky)
	{
		CounterAdd(RC_SKY_SHADOW_ATTEMPTED);
		if (PtFlag(PathFlag_PortalPreFilter) && g_Frame.PortalCount > 0u)
		{
			if (!PassesPortalPreFilter(P, selected.Direction))
			{
				CounterAdd(RC_SKY_SHADOW_PORTAL_REJ);
				return result;
			}
			CounterAdd(RC_SKY_SHADOW_PORTAL_OK);
		}
		else
		{
			CounterAdd(RC_SKY_SHADOW_PORTAL_OK);
		}
	}

	const float3 origin = OffsetRayOrigin(P, Ng);
	const float tMax = selected.Distance >= kFloatMax ? 1e30f : selected.Distance * (1.0f - 1e-4f);
	if (bSun)
	{
		CounterAdd(RC_SUN_SHADOW);
	}
	else if (!bSky)
	{
		CounterAdd(RC_OTHER_SHADOW);
	}
	const bool bVisible = TraceVisibility(origin, selected.Direction, tMax, Salt);
	if (bSky)
	{
		CounterAdd(bVisible ? RC_SKY_SHADOW_REACHED : RC_SKY_SHADOW_BLOCKED);
	}
	if (!bVisible)
	{
		return result;
	}

	// RIS weight W = (1/p_hat(y)) * (1/M) * sum(w).
	const float risWeight = weightSum / (float(candidateCount) * selectedTarget);
	const float3 Llocal = ToLocal(selected.Direction, T, B, N);

	float misDiffuse = 1.0f;
	float misSpecular = 1.0f;
	if (!selected.bDelta && !bLastVertex && !PtFlag(PathFlag_DisableBsdfMis))
	{
		if (bSplitLobes)
		{
			float pdfD, pdfS;
			BsdfLobePdfs(Bsdf, Vlocal, Llocal, pdfD, pdfS);
			misDiffuse = MisBalance(selectedSourcePdf, pdfD);
			misSpecular = MisBalance(selectedSourcePdf, pdfS);
		}
		else
		{
			misDiffuse = MisBalance(selectedSourcePdf, selectedEval.Pdf);
			misSpecular = misDiffuse;
		}
	}

	result.Diffuse = selectedEval.Diffuse * selected.Radiance * (risWeight * misDiffuse);
	result.Specular = selectedEval.Specular * selected.Radiance * (risWeight * misSpecular);
	// RIS is unbiased for any integrand the target covers; the full-mixture MIS weight (the
	// cache update never splits lobes) pairs with the BSDF-sampled irradiance estimate.
	result.Irradiance = selected.Radiance * (Llocal.z * risWeight * misDiffuse);
	return result;
}

// ---------------------------------------------------------------------------------------------
// BSDF-side MIS weights: what a BSDF-sampled ray finds on emitters, weighted against the NEE
// strategy of the vertex it left (balance heuristic, same light pdfs as EvaluateNee).
// PrevBsdfPdf is the solid-angle pdf the previous vertex sampled the ray with; bPrevDelta marks
// the camera and perfect mirrors (NEE cannot generate those, so the weight is 1).
// ---------------------------------------------------------------------------------------------

// Analytic sphere / rect lights are not in the TLAS: intersect them on the traced segment.
float3 AnalyticLightsOnSegment (RayDesc Ray, float SegmentEnd, bool bPrevDelta, float PrevBsdfPdf, float3 PrevPosition)
{
	float3 sum = 0.0f;
	if (g_Frame.LightCount == 0u || PtFlag(PathFlag_WhiteFurnace))
	{
		return sum;
	}
	for (uint li = 0u; li < g_Frame.LightCount; ++li)
	{
		const SLightData light = LoadLight(li);
		float tl;
		float3 le;
		if (light.Type >= LightType_Sphere && light.Type <= LightType_Spot &&
			IntersectAnalyticLight(light, Ray.Origin, Ray.Direction, SegmentEnd, tl, le))
		{
			float w = 1.0f;
			if (!bPrevDelta && !PtFlag(PathFlag_DisableBsdfMis) && !PtFlag(PathFlag_DisableNee))
			{
				const float pdfLight = LightSelectPmf(li) * PdfLightDirection(li, PrevPosition, Ray.Direction, tl, Ray.Origin + Ray.Direction * tl);
				w = MisBalance(PrevBsdfPdf, pdfLight);
			}
			else if (!bPrevDelta && PtFlag(PathFlag_DisableBsdfMis))
			{
				w = 0.0f;
			}
			sum += le * w;
		}
	}
	return sum;
}

// Sky + sun seen by an escaped ray.
float3 EscapedRadiance (float3 Direction, bool bPrevDelta, float PrevBsdfPdf, float3 PrevPosition)
{
	if (PtFlag(PathFlag_WhiteFurnace))
	{
		return 1.0f;
	}
	const float3 sky = EvalSkyEnvironment(Direction);
	const float3 sun = EvalSunDisk(Direction);
	float wSky = 1.0f;
	float wSun = 1.0f;
	if (!bPrevDelta)
	{
		if (PtFlag(PathFlag_DisableBsdfMis))
		{
			wSky = 0.0f;
			wSun = 0.0f;
		}
		else if (!PtFlag(PathFlag_DisableNee))
		{
			if (g_Frame.SkyLightIndex != FRT_INVALID_INDEX)
			{
				wSky = MisBalance(PrevBsdfPdf, LightSelectPmf(g_Frame.SkyLightIndex) * PdfSkyDirection(PrevPosition, Direction));
			}
			if (g_Frame.SunLightIndex != FRT_INVALID_INDEX)
			{
				wSun = MisBalance(PrevBsdfPdf, LightSelectPmf(g_Frame.SunLightIndex) *
					PdfLightDirection(g_Frame.SunLightIndex, PrevPosition, Direction, 0.0f, 0.0f));
			}
		}
	}
	return sky * wSky + sun * wSun;
}

// Weight of the emission of a surface hit by a BSDF ray (only registered emissive triangles
// are NEE light sources; other emissive surfaces are found by BSDF sampling alone).
float SurfaceEmissionMisWeight (SSurfaceHit Hit, bool bPrevDelta, float PrevBsdfPdf, float3 PrevPosition, float3 Direction, float HitT)
{
	if (bPrevDelta)
	{
		return 1.0f;
	}
	const bool bRegistered = (Hit.GeometryFlags & GeometryFlag_Emissive) != 0u;
	if (PtFlag(PathFlag_DisableBsdfMis) && bRegistered)
	{
		return 0.0f;
	}
	if (bRegistered && !PtFlag(PathFlag_DisableNee))
	{
		const SInstanceData instance = LoadInstance(Hit.InstanceIndex);
		const SGeometryData geometry = LoadGeometry(Hit.GeometryIndex);
		if (instance.EmissiveTriangleBase != FRT_INVALID_INDEX && geometry.EmissiveLocalBase != FRT_INVALID_INDEX)
		{
			const uint lightIndex = g_Frame.LightCount + instance.EmissiveTriangleBase + geometry.EmissiveLocalBase + Hit.PrimitiveIndex;
			const float pdfLight = LightSelectPmf(lightIndex) * PdfLightDirection(lightIndex, PrevPosition, Direction, HitT, Hit.Position);
			return MisBalance(PrevBsdfPdf, pdfLight);
		}
	}
	return 1.0f;
}

// Ray cone spread grows with the lobe width (RTXPT-style estimate from the pdf).
float GrowConeSpread (float ConeSpread, float LobePdf)
{
	if (LobePdf > 0.0f)
	{
		const float cosHalf = saturate(1.0f - 1.0f / (kTwoPi * LobePdf));
		ConeSpread += 0.3f * 2.0f * acos(cosHalf);
	}
	return ConeSpread;
}

// FP16 targets saturate at 65504; the solar disk alone is ~1e5. Clamp on the way out so no
// inf ever reaches the denoiser / TAA history (the tone mapper maps both to white anyway).
float3 ClampToHalf (float3 V) { return min(V, 60000.0f); }

// ---------------------------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------------------------

RayDesc GeneratePrimaryRay (float2 PixelPosition)
{
	const float2 ndc = (PixelPosition * g_Frame.InvRenderSize) * 2.0f - 1.0f;
	// Reconstruct through the inverse of the UNjittered view-projection: the jitter is already
	// in PixelPosition. Using the jittered inverse would apply it twice.
	const float4 nearPoint = mul(g_Frame.InvViewProjNoJitter, float4(ndc.x, -ndc.y, 0.5f, 1.0f));
	const float3 target = nearPoint.xyz / nearPoint.w;
	RayDesc ray;
	ray.Origin = g_Frame.CameraPosition;
	ray.Direction = normalize(target - g_Frame.CameraPosition);
	ray.TMin = 0.0f;
	ray.TMax = 1e30f;
	return ray;
}

float2 ProjectToPixel (float4x4 ViewProj, float3 WorldPosition)
{
	const float4 clip = mul(ViewProj, float4(WorldPosition, 1.0f));
	const float2 ndc = clip.xy / clip.w;
	return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * g_Frame.RenderSize;
}

// Gaussian pixel filter by importance sampling (sigma = 0.4 px), reference mode only.
float2 SamplePixelFilter (float2 U)
{
	const float r = 0.4f * SafeSqrt(-2.0f * log(max(1.0f - U.x, 1e-7f)));
	const float phi = kTwoPi * U.y;
	return clamp(float2(cos(phi), sin(phi)) * r, -1.5f, 1.5f);
}

#endif
