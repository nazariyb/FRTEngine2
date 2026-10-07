// Radiance cache resolve (one thread per hash entry) and clear. Cells hold irradiance.
//
// Resolve blends this frame's accumulated samples into the resolved irradiance. The history is a
// sample-weighted running mean whose weight decays by (1 - 1/AccumulationFrames) on every frame
// that brings samples: a fresh cell averages all its samples, a busy cell becomes an exponential
// moving average over ~AccumulationFrames frames, whatever its sample rate. Cells that received
// nothing for StaleFrames frames are evicted (their slot becomes a hole the bucket scan skips).

#include "RadianceCache/RadianceCache.hlsli"

ConstantBuffer<SPathTracerConstants> g_Pt : register(b2);

[numthreads(64, 1, 1)]
void Resolve (uint3 Id : SV_DispatchThreadID)
{
	const SRadianceCacheParams P = g_Pt.Cache;
	const uint entry = Id.y * kRcResolveRowEntries + Id.x;
	if (entry > P.CapacityMask)
	{
		return;
	}
	RWByteAddressBuffer keys = ResourceDescriptorHeap[P.KeyBuffer];
	if (all(keys.Load2(entry * 8u) == 0u))
	{
		return;
	}
	RWByteAddressBuffer accumulation = ResourceDescriptorHeap[P.AccumulationBuffer];
	const uint4 sums = accumulation.Load4(entry * 16u);
	SRcResolved r = RcLoadResolved(P, entry);

	if (sums.w > 0u)
	{
		const float n = float(sums.w);
		const float3 frameSum = float3(sums.xyz) * (1.0f / kRcRadianceScale);
		const float history = r.Weight * (1.0f - 1.0f / float(max(P.AccumulationFrames, 1u)));
		const float total = history + n;
		r.Irradiance = (r.Irradiance * history + frameSum) / total;
		r.Weight = min(total, 60000.0f);
		r.StaleFrames = 0u;
		accumulation.Store4(entry * 16u, uint4(0u, 0u, 0u, 0u));
	}
	else
	{
		r.StaleFrames += 1u;
		if (r.StaleFrames > P.StaleFrames)
		{
			keys.Store2(entry * 8u, uint2(0u, 0u));
			RWByteAddressBuffer resolved = ResourceDescriptorHeap[P.ResolvedBuffer];
			resolved.Store4(entry * 16u, uint4(0u, 0u, 0u, 0u));
			return;
		}
	}
	if (!IsFiniteFloat3(r.Irradiance))
	{
		r.Irradiance = 0.0f;
		r.Weight = 0.0f;
	}
	RcStoreResolved(P, entry, r);
}

[numthreads(64, 1, 1)]
void Clear (uint3 Id : SV_DispatchThreadID)
{
	const SRadianceCacheParams P = g_Pt.Cache;
	const uint entry = Id.y * kRcResolveRowEntries + Id.x;
	if (entry > P.CapacityMask)
	{
		return;
	}
	RWByteAddressBuffer keys = ResourceDescriptorHeap[P.KeyBuffer];
	RWByteAddressBuffer accumulation = ResourceDescriptorHeap[P.AccumulationBuffer];
	RWByteAddressBuffer resolved = ResourceDescriptorHeap[P.ResolvedBuffer];
	keys.Store2(entry * 8u, uint2(0u, 0u));
	accumulation.Store4(entry * 16u, uint4(0u, 0u, 0u, 0u));
	resolved.Store4(entry * 16u, uint4(0u, 0u, 0u, 0u));
}
