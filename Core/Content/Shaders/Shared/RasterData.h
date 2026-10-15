// Raster preview constants (Raster/*.hlsl), shared with CRasterPreview and CRasterShadows.
#ifndef FRT_RASTER_DATA_H
#define FRT_RASTER_DATA_H

#include "SceneData.h"
#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

FRT_GPU_CONST uint RasterMaxEmitters = 4u;     // emissive instances lit as sphere lights
FRT_GPU_CONST uint RasterMaxPortals = 4u;      // sky openings of an enclosed scene
// Local (cube) shadow maps, one atlas row each: the emitters first, then the portals.
FRT_GPU_CONST uint RasterMaxCubeShadows = 4u;
// Shadow views: the sun, then six cube faces per cube shadow (+X, -X, +Y, -Y, +Z, -Z).
FRT_GPU_CONST uint RasterSunShadowView = 0u;
FRT_GPU_CONST uint RasterShadowViewCount = 1u + 6u * RasterMaxCubeShadows;

FRT_GPU_CONST uint RasterFlag_SunShadow = 1u << 0u;        // SunShadowMap is valid
FRT_GPU_CONST uint RasterFlag_Enclosed = 1u << 1u;         // sky only enters through the portals
FRT_GPU_CONST uint RasterFlag_AmbientOcclusion = 1u << 2u; // AmbientOcclusion is valid

// Debug views of the raster preview (SRasterConstants::DebugMode).
FRT_GPU_CONST uint RasterDebug_None = 0u;
FRT_GPU_CONST uint RasterDebug_Sun = 1u;          // sun only (diffuse + specular, shadowed)
FRT_GPU_CONST uint RasterDebug_Emitters = 2u;     // emitter sphere lights only
FRT_GPU_CONST uint RasterDebug_Sky = 3u;          // sky only (through the portals when enclosed)
FRT_GPU_CONST uint RasterDebug_Ambient = 4u;      // energy-balance indirect only
FRT_GPU_CONST uint RasterDebug_Specular = 5u;     // every specular term, no diffuse
FRT_GPU_CONST uint RasterDebug_Diffuse = 6u;      // every diffuse term, no specular
FRT_GPU_CONST uint RasterDebug_SunShadow = 7u;    // sun visibility (white = lit)
FRT_GPU_CONST uint RasterDebug_Occlusion = 8u;    // ambient occlusion
FRT_GPU_CONST uint RasterDebug_Normal = 9u;       // shading normal * 0.5 + 0.5

struct SRasterShadowView
{
	float4x4 ViewProj;             // world -> clip, reversed Z (1 = nearest the light)
	float4 AtlasRect;              // uv offset (xy) and scale (zw) of this view's tile
	float3 Origin;                 // cube views: the light's centre
	float LightRadius;             // cube views: radius of the light (penumbra size), world units
};
FRT_GPU_STATIC_ASSERT_SIZE(SRasterShadowView, 96);

struct SRasterEmitterData
{
	float3 Position;
	float Radius;
	float3 Radiance;               // mean radiance of the emitting surface
	uint ShadowView;               // first of six cube-face views, FRT_INVALID_INDEX = unshadowed
};
FRT_GPU_STATIC_ASSERT_SIZE(SRasterEmitterData, 32);

struct SRasterConstants
{
	SRasterShadowView Views[RasterShadowViewCount];
	SRasterEmitterData Emitters[RasterMaxEmitters];
	SPortalData Portals[RasterMaxPortals];   // normals toward the interior (SRasterAmbient::Portals)
	uint4 PortalShadowViews;       // first cube-face view per portal, FRT_INVALID_INDEX = unshadowed

	float3 AmbientRadiance;        // energy-balance indirect radiance of the enclosure
	uint Flags;                    // RasterFlag_*

	uint SunShadowMap;             // Texture2D<float> depth (bindless SRV)
	uint LocalShadowMap;           // Texture2D<float> depth atlas of the emitters' cube faces
	uint SkyPrefiltered;           // Texture3D<float4>: equal-area direction x roughness
	uint SkyIrradiance;            // Texture2D<float4>: cosine-convolved sky

	uint EmitterCount;
	uint PortalCount;              // valid entries of Portals
	uint AmbientOcclusion;         // Texture2D<float> (render resolution) or FRT_INVALID_INDEX
	uint DebugMode;                // RasterDebug_*

	float2 SunShadowTexel;         // 1 / sun shadow map size
	float2 LocalShadowTexel;       // 1 / atlas size
	float SunShadowTexelWorld;     // world size of one sun shadow texel (normal-offset bias)
	float LocalShadowTileTexels;   // texels across one cube-face tile
	uint SkyPrefilterSlices;
	float Pad0;
};
FRT_GPU_STATIC_ASSERT_SIZE(SRasterConstants, 96 * 25 + 32 * 4 + 64 * 4 + 16 + 16 * 5);

// Root constants of the shadow-map pass (b0).
struct SRasterShadowDrawConstants
{
	uint InstanceIndex;
	uint GeometryIndex;
	uint ViewIndex;
	uint Pad0;
};
FRT_GPU_STATIC_ASSERT_SIZE(SRasterShadowDrawConstants, 16);

FRT_GPU_NAMESPACE_END

#endif // FRT_RASTER_DATA_H
