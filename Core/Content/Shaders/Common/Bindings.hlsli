#ifndef FRT_BINDINGS_HLSLI
#define FRT_BINDINGS_HLSLI

// Global root signature (see RHI/Pipelines.h):
//   b0 root constants (per pass, declared by the pass), b1 frame constants,
//   b2 pass constants (declared by the pass), t0 scene TLAS, s0..s5 static samplers.
// Everything else is bindless through ResourceDescriptorHeap[].

#include "Shared/FrameData.h"
#include "Shared/SceneData.h"

ConstantBuffer<SFrameConstants> g_Frame : register(b1);
RaytracingAccelerationStructure g_SceneTlas : register(t0);

SamplerState s_LinearWrap : register(s0);
SamplerState s_LinearClamp : register(s1);
SamplerState s_PointClamp : register(s2);
SamplerState s_PointWrap : register(s3);
SamplerState s_AnisoWrap : register(s4);
SamplerState s_LinearBorder : register(s5);

// Uniform indices (same for the whole dispatch): no NonUniformResourceIndex needed.
Texture2D<float4> Tex2D (uint Index) { return ResourceDescriptorHeap[Index]; }
Texture2D<float> Tex2DFloat (uint Index) { return ResourceDescriptorHeap[Index]; }
Texture2D<float2> Tex2DFloat2 (uint Index) { return ResourceDescriptorHeap[Index]; }
RWTexture2D<float4> RwTex2D (uint Index) { return ResourceDescriptorHeap[Index]; }
RWTexture2D<float> RwTex2DFloat (uint Index) { return ResourceDescriptorHeap[Index]; }
RWTexture2D<float2> RwTex2DFloat2 (uint Index) { return ResourceDescriptorHeap[Index]; }
RWTexture2D<uint> RwTex2DUint (uint Index) { return ResourceDescriptorHeap[Index]; }
Texture2D<uint> Tex2DUint (uint Index) { return ResourceDescriptorHeap[Index]; }
ByteAddressBuffer ByteBuffer (uint Index) { return ResourceDescriptorHeap[Index]; }
RWByteAddressBuffer RwByteBuffer (uint Index) { return ResourceDescriptorHeap[Index]; }

// Divergent indices (per-material / per-geometry): must be marked non-uniform.
Texture2D<float4> Tex2DNonUniform (uint Index) { return ResourceDescriptorHeap[NonUniformResourceIndex(Index)]; }
ByteAddressBuffer ByteBufferNonUniform (uint Index) { return ResourceDescriptorHeap[NonUniformResourceIndex(Index)]; }

// Scene tables. The indices come from frame constants, so they are uniform.
SInstanceData LoadInstance (uint InstanceIndex)
{
	StructuredBuffer<SInstanceData> instances = ResourceDescriptorHeap[g_Frame.InstanceBuffer];
	return instances[InstanceIndex];
}

SGeometryData LoadGeometry (uint GeometryIndex)
{
	StructuredBuffer<SGeometryData> geometries = ResourceDescriptorHeap[g_Frame.GeometryBuffer];
	return geometries[GeometryIndex];
}

SMaterialData LoadMaterial (uint MaterialIndex)
{
	StructuredBuffer<SMaterialData> materials = ResourceDescriptorHeap[g_Frame.MaterialBuffer];
	return materials[MaterialIndex];
}

SLightData LoadLight (uint LightIndex)
{
	StructuredBuffer<SLightData> lights = ResourceDescriptorHeap[g_Frame.LightBuffer];
	return lights[LightIndex];
}

SEmissiveTriangle LoadEmissiveTriangle (uint TriangleIndex)
{
	StructuredBuffer<SEmissiveTriangle> triangles = ResourceDescriptorHeap[g_Frame.EmissiveTriangleBuffer];
	return triangles[TriangleIndex];
}

SAliasEntry LoadLightAlias (uint Index)
{
	StructuredBuffer<SAliasEntry> table = ResourceDescriptorHeap[g_Frame.LightAliasTable];
	return table[Index];
}

SPortalData LoadPortal (uint Index)
{
	StructuredBuffer<SPortalData> portals = ResourceDescriptorHeap[g_Frame.PortalBuffer];
	return portals[Index];
}

bool HasFrameFlag (uint Flag) { return (g_Frame.Flags & Flag) != 0u; }

#endif
