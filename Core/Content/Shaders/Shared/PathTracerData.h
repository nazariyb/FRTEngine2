// Path tracer pass constants (root CBV b2), shared by C++ (CPathTracer) and PathTracer.hlsl.
#ifndef FRT_PATH_TRACER_DATA_H
#define FRT_PATH_TRACER_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

FRT_GPU_CONST uint PathFlag_Reorder = 1u << 0u;              // SER MaybeReorderThread on bounces
FRT_GPU_CONST uint PathFlag_SplitLobes = 1u << 1u;           // demodulated diffuse/specular outputs
FRT_GPU_CONST uint PathFlag_PortalPreFilter = 1u << 2u;      // legacy: reject sky NEE outside portals
FRT_GPU_CONST uint PathFlag_Regularize = 1u << 3u;           // path-space roughness regularisation
FRT_GPU_CONST uint PathFlag_DisableNee = 1u << 4u;           // BSDF sampling only (validation)
FRT_GPU_CONST uint PathFlag_DisableBsdfMis = 1u << 5u;       // NEE only for emitters (validation)
FRT_GPU_CONST uint PathFlag_Accumulate = 1u << 6u;           // write the reference accumulation
FRT_GPU_CONST uint PathFlag_NormalMaps = 1u << 7u;
FRT_GPU_CONST uint PathFlag_WhiteFurnace = 1u << 8u;         // albedo 1, unit sky, no lights, bounce rays escape
FRT_GPU_CONST uint PathFlag_PortalGuiding = 1u << 9u;
FRT_GPU_CONST uint PathFlag_InlineBounces = 1u << 10u;       // bounce rays via RayQuery (no RT stack spill)
FRT_GPU_CONST uint PathFlag_PrimarySurfaceReplacement = 1u << 11u; // real time: the denoiser guide of a delta-mirror pixel is the reflected surface

FRT_GPU_CONST uint PathDebug_None = 0u;
FRT_GPU_CONST uint PathDebug_BaseColor = 1u;
FRT_GPU_CONST uint PathDebug_ShadingNormal = 2u;
FRT_GPU_CONST uint PathDebug_GeometricNormal = 3u;
FRT_GPU_CONST uint PathDebug_Roughness = 4u;
FRT_GPU_CONST uint PathDebug_Metallic = 5u;
FRT_GPU_CONST uint PathDebug_Emission = 6u;
FRT_GPU_CONST uint PathDebug_MotionVectors = 7u;
FRT_GPU_CONST uint PathDebug_ViewZ = 8u;
FRT_GPU_CONST uint PathDebug_PathLength = 9u;
FRT_GPU_CONST uint PathDebug_DirectOnly = 10u;
FRT_GPU_CONST uint PathDebug_IndirectOnly = 11u;
FRT_GPU_CONST uint PathDebug_Uv = 12u;
FRT_GPU_CONST uint PathDebug_InstanceId = 13u;
FRT_GPU_CONST uint PathDebug_FirstBounceHit = 14u; // log hit distance of the first bounce ray, magenta = escaped
FRT_GPU_CONST uint PathDebug_RadianceCache = 15u;  // cached radiance at the primary hit (magenta = no valid cell)
FRT_GPU_CONST uint PathDebug_RadianceCacheCells = 16u; // one colour per cache cell, brightness = sample weight

// World-space radiance cache (see RadianceCache/RadianceCache.hlsli).
FRT_GPU_CONST uint RadianceCacheFlag_Query = 1u << 0u;     // real-time paths terminate at eligible cached vertices
FRT_GPU_CONST uint RadianceCacheFlag_Feedback = 1u << 1u;  // update paths end at cached vertices (multi-bounce)
FRT_GPU_CONST uint RadianceCacheFlag_Jitter = 1u << 2u;    // jitter lookups within a cell (hides the grid)
FRT_GPU_CONST uint RadianceCacheFlag_Valid = 1u << 3u;     // buffers exist (debug views may read them)

struct SRadianceCacheParams
{
	float3 CameraPosition;        // level-of-detail origin
	float CellScale;              // cell size (m) at 1 m from the camera; cells double per octave of distance

	uint KeyBuffer;               // RWByteAddressBuffer UAV: uint64 key per entry (0 = empty)
	uint AccumulationBuffer;      // RWByteAddressBuffer UAV: uint4 (rgb fixed point sums, sample count)
	uint ResolvedBuffer;          // RWByteAddressBuffer UAV: float3 radiance, uint (f16 weight | stale << 16)
	uint CapacityMask;            // entries - 1 (power of two)

	float RoughnessThreshold;     // minimum perceptual roughness of a cached vertex
	float FootprintScale;         // query when cone width >= cell size * FootprintScale
	float MinSampleWeight;        // a cell answers queries once its accumulated weight reaches this
	float SampleClamp;            // max component of one update sample (fireflies)

	uint Flags;                   // RadianceCacheFlag_*
	uint UpdateTileSize;          // one update path per tile of TileSize^2 pixels per frame
	uint AccumulationFrames;      // history length of the resolve blend
	uint StaleFrames;             // frames without samples before a cell is evicted

	uint FeedbackMaxDepth;        // update paths may end on the cache from a uniform depth in [1, this]
	float SpecularShareMax;       // vertices with a smaller specular share of their albedo are cacheable at any roughness
	uint Pad0;
	uint Pad1;
};
FRT_GPU_STATIC_ASSERT_SIZE(SRadianceCacheParams, 80);

struct SPathTracerConstants
{
	uint SamplesPerPixel;
	uint MaxBounces;
	uint RussianRouletteDepth;
	uint Flags;

	uint RisCandidates;
	float IndirectClamp;          // max luminance of a single indirect contribution (0 = off)
	float MinRoughness;
	uint DebugMode;

	uint OutDiffuse;              // RGBA16F: demodulated diffuse radiance, a = hit distance
	uint OutSpecular;             // RGBA16F: demodulated specular radiance, a = hit distance
	uint OutNormalRoughness;      // RGBA16F: world normal, a = linear roughness
	uint OutViewZ;                // R32F: linear view depth (FarZ-ish for sky)

	uint OutMotionVectors;        // RG16F: (previous - current) position, render pixels
	uint OutDiffuseAlbedo;        // R11G11B10F
	uint OutSpecularAlbedo;       // R11G11B10F
	uint OutEmission;             // RGBA16F: primary emission + escaped camera rays

	uint OutAccumulation;         // RGBA32F reference accumulation (rgb mean, a = sample count)
	uint OutDebug;                // RGBA16F debug visualisation
	uint OutComposite;            // RGBA16F full radiance this frame (no denoiser path)
	uint RisCandidatesSecondary;  // RIS candidates at bounce vertices (RisCandidates: primary)

	SRadianceCacheParams Cache;
};
FRT_GPU_STATIC_ASSERT_SIZE(SPathTracerConstants, 160);

FRT_GPU_NAMESPACE_END

#endif
