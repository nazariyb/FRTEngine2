// Shared core types/constant buffers
#include "../CoreTypes.hlsli"

// Hit information, aka ray payload
// This sample carries shading color, hit distance, and recursion depth.
// Note that the payload should be kept as small as possible,
// and that its size must be declared in the corresponding
// D3D12_RAYTRACING_SHADER_CONFIG pipeline subobjet.
struct HitInfo
{
	float3 color;
	float distance;
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
