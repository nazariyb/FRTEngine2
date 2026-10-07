#ifndef FRT_RADIANCE_CACHE_HLSLI
#define FRT_RADIANCE_CACHE_HLSLI

// World-space radiance cache: a spatially hashed grid in the spirit of NVIDIA SHaRC and of
// Binder et al. 2018 (fast path space filtering by jittered spatial hashing).
//
// A cell is (quantised position, level of detail, orientation class). Its size doubles with
// every octave of distance to the camera, so a cell covers a roughly constant screen area.
// The orientation class (dominant axis and sign of the geometric normal facing the ray) keeps
// the two sides of a thin wall, and floors from the walls next to them, in different cells.
//
// What a cell holds: the IRRADIANCE at its surfaces - all light arriving there, direct and
// indirect, every bounce - which is view independent by definition. A lookup turns it into the
// vertex's full outgoing radiance with the vertex's own BSDF, L_o(V) = DirectionalAlbedo(V) *
// E / pi: exact for the diffuse layer, view dependent through the albedo (Fresnel), and the
// texture of the querying vertex is kept. The vertex's own emission is NOT in the cell: whoever
// terminates a path at a cached vertex adds the emission there exactly, with the same MIS
// weight the full path tracer would use, and then the cached reflection in place of NEE +
// continuation. That keeps the estimator consistent with NEE at the previous vertex (nothing
// double counted) and keeps emitters sharp. Only rough or diffuse-dominated vertices are cached
// (a sharp reflection is not a function of irradiance).
//
// Storage (all RWByteAddressBuffer, permanently in UAV state):
//   keys         uint64 per entry, 0 = empty; buckets of kRcBucketSize entries, linear probing
//                inside the bucket; inserts claim slots with a 64-bit compare-exchange
//   accumulation uint4 per entry: rgb sums in fixed point + sample count of this frame (update)
//   resolved     float3 irradiance + (f16 accumulated weight | stale frames << 16) (resolve)

#include "Common/Bindings.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Shared/PathTracerData.h"

static const uint kRcBucketSize = 16u;              // 16 x 8 B keys: one 128 B cache line per probe
// Fixed point of the per-frame accumulation sums (18.14): 6e-5 resolution with stochastic
// rounding (unbiased), and a per-frame sum of up to 2^18 per channel and cell before a 32-bit
// counter could wrap (64 samples at the default clamp).
static const float kRcRadianceScale = 16384.0f;
static const int kRcMinLevel = -6;
static const int kRcMaxLevel = 25;
static const float kRcSegmentFactor = 1.7320508f;   // sqrt(3): a segment must leave its own cell
static const uint kRcInvalidEntry = 0xFFFFFFFFu;
// Resolve / clear run one thread per entry as a 2D grid of 1024 x N groups of 64 threads: a 1D
// dispatch of 2^22+ entries would exceed the 65535 groups-per-dimension limit of D3D12.
// Must match kResolveGroupsPerRow * kResolveGroupSize in RadianceCache.cpp.
static const uint kRcResolveRowEntries = 1024u * 64u;

bool RcHasFlag (SRadianceCacheParams P, uint Flag) { return (P.Flags & Flag) != 0u; }

// Bob Jenkins' 32-bit integer hash (as in SHaRC).
uint RcHashJenkins (uint A)
{
	A = (A + 0x7ed55d16u) + (A << 12u);
	A = (A ^ 0xc761c23cu) ^ (A >> 19u);
	A = (A + 0x165667b1u) + (A << 5u);
	A = (A + 0xd3a2646cu) ^ (A << 9u);
	A = (A + 0xfd7046c5u) + (A << 3u);
	A = (A ^ 0xb55a4f09u) ^ (A >> 16u);
	return A;
}

int RcLevel (SRadianceCacheParams P, float3 Position)
{
	const float3 d = Position - P.CameraPosition;
	const float distance = sqrt(max(dot(d, d), 1e-8f));
	return clamp(int(floor(log2(distance))), kRcMinLevel, kRcMaxLevel);
}

float RcCellSizeAtLevel (SRadianceCacheParams P, int Level)
{
	return P.CellScale * exp2(float(Level));
}

float RcCellSize (SRadianceCacheParams P, float3 Position)
{
	return RcCellSizeAtLevel(P, RcLevel(P, Position));
}

// Dominant axis and sign of the (ray-facing) geometric normal: 0..5.
uint RcOrientation (float3 N)
{
	const float3 a = abs(N);
	const uint axis = (a.x >= a.y && a.x >= a.z) ? 0u : (a.y >= a.z ? 1u : 2u);
	return axis * 2u + (N[axis] < 0.0f ? 1u : 0u);
}

// 64-bit key as (lo, hi). 3 x 18-bit wrapped cell coordinates, 5-bit level, 3-bit orientation
// and a marker bit, so a valid key is never 0.
uint2 RcMakeKey (int3 Cell, int Level, uint Orientation)
{
	const uint3 c = uint3(Cell) & 0x3FFFFu;
	const uint level = uint(Level - kRcMinLevel) & 0x1Fu;
	uint2 key;
	key.x = c.x | (c.y << 18u);
	key.y = (c.y >> 14u) | (c.z << 4u) | (level << 22u) | ((Orientation & 7u) << 27u) | (1u << 30u);
	return key;
}

uint2 RcComputeKey (SRadianceCacheParams P, float3 Position, float3 Normal)
{
	const int level = RcLevel(P, Position);
	const float cellSize = RcCellSizeAtLevel(P, level);
	const int3 cell = int3(floor(Position / cellSize));
	return RcMakeKey(cell, level, RcOrientation(Normal));
}

uint RcBucketBase (SRadianceCacheParams P, uint2 Key, out uint Start)
{
	const uint h = RcHashJenkins(Key.x ^ RcHashJenkins(Key.y));
	Start = h & (kRcBucketSize - 1u);
	return (h & P.CapacityMask) & ~(kRcBucketSize - 1u);
}

// Read-only lookup. Scans the whole bucket: evictions leave holes, so an empty slot does not
// end the probe sequence. Fully unrolled with a single exit: the 16 loads hit one cache line,
// and a data-dependent loop exit here, inlined into the path tracer's bounce loop, made the
// RTX 40 driver hang the GPU (observed with several samples per pixel).
uint RcFindEntry (SRadianceCacheParams P, uint2 Key)
{
	RWByteAddressBuffer keys = ResourceDescriptorHeap[P.KeyBuffer];
	uint start;
	const uint base = RcBucketBase(P, Key, start);
	uint found = kRcInvalidEntry;
	[unroll]
	for (uint i = 0u; i < kRcBucketSize; ++i)
	{
		const uint slot = base + ((start + i) & (kRcBucketSize - 1u));
		const uint2 k = keys.Load2(slot * 8u);
		if (found == kRcInvalidEntry && all(k == Key))
		{
			found = slot;
		}
	}
	return found;
}

uint64_t RcKeyTo64 (uint2 Key) { return uint64_t(Key.x) | (uint64_t(Key.y) << 32u); }

// Find or insert. Looks the key up first so an eviction hole earlier in the bucket never
// produces a duplicate; concurrent inserts of the same key meet in the same first free slot.
uint RcInsertEntry (SRadianceCacheParams P, uint2 Key)
{
	const uint existing = RcFindEntry(P, Key);
	if (existing != kRcInvalidEntry)
	{
		return existing;
	}
	RWByteAddressBuffer keys = ResourceDescriptorHeap[P.KeyBuffer];
	const uint64_t key64 = RcKeyTo64(Key);
	uint start;
	const uint base = RcBucketBase(P, Key, start);
	for (uint i = 0u; i < kRcBucketSize; ++i)
	{
		const uint slot = base + ((start + i) & (kRcBucketSize - 1u));
		uint64_t previous = 0u;
		keys.InterlockedCompareExchange64(slot * 8u, uint64_t(0u), key64, previous);
		if (previous == 0u || previous == key64)
		{
			return slot;
		}
	}
	return kRcInvalidEntry; // bucket full: this vertex is simply not cached
}

struct SRcResolved
{
	float3 Irradiance;
	float Weight;       // accumulated (decayed) sample weight
	uint StaleFrames;
};

SRcResolved RcLoadResolved (SRadianceCacheParams P, uint Entry)
{
	RWByteAddressBuffer resolved = ResourceDescriptorHeap[P.ResolvedBuffer];
	const uint4 raw = resolved.Load4(Entry * 16u);
	SRcResolved r;
	r.Irradiance = asfloat(raw.xyz);
	r.Weight = f16tof32(raw.w & 0xFFFFu);
	r.StaleFrames = raw.w >> 16u;
	return r;
}

void RcStoreResolved (SRadianceCacheParams P, uint Entry, SRcResolved R)
{
	RWByteAddressBuffer resolved = ResourceDescriptorHeap[P.ResolvedBuffer];
	resolved.Store4(Entry * 16u, uint4(asuint(R.Irradiance), (f32tof16(R.Weight) & 0xFFFFu) | (min(R.StaleFrames, 0xFFFFu) << 16u)));
}

bool RcReadEntry (SRadianceCacheParams P, uint Entry, out float3 Irradiance)
{
	Irradiance = 0.0f;
	if (Entry == kRcInvalidEntry)
	{
		return false;
	}
	const SRcResolved r = RcLoadResolved(P, Entry);
	if (!(r.Weight >= P.MinSampleWeight))
	{
		return false;
	}
	Irradiance = r.Irradiance;
	return true;
}

// Position used for the lookup. Jittering within the tangent plane by up to half a cell turns
// the cell grid into noise (Binder et al.); the denoiser removes it, a visible grid it cannot.
float3 RcJitterPosition (SRadianceCacheParams P, float3 Position, float3 Normal, float2 U)
{
	if (!RcHasFlag(P, RadianceCacheFlag_Jitter))
	{
		return Position;
	}
	float3 t, b;
	BuildOrthonormalBasis(Normal, t, b);
	const float cellSize = RcCellSize(P, Position);
	return Position + (t * (U.x - 0.5f) + b * (U.y - 0.5f)) * cellSize;
}

// Cached irradiance at a surface point (Normal = geometric normal facing the ray). Falls back to
// the unjittered cell when the jittered one does not exist (edges, corners).
bool RcQuery (SRadianceCacheParams P, float3 Position, float3 Normal, float2 U, out float3 Irradiance)
{
	uint entry = RcFindEntry(P, RcComputeKey(P, RcJitterPosition(P, Position, Normal, U), Normal));
	if (entry == kRcInvalidEntry && RcHasFlag(P, RadianceCacheFlag_Jitter))
	{
		entry = RcFindEntry(P, RcComputeKey(P, Position, Normal));
	}
	return RcReadEntry(P, entry, Irradiance);
}

// Adds one path vertex's irradiance sample to its cell. Fixed point with stochastic rounding, so
// the quantisation is unbiased even for very dark cells.
void RcAccumulate (SRadianceCacheParams P, uint Entry, float3 Irradiance, inout SRng Rng)
{
	if (Entry == kRcInvalidEntry || !IsFiniteFloat3(Irradiance))
	{
		return;
	}
	Irradiance = max(Irradiance, 0.0f);
	const float m = MaxComponent(Irradiance);
	if (m > P.SampleClamp)
	{
		Irradiance *= P.SampleClamp / m;
	}
	const uint3 q = uint3(Irradiance * kRcRadianceScale + float3(NextFloat(Rng), NextFloat(Rng), NextFloat(Rng)));
	RWByteAddressBuffer accumulation = ResourceDescriptorHeap[P.AccumulationBuffer];
	const uint address = Entry * 16u;
	if (q.x != 0u) { accumulation.InterlockedAdd(address + 0u, q.x); }
	if (q.y != 0u) { accumulation.InterlockedAdd(address + 4u, q.y); }
	if (q.z != 0u) { accumulation.InterlockedAdd(address + 8u, q.z); }
	accumulation.InterlockedAdd(address + 12u, 1u);
}

// Is a surface's reflection well described by its irradiance? Rough lobes, or surfaces whose
// reflection is dominated by the diffuse layer (a dielectric's 4 % coat barely matters).
// SpecularShare = specular / (specular + diffuse) directional albedo along the incoming ray.
bool RcCacheableSurface (SRadianceCacheParams P, float Roughness, float SpecularShare)
{
	return Roughness >= P.RoughnessThreshold || SpecularShare <= P.SpecularShareMax;
}

// Is a vertex eligible for a cache lookup (shared by the real-time path tracer and the update
// paths' feedback)? The cone footprint and the segment must both exceed the cell, otherwise the
// cell would replace detail the path can still resolve (contact shadows, nearby occluders).
bool RcEligible (SRadianceCacheParams P, float3 Position, float Roughness, float SpecularShare, float ConeWidth, float SegmentLength)
{
	if (!RcCacheableSurface(P, Roughness, SpecularShare))
	{
		return false;
	}
	const float cellSize = RcCellSize(P, Position);
	return ConeWidth >= cellSize * P.FootprintScale && SegmentLength >= cellSize * kRcSegmentFactor;
}

#endif
