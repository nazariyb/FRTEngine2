// GPU scene description shared by C++ (CGpuScene) and every shader that reads the scene.
#ifndef FRT_SCENE_DATA_H
#define FRT_SCENE_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------------------------
// Instances (one per TLAS instance, indexed by InstanceIndex())
// ---------------------------------------------------------------------------------------------

FRT_GPU_CONST uint InstanceFlag_NegativeDeterminant = 1u << 0u; // mirrored transform (flips bitangent)
FRT_GPU_CONST uint InstanceFlag_Helper = 1u << 1u;              // debug geometry (portal viz)

// TLAS instance masks.
FRT_GPU_CONST uint InstanceMask_Scene = 0x01u;
FRT_GPU_CONST uint InstanceMask_Helper = 0x02u;   // visible to camera rays only
FRT_GPU_CONST uint InstanceMask_Camera = 0xFFu;
FRT_GPU_CONST uint InstanceMask_Secondary = 0x01u;

struct SInstanceData
{
	float4 ObjectToWorld[3];        // rows of the 3x4 column-vector transform (render space)
	float4 PrevObjectToWorld[3];
	float4 NormalToWorld[3];        // rows of inverse-transpose(3x3); w unused
	uint GeometryBase;              // first SGeometryData entry of this instance's model
	uint GeometryCount;
	uint EmissiveTriangleBase;      // first SEmissiveTriangle of this instance, or FRT_INVALID_INDEX
	uint Flags;
};
FRT_GPU_STATIC_ASSERT_SIZE(SInstanceData, 160);

// ---------------------------------------------------------------------------------------------
// Geometry (one per model section; global index = Instance.GeometryBase + GeometryIndex())
// ---------------------------------------------------------------------------------------------

FRT_GPU_CONST uint GeometryFlag_AlphaTested = 1u << 0u;
FRT_GPU_CONST uint GeometryFlag_DoubleSided = 1u << 1u;
FRT_GPU_CONST uint GeometryFlag_Emissive = 1u << 2u;
FRT_GPU_CONST uint GeometryFlag_AlphaBlend = 1u << 3u;   // stochastic transparency

// Compact ray tracing vertex: position f32x3, octahedral normal (snorm16x2), octahedral
// tangent (snorm16x2, sign of bitangent in the lowest bit of y), uv f32x2.
FRT_GPU_CONST uint RtVertexStride = 28u;

struct SGeometryData
{
	uint VertexBufferSrv;       // ByteAddressBuffer of RtVertex
	uint IndexBufferSrv;        // ByteAddressBuffer of uint32 indices
	uint VertexOffset;          // first vertex of this section (indices are section-local)
	uint IndexOffset;           // first index of this section
	uint MaterialIndex;
	uint Flags;
	uint EmissiveLocalBase;     // first emissive triangle of this section within the instance range
	uint TriangleCount;
};
FRT_GPU_STATIC_ASSERT_SIZE(SGeometryData, 32);

// ---------------------------------------------------------------------------------------------
// Materials (glTF metallic-roughness)
// ---------------------------------------------------------------------------------------------

FRT_GPU_CONST uint MaterialFlag_AlphaMask = 1u << 0u;
FRT_GPU_CONST uint MaterialFlag_AlphaBlend = 1u << 1u;
FRT_GPU_CONST uint MaterialFlag_DoubleSided = 1u << 2u;
FRT_GPU_CONST uint MaterialFlag_Unlit = 1u << 3u;

struct SMaterialData
{
	float4 BaseColorFactor;     // linear
	float3 EmissiveFactor;      // linear radiance multiplier (strength folded in)
	float AlphaCutoff;
	float Metallic;
	float Roughness;            // perceptual
	float NormalScale;
	float SpecularFactor;       // KHR_materials_specular strength (1 = F0 0.04 for dielectrics)
	uint BaseColorTexture;      // bindless SRV or FRT_INVALID_INDEX
	uint NormalTexture;
	uint MetallicRoughnessTexture; // G = roughness, B = metallic
	uint EmissiveTexture;
	uint Flags;
	float Ior;
	float Transmission;
	float Pad0;
};
FRT_GPU_STATIC_ASSERT_SIZE(SMaterialData, 80);

// ---------------------------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------------------------

FRT_GPU_CONST uint LightType_Sun = 0u;       // directional with an angular radius
FRT_GPU_CONST uint LightType_Sky = 1u;       // environment map (importance sampled)
FRT_GPU_CONST uint LightType_Sphere = 2u;    // point light when Radius == 0
FRT_GPU_CONST uint LightType_Rect = 3u;      // one-sided rectangle unless LightFlag_TwoSided
FRT_GPU_CONST uint LightType_Spot = 4u;      // sphere light with a cone falloff
FRT_GPU_CONST uint LightType_Triangle = 5u;  // emissive mesh triangle (virtual, see SEmissiveTriangle)

FRT_GPU_CONST uint LightFlag_TwoSided = 1u << 0u;
FRT_GPU_CONST uint LightFlag_CastsShadows = 1u << 1u;

struct SLightData
{
	uint Type;
	uint Flags;
	float Radius;               // sphere radius; sun: cos(angular radius)
	float SolidAngle;           // sun: cone solid angle; rect: area
	float3 Position;            // sphere / rect centre
	float SelectionPower;       // CPU estimate used to build the alias table (informational)
	float3 Radiance;            // emitted radiance (sphere/rect/sun); intensity for point lights
	float SpotCosOuter;
	float3 AxisU;               // rect: half extent U (world); sun: direction TO the sun
	float SpotCosInner;
	float3 AxisV;               // rect: half extent V (world); spot: direction
	float Pad0;
};
FRT_GPU_STATIC_ASSERT_SIZE(SLightData, 80);

// Emissive triangle registered for NEE. Positions are fetched from the geometry and transformed
// with the instance, so moving emitters need no CPU rebuild of this list.
struct SEmissiveTriangle
{
	uint InstanceIndex;
	uint GeometryIndex;         // global SGeometryData index
	uint PrimitiveIndex;        // section-local triangle
	float Power;                // luminance flux estimate (selection weight)
	float3 AverageRadiance;     // emissive factor * mean emissive texel, for RIS targets
	float Area;                 // world-space area at build time
};
FRT_GPU_STATIC_ASSERT_SIZE(SEmissiveTriangle, 32);

// Walker alias table over every light (analytic lights first, then emissive triangles).
struct SAliasEntry
{
	float Threshold;            // probability of keeping this entry
	uint Alias;                 // otherwise take this entry
	float Pmf;                  // selection probability of this entry
	uint Pad0;
};
FRT_GPU_STATIC_ASSERT_SIZE(SAliasEntry, 16);

// Sky portals: openings through which the sky is visible from inside (window, door).
FRT_GPU_CONST uint PortalFlag_Ellipse = 1u << 0u;

struct SPortalData
{
	float3 Center;
	float Area;
	float3 Normal;              // unit, facing the interior
	uint Flags;
	float3 AxisU;               // half extent
	float Pad0;
	float3 AxisV;               // half extent
	float Pad1;
};
FRT_GPU_STATIC_ASSERT_SIZE(SPortalData, 64);

FRT_GPU_NAMESPACE_END

#endif // FRT_SCENE_DATA_H
