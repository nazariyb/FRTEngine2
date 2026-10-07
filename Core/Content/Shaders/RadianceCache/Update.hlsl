// Radiance cache update: one full path per tile of TileSize^2 pixels per frame, from a random
// pixel of the tile (so every pixel's surfaces are refreshed every TileSize^2 frames on average).
//
// A cell stores the IRRADIANCE at its surfaces (all incident light: direct + indirect, every
// bounce). A lookup turns it into outgoing radiance with the querying vertex's own BSDF:
// L_o = DirectionalAlbedo(V) * E / pi. That is exact for the diffuse layer, keeps the albedo
// texture of the vertex, and stays view dependent through the albedo (Fresnel) instead of
// freezing whatever directions the update paths happened to arrive from.
//
// Every recorded path vertex x_j keeps
//   cells[j]   its cache entry,
//   weights[j] the factor that turns radiance arriving at the current end of the path into an
//              irradiance sample at x_j: cos/pdf of the direction sampled at x_j, times the BSDF
//              weights of every later bounce, and
//   sums[j]    the irradiance sample accumulated so far.
// NEE at x_j adds its light sample without the BSDF (L cos W mis) to x_j itself, and its
// BSDF-weighted radiance to the earlier vertices; emission / sky / analytic lights found by a
// BSDF ray, and the cached radiance a path ends on, go to every earlier vertex. Each recorded
// vertex then contributes one irradiance sample to its cell.
//
// NEE / MIS are exactly the real-time estimator's (PathTracerCommon.hlsli, full BSDF mixture):
// emission found by a BSDF ray carries the balance weight against NEE at the vertex it left, so
// the two strategies sum to the complete incident light whatever the integrand (radiance or
// irradiance). The vertex's own emission is never part of its irradiance.
//
// Ending a path: from a random depth in [1, FeedbackMaxDepth], or at the last bounce, an eligible
// vertex whose cell is warm stops the path and hands its cached outgoing radiance to all earlier
// vertices (infinite-bounce feedback; the vertex itself gets no sample since nothing was
// estimated for it). Without a warm cell the last vertex only does NEE for the earlier vertices:
// its own sample would lack all indirect light, so it is not recorded.

#include "PathTracing/PathTracerCommon.hlsli"
#include "RadianceCache/RadianceCache.hlsli"

static const uint kRcMaxVertices = 6u;

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID)
{
	const SRadianceCacheParams P = g_Pt.Cache;
	const uint tileSize = max(P.UpdateTileSize, 1u);
	const uint2 renderSize = uint2(g_Frame.RenderSize);
	const uint2 tileCount = (renderSize + tileSize - 1u) / tileSize;
	if (any(Id.xy >= tileCount))
	{
		return;
	}

	SRng rng = CreateRng(PcgHash(Id.x * 0x8da6b343u ^ Id.y * 0xd8163841u) ^ PcgHash(g_Frame.FrameIndex * 0x9e3779b9u + 0x51ed270bu));
	const uint tilePixels = tileSize * tileSize;
	const uint pick = min(uint(NextFloat(rng) * float(tilePixels)), tilePixels - 1u);
	const uint2 pixel = Id.xy * tileSize + uint2(pick % tileSize, pick / tileSize);
	if (any(pixel >= renderSize))
	{
		return;
	}

	// Independent of the real-time sample sequence of that pixel (hashed, no QMC).
	SSampler sampler = CreateSampler(pixel, g_Frame.FrameIndex, 0x2545F491u);
	sampler.bLowDiscrepancy = false;

	RayDesc ray = GeneratePrimaryRay(float2(pixel) + float2(NextFloat(rng), NextFloat(rng)));

	// Vertex history, newest first (index 0), so every access is static and stays in registers.
	uint cells[kRcMaxVertices];
	float3 weights[kRcMaxVertices];
	float3 sums[kRcMaxVertices];
	[unroll]
	for (uint i = 0u; i < kRcMaxVertices; ++i)
	{
		cells[i] = kRcInvalidEntry;
		weights[i] = 0.0f;
		sums[i] = 0.0f;
	}

	const uint maxBounces = min(g_Pt.MaxBounces, kRcMaxVertices);
	const bool bFeedback = RcHasFlag(P, RadianceCacheFlag_Feedback);
	const uint feedbackSpan = clamp(P.FeedbackMaxDepth, 1u, max(maxBounces, 1u));
	const uint feedbackDepth = 1u + min(uint(NextFloat(rng) * float(feedbackSpan)), feedbackSpan - 1u);

	float prevBsdfPdf = 0.0f;
	bool bPrevDelta = true;
	float3 prevPosition = ray.Origin;
	float coneWidth = 0.0f;
	float coneSpread = g_Frame.PixelSpreadAngle;

	for (uint bounce = 0u; bounce <= maxBounces; ++bounce)
	{
		const bool bCamera = bounce == 0u;
		const uint mask = bCamera ? InstanceMask_Camera : InstanceMask_Secondary;
		const uint salt = PcgHash(g_Frame.FrameIndex ^ (bounce * 0x632BE5ABu) ^ 0x7F4A7C15u) ^ (pixel.x << 16u) ^ pixel.y;
		const STraceResult tr = TraceSceneInline(ray, mask, salt);
		const float segmentEnd = tr.bHit ? tr.T : 1e30f;

		// Light arriving along this segment: it belongs to every earlier vertex.
		float3 arriving = 0.0f;
		if (!bCamera)
		{
			arriving += AnalyticLightsOnSegment(ray, segmentEnd, bPrevDelta, prevBsdfPdf, prevPosition);
		}
		if (!tr.bHit)
		{
			if (!bCamera)
			{
				arriving += EscapedRadiance(ray.Direction, bPrevDelta, prevBsdfPdf, prevPosition);
			}
			[unroll]
			for (uint j = 0u; j < kRcMaxVertices; ++j)
			{
				sums[j] += weights[j] * arriving;
			}
			break;
		}

		SSurfaceHit hit = ReconstructSurfaceHit(tr.InstanceIndex, tr.GeometryIndex, tr.PrimitiveIndex, tr.Barycentrics, ray.Direction);
		coneWidth += coneSpread * tr.T;
		const SMaterialSample material = EvaluateMaterial(hit, coneWidth, ray.Direction, PtFlag(PathFlag_NormalMaps));
		const SShadingFrame frame = MakeShadingFrame(hit, material, ray.Direction);
		const SBsdfParams bsdf = MakeBsdfParams(material, frame.Vlocal.z, 0.0f);

		if (!bCamera && any(material.Emission > 0.0f))
		{
			arriving += material.Emission * SurfaceEmissionMisWeight(hit, bPrevDelta, prevBsdfPdf, prevPosition, ray.Direction, tr.T);
		}

		const bool bLastVertex = bounce == maxBounces;
		const float specularShare = SpecularShare(bsdf, frame.Vlocal.z);
		const bool bCacheable = RcCacheableSurface(P, material.Roughness, specularShare);
		const uint entry = bCacheable ? RcInsertEntry(P, RcComputeKey(P, hit.Position, hit.GeometricNormal)) : kRcInvalidEntry;

		// Multi-bounce feedback: the rest of the path is this vertex's BSDF applied to the warm
		// cell's irradiance.
		float3 cachedIrradiance = 0.0f;
		const bool bEndOnCache = !bCamera && bFeedback && (bounce >= feedbackDepth || bLastVertex) &&
			RcEligible(P, hit.Position, material.Roughness, specularShare, coneWidth, tr.T) && RcReadEntry(P, entry, cachedIrradiance);
		if (bEndOnCache)
		{
			arriving += cachedIrradiance * DirectionalAlbedo(bsdf, frame.Vlocal.z) * kInvPi;
		}
		[unroll]
		for (uint j = 0u; j < kRcMaxVertices; ++j)
		{
			sums[j] += weights[j] * arriving;
		}
		if (bEndOnCache)
		{
			break;
		}

		// Record this vertex (its weight is set by the direction it samples below). The last
		// vertex stays unrecorded; its NEE carries MIS weight 1 because no BSDF continuation will
		// look for the same lights.
		if (!bLastVertex)
		{
			[unroll]
			for (uint j = kRcMaxVertices - 1u; j > 0u; --j)
			{
				cells[j] = cells[j - 1u];
				weights[j] = weights[j - 1u];
				sums[j] = sums[j - 1u];
			}
			cells[0] = entry;
			weights[0] = 0.0f;
			sums[0] = 0.0f;
		}
		const SNeeResult nee = EvaluateNee(
			hit.Position, hit.GeometricNormal, frame.T, frame.B, frame.N, frame.Vlocal, bsdf, sampler, rng, bLastVertex, false,
			g_Pt.RisCandidates, salt);
		const float3 reflected = nee.Diffuse + nee.Specular;
		[unroll]
		for (uint j = 0u; j < kRcMaxVertices; ++j)
		{
			sums[j] += weights[j] * reflected;
		}
		if (bLastVertex)
		{
			break;
		}
		sums[0] += nee.Irradiance;

		// BSDF sampling (full mixture, like the reference estimator).
		const float4 ub = SampleNext4D(sampler);
		const SBsdfSample bs = SampleBsdf(bsdf, frame.Vlocal, ub.xyz);
		if (!bs.bValid)
		{
			break;
		}
		const float3 L = ToWorld(bs.L, frame.T, frame.B, frame.N);
		if (dot(L, hit.GeometricNormal) <= 0.0f)
		{
			break;
		}
		if (!IsFiniteFloat3(bs.Weight) || MaxComponent(bs.Weight) <= 0.0f)
		{
			break;
		}
		[unroll]
		for (uint j = 1u; j < kRcMaxVertices; ++j)
		{
			weights[j] *= bs.Weight;
		}
		// Irradiance estimate along the sampled direction: cos / pdf. A delta (mirror) sample has
		// no density: it contributes nothing to this vertex's irradiance, and the diffuse-lobe
		// samples' cos / pdf already accounts for that (their pdf excludes the delta lobe).
		weights[0] = bs.Pdf > 0.0f ? (bs.L.z / bs.Pdf).xxx : 0.0f;

		coneSpread = GrowConeSpread(coneSpread, bs.Pdf);
		prevBsdfPdf = bs.Pdf;
		bPrevDelta = bs.Pdf <= 0.0f;
		prevPosition = hit.Position;
		ray.Origin = SpawnRayOrigin(hit.Position, hit.GeometricNormal, L);
		ray.Direction = L;
		ray.TMin = 0.0f;
		ray.TMax = 1e30f;
	}

	[unroll]
	for (uint j = 0u; j < kRcMaxVertices; ++j)
	{
		if (cells[j] != kRcInvalidEntry)
		{
			RcAccumulate(P, cells[j], sums[j], rng);
		}
	}
}
