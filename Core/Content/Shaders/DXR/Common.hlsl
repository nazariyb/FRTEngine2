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
};


// Attributes output by the raytracing when hitting a surface,
// here the barycentric coordinates
struct Attributes
{
	float2 bary;
};
