// Shared core types/constant buffers
#include "../CoreTypes.hlsli"

// Hit information, aka ray payload.
// Kept minimal to reduce register pressure and SER bandwidth:
//   float3 color    — accumulated radiance                  (12 bytes)
//   uint   depth    — current bounce depth                   (4 bytes)
//   uint   rngState — hash-based PRNG state                  (4 bytes)
// Total: 20 bytes.
// Size must match the MaxPayloadSizeInBytes in D3D12_RAYTRACING_SHADER_CONFIG.
struct HitInfo
{
	float3 color;
	uint depth;
	uint rngState;
};


// Attributes output by the raytracing when hitting a surface,
// here the barycentric coordinates
struct Attributes
{
	float2 bary;
};

// 32-bit hash/PRNG step
uint NextUint (inout uint s)
{
	s ^= s >> 17;
	s *= 0xED5AD4BBu;
	s ^= s >> 11;
	s *= 0xAC4C1B51u;
	s ^= s >> 15;
	s *= 0x31848BABu;
	s ^= s >> 14;
	return s;
}

float NextFloat01 (inout uint s)
{
	return (NextUint(s) & 0x00FFFFFFu) / 16777216.0f; // [0,1)
}
