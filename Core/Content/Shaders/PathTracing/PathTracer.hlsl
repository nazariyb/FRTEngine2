// Unidirectional path tracer.
//
// Iterative loop in ray generation (recursion depth 1). Bounce rays use TraceRay with a tiny
// closest-hit that only reports the hit; with SM 6.9 they use HitObject::TraceRay +
// MaybeReorderThread (Shader Execution Reordering) and no closest-hit runs at all. Shading,
// next-event estimation and BSDF sampling all happen here in raygen.
//
// Estimator: NEE with resampled importance sampling over an alias-table light distribution,
// MIS (balance heuristic) against BSDF sampling on both sides - for emissive triangles, analytic
// sphere/rect lights, the sun disk and the sky. Throughput-driven Russian roulette. Shadow rays
// are inline RayQuery with in-place alpha testing.
//
// Real time only: at a secondary vertex whose footprint exceeds a radiance cache cell the path
// ends with throughput * (the vertex's BSDF applied to the cell's cached irradiance), see
// RadianceCache/RadianceCache.hlsli. The vertex's emission is still added exactly first, with
// its usual MIS weight.
//
// Real time only: primary surface replacement (PSR). When the camera ray hits a pure delta
// mirror (metal below kMinAlpha), the reflected surface becomes the pixel's primary surface:
// its normal, view depth, motion and albedos go to the denoiser guide, unfolded through the
// mirror plane (the virtual image behind the mirror), and the mirror's Fresnel weight is folded
// into the stored albedos. The mirror is a delta lobe in both estimators, so this only relabels
// which surface the signals belong to (no bias). Without it the reflection is a roughness-0
// specular signal the denoiser can only accumulate along surface motion: grainy, and smeared
// under camera motion because the reflected image does not move with the mirror.

#include "PathTracing/PathTracerCommon.hlsli"
#include "RadianceCache/RadianceCache.hlsli"

#ifndef FRT_SER
#define FRT_SER 0
#endif

struct [raypayload] SHitPayload
{
	uint InstanceIndex : read(caller) : write(closesthit, miss);
	uint GeometryIndex : read(caller) : write(closesthit);
	uint PrimitiveIndex : read(caller) : write(closesthit);
	float2 Barycentrics : read(caller) : write(closesthit);
	float HitT : read(caller) : write(closesthit);
	uint Salt : read(anyhit) : write(caller);
};

// ---------------------------------------------------------------------------------------------
// Hit / miss / any-hit (the only work done outside raygen)
// ---------------------------------------------------------------------------------------------

[shader("closesthit")]
void ClosestHit (inout SHitPayload Payload, BuiltInTriangleIntersectionAttributes Attributes)
{
	Payload.InstanceIndex = InstanceIndex();
	Payload.GeometryIndex = GeometryIndex();
	Payload.PrimitiveIndex = PrimitiveIndex();
	Payload.Barycentrics = Attributes.barycentrics;
	Payload.HitT = RayTCurrent();
}

[shader("miss")]
void Miss (inout SHitPayload Payload)
{
	Payload.InstanceIndex = FRT_INVALID_INDEX;
}

[shader("anyhit")]
void AnyHit (inout SHitPayload Payload, BuiltInTriangleIntersectionAttributes Attributes)
{
	if (IsTransparentHit(InstanceIndex(), GeometryIndex(), PrimitiveIndex(), Attributes.barycentrics, RayTCurrent(), Payload.Salt))
	{
		IgnoreHit();
	}
}

// ---------------------------------------------------------------------------------------------
// Tracing
// ---------------------------------------------------------------------------------------------

STraceResult TraceScene (RayDesc Ray, uint Mask, uint Salt, bool bReorder, bool bLastSegment)
{
	if (PtFlag(PathFlag_InlineBounces) && !bReorder)
	{
		return TraceSceneInline(Ray, Mask, Salt);
	}
	SHitPayload payload;
	payload.Salt = Salt;
	payload.InstanceIndex = FRT_INVALID_INDEX;
	payload.GeometryIndex = 0u;
	payload.PrimitiveIndex = 0u;
	payload.Barycentrics = 0.0f;
	payload.HitT = 0.0f;

	STraceResult r = (STraceResult)0;
#if FRT_SER
	if (bReorder)
	{
		// One hit group for every geometry: geometry multiplier 0 (the table has a single record).
		dx::HitObject hit = dx::HitObject::TraceRay(g_SceneTlas, RAY_FLAG_NONE, Mask, 0, 0, 0, Ray, payload);
		// Coherence key: which mesh section was hit (data locality for vertex + material fetch)
		// and whether the path ends at this vertex (keeps terminating lanes together).
		uint hint = 0u;
		if (hit.IsHit())
		{
			hint = (hit.GetInstanceIndex() * 131u + hit.GetGeometryIndex() * 17u) & 0x7Fu;
		}
		hint = (hint << 1u) | (bLastSegment ? 1u : 0u);
		dx::MaybeReorderThread(hit, hint, 8u);
		r.bHit = hit.IsHit();
		if (r.bHit)
		{
			BuiltInTriangleIntersectionAttributes attributes;
			hit.GetAttributes(attributes);
			r.InstanceIndex = hit.GetInstanceIndex();
			r.GeometryIndex = hit.GetGeometryIndex();
			r.PrimitiveIndex = hit.GetPrimitiveIndex();
			r.Barycentrics = attributes.barycentrics;
			r.T = hit.GetRayTCurrent();
		}
		return r;
	}
#endif
	// Coherent rays (camera) skip the hit object: the closest-hit only copies the hit record.
	TraceRay(g_SceneTlas, RAY_FLAG_NONE, Mask, 0, 0, 0, Ray, payload);
	r.bHit = payload.InstanceIndex != FRT_INVALID_INDEX;
	r.InstanceIndex = payload.InstanceIndex;
	r.GeometryIndex = payload.GeometryIndex;
	r.PrimitiveIndex = payload.PrimitiveIndex;
	r.Barycentrics = payload.Barycentrics;
	r.T = payload.HitT;
	return r;
}

// ---------------------------------------------------------------------------------------------
// Path state
// ---------------------------------------------------------------------------------------------

static const uint kSignalTotal = 0u;
static const uint kSignalDiffuse = 1u;
static const uint kSignalSpecular = 2u;

struct SPathOutput
{
	float3 Diffuse;       // radiance routed through the primary diffuse lobe (not yet demodulated)
	float3 Specular;
	float3 Emission;      // primary emission / escaped camera ray
	float DiffuseHitT;
	float SpecularHitT;
	float DiffuseHitCount;
	float SpecularHitCount;
	uint PathLength;
	float3 DebugRadiance; // direct-only / indirect-only debug views (one accumulator, mode-selected)
};

void AddRadiance (inout SPathOutput Out, uint Signal, float3 L, uint Bounce)
{
	if (!IsFiniteFloat3(L))
	{
		CounterAdd(RC_INVALID_SAMPLE);
		return;
	}
	L = max(L, 0.0f);
	if (Bounce >= 1u && g_Pt.IndirectClamp > 0.0f)
	{
		const float m = MaxComponent(L);
		if (m > g_Pt.IndirectClamp)
		{
			L *= g_Pt.IndirectClamp / m;
		}
	}
	if ((Bounce == 0u && g_Pt.DebugMode == PathDebug_DirectOnly) || (Bounce > 0u && g_Pt.DebugMode == PathDebug_IndirectOnly))
	{
		Out.DebugRadiance += L;
	}
	if (Signal == kSignalSpecular)
	{
		Out.Specular += L;
	}
	else if (Signal == kSignalDiffuse)
	{
		Out.Diffuse += L;
	}
	else
	{
		Out.Diffuse += L; // total (no split): carried in the diffuse slot
	}
}

// Cache debug views at the primary hit: the outgoing radiance a lookup there would give (no
// jitter, so cells are visible), and the cell layout (brightness = accumulated sample weight).
float3 RadianceCacheDebug (SSurfaceHit Hit, uint Mode, float3 Albedo)
{
	const SRadianceCacheParams P = g_Pt.Cache;
	if (!RcHasFlag(P, RadianceCacheFlag_Valid))
	{
		return float3(0.25f, 0.0f, 0.25f);
	}
	const uint entry = RcFindEntry(P, RcComputeKey(P, Hit.Position, Hit.GeometricNormal));
	if (Mode == PathDebug_RadianceCache)
	{
		float3 irradiance;
		return RcReadEntry(P, entry, irradiance) ? irradiance * Albedo * kInvPi : float3(1.0f, 0.0f, 1.0f) * 1e-3f;
	}
	if (entry == kRcInvalidEntry)
	{
		return 0.0f;
	}
	const uint h = PcgHash(entry);
	const float3 color = float3(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF) / 255.0f;
	const SRcResolved r = RcLoadResolved(P, entry);
	return color * saturate(0.25f + 0.75f * log2(1.0f + r.Weight) / 8.0f);
}

// ---------------------------------------------------------------------------------------------
// Ray generation
// ---------------------------------------------------------------------------------------------

// Denoiser / upscaler G-buffer of the first sample's primary vertex. Written as soon as it is
// known so none of it stays live (and spilled) across the bounce loop.
//
// bSeenInMirror (PSR) is carried in the lowest mantissa bit of ViewZ (a 1e-7 relative change; see
// ViewZSeenInMirror in Common/Math.hlsli): the virtual image of a plane perpendicular to the mirror (floor, ceiling)
// lies ON the real plane, so the denoiser's and the TAA's plane / depth tests alone would accept
// the history and neighbours of the real floor for the reflected one and vice versa (a dark band
// along the mirror edge in motion, where the two meet in screen space).
void WriteGBuffer (uint2 Pixel, float3 Normal, float Roughness, float ViewZ, float2 Motion, float3 DiffuseAlbedo, float3 SpecularAlbedo,
	bool bSeenInMirror)
{
	if (g_Pt.OutDiffuse == FRT_INVALID_INDEX)
	{
		return;
	}
	RwTex2D(g_Pt.OutNormalRoughness)[Pixel] = float4(Normal, Roughness);
	RwTex2DFloat(g_Pt.OutViewZ)[Pixel] = asfloat((asuint(ViewZ) & ~1u) | (bSeenInMirror ? 1u : 0u));
	RwTex2DFloat2(g_Pt.OutMotionVectors)[Pixel] = Motion;
	RwTex2D(g_Pt.OutDiffuseAlbedo)[Pixel] = float4(max(DiffuseAlbedo, 0.01f), 1.0f);
	RwTex2D(g_Pt.OutSpecularAlbedo)[Pixel] = float4(max(SpecularAlbedo, 0.01f), 1.0f);
}

// PSR mirror plane (unit normal, plane offset dot(N, P)); a zero normal = no replacement.
float3 MirrorPoint (float4 Plane, float3 X)
{
	return X - (2.0f * (dot(Plane.xyz, X) - Plane.w)) * Plane.xyz;
}

float3 MirrorDirection (float4 Plane, float3 D)
{
	return D - (2.0f * dot(Plane.xyz, D)) * Plane.xyz;
}

// A pixel may hand its guide to the reflected surface: a delta lobe with nothing else (pure
// metal), so the whole reflected radiance is the mirror's Fresnel weight times what it sees.
bool IsPsrMirror (SBsdfParams Bsdf)
{
	return Bsdf.Alpha <= 0.0f && MaxComponent(Bsdf.DiffuseAlbedo) <= 0.0f;
}

[shader("raygeneration")]
void RayGen ()
{
	const uint2 pixel = DispatchRaysIndex().xy;
	const bool bReference = HasFrameFlag(FrameFlag_ReferenceMode);
	const bool bSplit = PtFlag(PathFlag_SplitLobes) && !bReference;
	const bool bCacheQuery = !bReference && RcHasFlag(g_Pt.Cache, RadianceCacheFlag_Query);
	const bool bPsr = bSplit && PtFlag(PathFlag_PrimarySurfaceReplacement);
	const uint spp = max(g_Pt.SamplesPerPixel, 1u);
	const uint maxBounces = g_Pt.MaxBounces;

	SPathOutput pathOut = (SPathOutput)0;

	// G-buffer of the first sample's primary hit.
	float3 debugValue = 0.0f;
	bool bPrimaryHit = false;

	for (uint s = 0u; s < spp; ++s)
	{
		const uint sampleIndex = (bReference ? g_Frame.AccumulationIndex : g_Frame.FrameIndex) * spp + s;
		SSampler sampler = CreateSampler(pixel, sampleIndex, 0u);
		SRng rng = CreateRng(PcgHash(pixel.x * 0x8da6b343u ^ pixel.y * 0xd8163841u) ^ PcgHash(sampleIndex * 0x9e3779b9u));

		const float4 cameraSample = SampleNext4D(sampler);
		float2 subPixel;
		if (bReference)
		{
			subPixel = 0.5f + SamplePixelFilter(cameraSample.xy);
		}
		else
		{
			// Real time: the TAA jitter positions sample 0; extra samples spread around it.
			subPixel = 0.5f + g_Frame.Jitter + (s > 0u ? (cameraSample.xy - 0.5f) : 0.0f);
		}
		RayDesc ray = GeneratePrimaryRay(float2(pixel) + subPixel);
		CounterAdd(RC_PRIMARY);

		float3 throughput = 1.0f;
		uint signal = kSignalTotal;
		float prevBsdfPdf = 0.0f;       // solid angle; 0 = delta or camera
		bool bPrevDelta = true;
		float3 prevPosition = ray.Origin;
		float coneWidth = 0.0f;
		float coneSpread = g_Frame.PixelSpreadAngle;
		float pathAlphaFloor = 0.0f;
		uint pathLength = 0u;
		// PSR: plane of the mirror the logical primary is seen through (one level: a mirror seen in
		// a mirror stays an ordinary specular primary), and how many segments it added.
		float4 psrPlane = 0.0f;
		uint psrDepth = 0u;

		// "bounce" counts scattering events from the logical primary vertex: a replaced mirror does
		// not count, so the reflected surface gets the primary's lobe split, RIS candidates, cache
		// rules and bounce budget.
		for (uint step = 0u; step <= maxBounces + psrDepth; ++step)
		{
			const uint bounce = step - psrDepth;
			const bool bCamera = bounce == 0u;      // logical primary vertex
			const bool bCameraRay = step == 0u;
			const uint mask = bCameraRay ? InstanceMask_Camera : InstanceMask_Secondary;
			const uint salt = PcgHash(sampleIndex ^ (step * 0x632BE5ABu)) ^ (pixel.x << 16u) ^ pixel.y;
			// White furnace: bounce rays always escape into the unit environment, so a pixel
			// converges to the directional albedo of its primary BSDF (1 for albedo-1 materials).
			STraceResult tr = (STraceResult)0;
			if (bCameraRay || !PtFlag(PathFlag_WhiteFurnace))
			{
				tr = TraceScene(ray, mask, salt, PtFlag(PathFlag_Reorder) && !bCamera, bounce == maxBounces);
			}
			if (!bCameraRay)
			{
				CounterAdd(RC_BOUNCE);
			}

			const float segmentEnd = tr.bHit ? tr.T : 1e30f;

			// Analytic sphere / rect lights are not in the TLAS: intersect them on this segment.
			{
				const float3 analytic = AnalyticLightsOnSegment(ray, segmentEnd, bPrevDelta, prevBsdfPdf, prevPosition);
				if (any(analytic > 0.0f))
				{
					if (bCamera)
					{
						pathOut.Emission += throughput * analytic;
					}
					else
					{
						AddRadiance(pathOut, signal, throughput * analytic, bounce);
					}
				}
			}

			if (!tr.bHit)
			{
				CounterAdd(RC_ESCAPED);
				const float3 env = EscapedRadiance(ray.Direction, bPrevDelta, prevBsdfPdf, prevPosition);
				if (bCamera)
				{
					pathOut.Emission += throughput * env;
					if (s == 0u)
					{
						// Sky seen in a PSR mirror: its virtual direction is the camera ray's.
						const float3 viewDirection = psrDepth > 0u ? MirrorDirection(psrPlane, ray.Direction) : ray.Direction;
						const float3 farPoint = g_Frame.CameraPosition + viewDirection * 1e5f;
						const float2 motion = ProjectToPixel(g_Frame.PrevViewProjNoJitter, farPoint) - ProjectToPixel(g_Frame.ViewProjNoJitter, farPoint);
						WriteGBuffer(pixel, 0.0f, 1.0f, 1e6f, motion, 0.0f, 0.0f, false);
						if (bCameraRay)
						{
							debugValue = env;
						}
					}
				}
				else
				{
					AddRadiance(pathOut, signal, throughput * env, bounce);
					if (bounce == 1u && s == 0u && g_Pt.DebugMode == PathDebug_FirstBounceHit)
					{
						debugValue = float3(1.0f, 0.0f, 1.0f);
					}
					if (bounce == 1u)
					{
						if (signal == kSignalSpecular) { pathOut.SpecularHitT += 1e4f; pathOut.SpecularHitCount += 1.0f; }
						else { pathOut.DiffuseHitT += 1e4f; pathOut.DiffuseHitCount += 1.0f; }
					}
				}
				break;
			}

			++pathLength;
			if (bounce == 1u && s == 0u && g_Pt.DebugMode == PathDebug_FirstBounceHit)
			{
				debugValue = saturate(log2(1.0f + tr.T) / 5.0f).xxx;
			}
			if (bounce == 1u)
			{
				if (signal == kSignalSpecular) { pathOut.SpecularHitT += tr.T; pathOut.SpecularHitCount += 1.0f; }
				else { pathOut.DiffuseHitT += tr.T; pathOut.DiffuseHitCount += 1.0f; }
			}

			SSurfaceHit hit = ReconstructSurfaceHit(tr.InstanceIndex, tr.GeometryIndex, tr.PrimitiveIndex, tr.Barycentrics, ray.Direction);
			coneWidth += coneSpread * tr.T;
			SMaterialSample material = EvaluateMaterial(hit, coneWidth, ray.Direction, PtFlag(PathFlag_NormalMaps));
			if (PtFlag(PathFlag_WhiteFurnace))
			{
				material.BaseColor = 1.0f;
				material.Emission = 0.0f;
			}

			const SShadingFrame frame = MakeShadingFrame(hit, material, ray.Direction);
			const float3 N = frame.N;
			const float3 T = frame.T;
			const float3 Bt = frame.B;
			const float3 Vlocal = frame.Vlocal;

			SBsdfParams bsdf = MakeBsdfParams(material, Vlocal.z, PtFlag(PathFlag_Regularize) ? pathAlphaFloor : 0.0f);

			// PSR: a pure delta mirror hit by the camera ray hands the guide on to what it reflects.
			const float3 psrDirection = reflect(ray.Direction, N);
			const bool bReplace = bPsr && bCameraRay && IsPsrMirror(bsdf) && dot(psrDirection, hit.GeometricNormal) > 0.0f;

			if (bCamera && s == 0u && !bReplace)
			{
				bPrimaryHit = true;
				const SInstanceData instance = LoadInstance(hit.InstanceIndex);
				const SGeometryData geometry = LoadGeometry(hit.GeometryIndex);
				const float3 prevPos = PrevWorldPosition(instance, geometry, hit.PrimitiveIndex,
					float3(1.0f - tr.Barycentrics.x - tr.Barycentrics.y, tr.Barycentrics.x, tr.Barycentrics.y));
				// Seen through a PSR mirror: the virtual surface behind the mirror plane (it lies on
				// the camera ray; a static mirror maps a static point to a static virtual point).
				float3 guideNormal = N;
				float3 guidePosition = hit.Position;
				float3 guidePrevPosition = prevPos;
				if (psrDepth > 0u)
				{
					guideNormal = MirrorDirection(psrPlane, N);
					guidePosition = MirrorPoint(psrPlane, hit.Position);
					guidePrevPosition = MirrorPoint(psrPlane, prevPos);
				}
				const float viewZ = dot(guidePosition - g_Frame.CameraPosition, g_Frame.CameraForward);
				const float2 motion = ProjectToPixel(g_Frame.PrevViewProjNoJitter, guidePrevPosition) - ProjectToPixel(g_Frame.ViewProjNoJitter, guidePosition);
				// The mirror's Fresnel weight (throughput) is part of the demodulation factors, so
				// the denoiser sees the reflected surface's own demodulated lighting.
				WriteGBuffer(pixel, guideNormal, sqrt(max(bsdf.Alpha, 0.0f)), viewZ, motion,
					throughput * bsdf.DiffuseAlbedo, throughput * SpecularDirectionalAlbedo(bsdf, Vlocal.z), psrDepth > 0u);
				// Guide debug views show what the denoiser gets.
				switch (g_Pt.DebugMode)
				{
					case PathDebug_ShadingNormal: debugValue = guideNormal * 0.5f + 0.5f; break;
					case PathDebug_MotionVectors: debugValue = float3(abs(motion) * 0.1f, 0.0f); break;
					case PathDebug_ViewZ: debugValue = frac(viewZ); break;
					default: break;
				}
			}
			if (bCameraRay && s == 0u)
			{
				switch (g_Pt.DebugMode)
				{
					case PathDebug_BaseColor: debugValue = material.BaseColor; break;
					case PathDebug_GeometricNormal: debugValue = hit.GeometricNormal * 0.5f + 0.5f; break;
					case PathDebug_Roughness: debugValue = material.Roughness; break;
					case PathDebug_Metallic: debugValue = material.Metallic; break;
					case PathDebug_Emission: debugValue = material.Emission; break;
					case PathDebug_Uv: debugValue = float3(frac(hit.Uv), 0.0f); break;
					case PathDebug_InstanceId:
					{
						const uint h = PcgHash(hit.InstanceIndex * 1024u + hit.GeometryIndex);
						debugValue = float3(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF) / 255.0f;
						break;
					}
					case PathDebug_RadianceCache:
					case PathDebug_RadianceCacheCells:
						debugValue = RadianceCacheDebug(hit, g_Pt.DebugMode, DirectionalAlbedo(bsdf, Vlocal.z));
						break;
					default: break;
				}
			}

			// Emission seen by this segment.
			if (any(material.Emission > 0.0f))
			{
				const float w = bCamera ? 1.0f : SurfaceEmissionMisWeight(hit, bPrevDelta, prevBsdfPdf, prevPosition, ray.Direction, tr.T);
				if (bCamera)
				{
					pathOut.Emission += throughput * material.Emission;
				}
				else
				{
					AddRadiance(pathOut, signal, throughput * material.Emission * w, bounce);
				}
			}

			if (bReplace)
			{
				// The delta mirror has no NEE and one deterministic direction whose weight is its
				// Fresnel directional albedo, exactly the BSDF sample weight of the delta lobe.
				throughput *= SpecularDirectionalAlbedo(bsdf, Vlocal.z);
				psrPlane = float4(N, dot(N, hit.Position));
				++psrDepth;
				prevBsdfPdf = 0.0f;
				bPrevDelta = true;
				prevPosition = hit.Position;
				ray.Origin = SpawnRayOrigin(hit.Position, hit.GeometricNormal, psrDirection);
				ray.Direction = psrDirection;
				ray.TMin = 0.0f;
				ray.TMax = 1e30f;
				continue;
			}

			// Radiance cache: the rest of the path from here (direct + all further bounces) is the
			// light reflected from the cell's cached irradiance by this vertex's own BSDF. Only
			// where the path's footprint already exceeds the cell, so the cell average replaces
			// nothing the path could still resolve.
			bool bCacheHit = false;
			if (bCacheQuery && !bCamera &&
				RcEligible(g_Pt.Cache, hit.Position, material.Roughness, SpecularShare(bsdf, Vlocal.z), coneWidth, tr.T))
			{
				float3 cached = 0.0f;
				const float2 jitter = float2(NextFloat(rng), NextFloat(rng));
				bCacheHit = RcQuery(g_Pt.Cache, hit.Position, hit.GeometricNormal, jitter, cached);
				if (bCacheHit)
				{
					AddRadiance(pathOut, signal, throughput * cached * DirectionalAlbedo(bsdf, Vlocal.z) * kInvPi, bounce);
				}
			}
			if (bCacheHit)
			{
				break;
			}

			const bool bLastVertex = bounce == maxBounces;

			// Next-event estimation.
			{
				const SNeeResult nee = EvaluateNee(
					hit.Position, hit.GeometricNormal, T, Bt, N, Vlocal, bsdf, sampler, rng, bLastVertex, bSplit && bCamera,
					bCamera ? g_Pt.RisCandidates : g_Pt.RisCandidatesSecondary, salt);
				if (bCamera && bSplit)
				{
					// throughput: 1, or a PSR mirror's Fresnel weight.
					AddRadiance(pathOut, kSignalDiffuse, throughput * nee.Diffuse, 0u);
					AddRadiance(pathOut, kSignalSpecular, throughput * nee.Specular, 0u);
				}
				else
				{
					AddRadiance(pathOut, signal, throughput * (nee.Diffuse + nee.Specular), bounce);
				}
			}

			if (bLastVertex)
			{
				break;
			}

			// Low-discrepancy sampling stops paying off after the first scattering event.
			if (bounce >= 1u)
			{
				sampler.bLowDiscrepancy = false;
			}

			// BSDF sampling.
			const float4 ub = SampleNext4D(sampler);
			SBsdfSample bs = SampleBsdf(bsdf, Vlocal, ub.xyz);
			if (!bs.bValid)
			{
				break;
			}
			const float3 L = ToWorld(bs.L, T, Bt, N);
			if (dot(L, hit.GeometricNormal) <= 0.0f)
			{
				break; // would leave through the geometric surface: light leak
			}

			float3 weight = bs.Weight;
			float lobePdf = bs.Pdf;
			if (bCamera && bSplit)
			{
				// Route this path to the lobe that generated it and weight it by that lobe alone
				// (one-sample lobe selection), so the denoiser gets clean per-lobe signals.
				signal = bs.bSpecular ? kSignalSpecular : kSignalDiffuse;
				if (bs.Pdf > 0.0f)
				{
					const SBsdfEval e = EvalBsdf(bsdf, Vlocal, bs.L);
					float pdfD, pdfS;
					BsdfLobePdfs(bsdf, Vlocal, bs.L, pdfD, pdfS);
					lobePdf = bs.bSpecular ? pdfS : pdfD;
					weight = lobePdf > 0.0f ? (bs.bSpecular ? e.Specular : e.Diffuse) / lobePdf : 0.0f;
				}
			}

			// Russian roulette on the throughput the path would carry after this bounce.
			float3 nextThroughput = throughput * weight;
			if (bounce + 1u >= g_Pt.RussianRouletteDepth && !bCamera)
			{
				// Survival probability = remaining path throughput (already including earlier RR
				// rescaling), so dim paths die early and bright ones are never killed.
				const float m = MaxComponent(nextThroughput);
				if (m < 1.0f)
				{
					const float survive = max(m, 0.05f);
					if (NextFloat(rng) >= survive)
					{
						CounterAdd(RC_RR_TERMINATED);
						break;
					}
					nextThroughput /= survive;
				}
			}
			throughput = nextThroughput;
			if (!IsFiniteFloat3(throughput) || MaxComponent(throughput) <= 0.0f)
			{
				break;
			}

			coneSpread = GrowConeSpread(coneSpread, lobePdf);
			if (lobePdf > 0.0f)
			{
				pathAlphaFloor = max(pathAlphaFloor, 0.5f * bsdf.Alpha);
			}

			prevBsdfPdf = lobePdf;
			bPrevDelta = lobePdf <= 0.0f;
			prevPosition = hit.Position;

			ray.Origin = SpawnRayOrigin(hit.Position, hit.GeometricNormal, L);
			ray.Direction = L;
			ray.TMin = 0.0f;
			ray.TMax = 1e30f;
		}
		pathOut.PathLength += pathLength;
	}

	const float invSpp = 1.0f / float(spp);
	const float3 diffuse = pathOut.Diffuse * invSpp;
	const float3 specular = pathOut.Specular * invSpp;
	const float3 emission = pathOut.Emission * invSpp;

	// Outputs for the real-time pipeline.
	if (g_Pt.OutDiffuse != FRT_INVALID_INDEX)
	{
		// Demodulate by exactly what was stored (incl. the 8-bit quantisation of the diffuse
		// albedo) so remodulation is a lossless round trip.
		const float3 dAlb = bSplit ? RwTex2D(g_Pt.OutDiffuseAlbedo)[pixel].rgb : 1.0f;
		const float3 sAlb = bSplit ? RwTex2D(g_Pt.OutSpecularAlbedo)[pixel].rgb : 1.0f;
		const float diffHit = pathOut.DiffuseHitCount > 0.0f ? pathOut.DiffuseHitT / pathOut.DiffuseHitCount : 0.0f;
		const float specHit = pathOut.SpecularHitCount > 0.0f ? pathOut.SpecularHitT / pathOut.SpecularHitCount : 0.0f;
		RwTex2D(g_Pt.OutDiffuse)[pixel] = float4(ClampToHalf(bSplit ? diffuse / dAlb : diffuse), min(diffHit, 60000.0f));
		RwTex2D(g_Pt.OutSpecular)[pixel] = float4(ClampToHalf(bSplit ? specular / sAlb : specular), min(specHit, 60000.0f));
		RwTex2D(g_Pt.OutEmission)[pixel] = float4(ClampToHalf(emission), bPrimaryHit ? 1.0f : 0.0f);
	}

	const float3 total = diffuse + specular + emission;
	if (g_Pt.OutComposite != FRT_INVALID_INDEX)
	{
		RwTex2D(g_Pt.OutComposite)[pixel] = float4(ClampToHalf(total), 1.0f);
	}

	if (PtFlag(PathFlag_Accumulate) && g_Pt.OutAccumulation != FRT_INVALID_INDEX)
	{
		RWTexture2D<float4> accumulation = RwTex2D(g_Pt.OutAccumulation);
		const float n = float(g_Frame.AccumulationIndex) * float(spp);
		float4 previous = g_Frame.AccumulationIndex > 0u ? accumulation[pixel] : float4(0.0f, 0.0f, 0.0f, 0.0f);
		const float blend = float(spp) / (n + float(spp));
		accumulation[pixel] = float4(lerp(previous.rgb, total, blend), n + float(spp));
	}

	if (g_Pt.OutDebug != FRT_INVALID_INDEX && g_Pt.DebugMode != PathDebug_None)
	{
		float3 dbg = debugValue;
		if (g_Pt.DebugMode == PathDebug_PathLength)
		{
			dbg = float3(saturate(pathOut.PathLength * invSpp / max(float(maxBounces + 1u), 1.0f)), 0.0f, 0.0f);
		}
		else if (g_Pt.DebugMode == PathDebug_DirectOnly)
		{
			dbg = pathOut.DebugRadiance * invSpp + emission;
		}
		else if (g_Pt.DebugMode == PathDebug_IndirectOnly)
		{
			dbg = pathOut.DebugRadiance * invSpp;
		}
		RwTex2D(g_Pt.OutDebug)[pixel] = float4(ClampToHalf(dbg), 1.0f);
	}
}
