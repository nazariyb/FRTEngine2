#ifndef FRT_RAYTRACING_HLSLI
#define FRT_RAYTRACING_HLSLI

#include "Common/Bindings.hlsli"
#include "Common/Random.hlsli"
#include "Common/Scene.hlsli"

// Ray origin offset (Wächter & Binder, Ray Tracing Gems ch. 6): move the origin along the
// geometric normal by a few ULPs of the position's magnitude, so spawned rays never re-hit
// their own surface regardless of scene scale. Use with TMin = 0.
float3 OffsetRayOrigin (float3 P, float3 N)
{
	const float kOrigin = 1.0f / 32.0f;
	const float kFloatScale = 1.0f / 65536.0f;
	const float kIntScale = 256.0f;
	const int3 ofi = int3(kIntScale * N);
	const float3 pi = asfloat(asint(P) + select(P < 0.0f, -ofi, ofi));
	return select(abs(P) < kOrigin, P + kFloatScale * N, pi);
}

// Origin on the side of the surface the new direction leaves through.
float3 SpawnRayOrigin (float3 P, float3 GeometricNormal, float3 Direction)
{
	return OffsetRayOrigin(P, dot(Direction, GeometricNormal) >= 0.0f ? GeometricNormal : -GeometricNormal);
}

// Alpha decision shared by any-hit and inline shadow queries. Mask: hard cutoff. Blend:
// stochastic transparency against a hashed threshold (unbiased in expectation).
bool IsTransparentHit (uint InstanceIndex, uint LocalGeometryIndex, uint PrimitiveIndex, float2 Barycentrics, float HitT, uint Salt)
{
	const SInstanceData instance = LoadInstance(InstanceIndex);
	const SGeometryData geometry = LoadGeometry(instance.GeometryBase + LocalGeometryIndex);
	if ((geometry.Flags & (GeometryFlag_AlphaTested | GeometryFlag_AlphaBlend)) == 0u)
	{
		return false;
	}
	const SMaterialData material = LoadMaterial(geometry.MaterialIndex);
	const float alpha = EvaluateOpacity(InstanceIndex, LocalGeometryIndex, PrimitiveIndex, Barycentrics, HitT);
	if ((geometry.Flags & GeometryFlag_AlphaBlend) != 0u)
	{
		const uint h = PcgHash(PrimitiveIndex * 0x9E3779B1u ^ PcgHash(asuint(HitT)) ^ Salt);
		return alpha < UintToUnitFloat(h);
	}
	return alpha < material.AlphaCutoff;
}

// Binary visibility along a segment, inline (no shader table). Non-opaque candidates are
// alpha tested in place.
bool TraceVisibility (float3 Origin, float3 Direction, float TMax, uint Salt)
{
	RayDesc ray;
	ray.Origin = Origin;
	ray.Direction = Direction;
	ray.TMin = 0.0f;
	ray.TMax = TMax;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline(g_SceneTlas, RAY_FLAG_NONE, InstanceMask_Secondary, ray);
	while (query.Proceed())
	{
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
		{
			if (!IsTransparentHit(
				query.CandidateInstanceIndex(),
				query.CandidateGeometryIndex(),
				query.CandidatePrimitiveIndex(),
				query.CandidateTriangleBarycentrics(),
				query.CandidateTriangleRayT(),
				Salt))
			{
				query.CommitNonOpaqueTriangleHit();
			}
		}
	}
	return query.CommittedStatus() == COMMITTED_NOTHING;
}

// Ray counters (RayCounters.h slot layout). Wave-coalesced: one atomic per wave per call.
#define RC_PRIMARY               0u
#define RC_BOUNCE                1u
#define RC_SUN_SHADOW            2u
#define RC_SKY_SHADOW_ATTEMPTED  3u
#define RC_SKY_SHADOW_PORTAL_OK  4u
#define RC_SKY_SHADOW_PORTAL_REJ 5u
#define RC_SKY_SHADOW_REACHED    6u
#define RC_SKY_SHADOW_BLOCKED    7u
#define RC_PORTAL_TESTS          8u
#define RC_OTHER_SHADOW          9u
#define RC_ESCAPED               10u
#define RC_RR_TERMINATED         11u
#define RC_INVALID_SAMPLE        12u

void CounterAdd (uint Slot, uint Amount = 1u)
{
	if ((g_Frame.Flags & FrameFlag_CountersEnabled) == 0u || g_Frame.RayCounterBuffer == FRT_INVALID_INDEX)
	{
		return;
	}
	const uint total = WaveActiveSum(Amount);
	if (WaveIsFirstLane() && total > 0u)
	{
		RWByteAddressBuffer counters = ResourceDescriptorHeap[g_Frame.RayCounterBuffer];
		// 64-bit slots: low word with carry into the high word.
		uint previous;
		counters.InterlockedAdd(Slot * 8u, total, previous);
		if (previous > 0xFFFFFFFFu - total)
		{
			counters.InterlockedAdd(Slot * 8u + 4u, 1u);
		}
	}
}

#endif
