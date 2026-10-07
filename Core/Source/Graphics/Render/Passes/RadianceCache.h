#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Shared/PathTracerData.h"


namespace frt::graphics
{
struct SRadianceCacheSettings
{
	bool bEnabled = true;
	uint32 CapacityLog2 = 20u;            // hash entries = 2^CapacityLog2 (40 B each); Sponza needs < 2^18
	float CellScale = 0.06f;              // cell size (m) at 1 m from the camera, doubling per octave of distance
	uint32 UpdateTileSize = 5u;           // one update path per TileSize x TileSize pixels per frame (5: 4 %)
	// Resolve history length: stability vs lag after lighting changes. The denoiser's own 64-frame
	// history hides the extra cache noise: 32 -> 16 left the final image unchanged (static default
	// view relMSE 0.0512 / SSIM 0.593, fast camera motion SSIM 0.365 both ways; raw 1-spp relMSE
	// 17.3 -> 17.5) and cut the share of a sudden indirect-lighting change (sun step, TimeOfDay
	// 0.5 -> 0.47) still missing 1 s later from 0.60 to 0.50.
	uint32 AccumulationFrames = 16u;
	uint32 StaleFrames = 128u;            // frames without samples before a cell is evicted
	float RoughnessThreshold = 0.25f;     // glossier vertices are not answered from the cache: irradiance cannot give a sharp reflection
	float FootprintScale = 1.0f;          // query when the ray cone is wider than FootprintScale cells
	float MinSampleWeight = 8.0f;         // accumulated samples before a cell answers queries
	float SampleClamp = 4096.0f;          // max component of one irradiance sample: overflow guard (lower values bias dark)
	float SpecularShareMax = 0.15f;       // ... unless the specular share of the albedo is below this (diffuse dominated)
	bool bFeedback = true;                // update paths end at cached vertices: multi-bounce beyond MaxBounces
	uint32 FeedbackMaxDepth = 4u;         // update paths may end on the cache from a uniform depth in [1, this] (1 starves deep cells)
	bool bJitter = true;                  // jittered lookups (Binder et al.): no visible cell grid
};

/**
 * World-space radiance cache (SHaRC-style spatial hash grid), real-time mode only.
 *
 * Each frame, before the path tracer:
 *   - Update: a rotating subset of pixels (one per tile) traces full paths and adds, for every
 *     path vertex, the irradiance the rest of the path brought to it into that vertex's cell;
 *   - Resolve: blends the frame's sums into the resolved irradiance (bounded history), evicts
 *     cells that stopped receiving samples, clears the sums.
 * The real-time path tracer then ends paths at eligible secondary vertices with the vertex's
 * own BSDF applied to the cached irradiance (RadianceCache/RadianceCache.hlsli, Update.hlsl).
 */
class FRT_CORE_API CRadianceCache
{
public:
	CRadianceCache () = default;
	CRadianceCache (const CRadianceCache&) = delete;
	CRadianceCache& operator= (const CRadianceCache&) = delete;

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();

	// 64-bit atomics on descriptor-heap buffers are required for the key inserts.
	bool IsSupported () const { return bSupported; }
	bool IsActive () const { return Settings.bEnabled && bSupported && Keys.IsValid(); }

	// Allocates / clears as the settings require and fills the shader parameters. Call every
	// frame in real-time mode before Update(); Out.Flags stays 0 while the cache is off.
	void Prepare (CRenderContext& Context, gpu::SRadianceCacheParams& Out);

	// Update + resolve. Constants are the path tracer's constants for this frame (Cache filled
	// by Prepare); the update kernel shares the path tracer's estimator code and settings.
	void Update (CRenderContext& Context, const gpu::SPathTracerConstants& Constants, uint32 RenderWidth, uint32 RenderHeight);

	void RequestClear () { bNeedsClear = true; }
	// The cache is not maintained in reference mode: start over when real time resumes.
	void MarkIdle () { bWasActive = false; }
	void DrawSettingsUi ();

	SRadianceCacheSettings Settings;

private:
	void Allocate (uint32 CapacityLog2);
	void Release ();
	void Clear (CRenderContext& Context, const gpu::SRadianceCacheParams& Params);

	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	rhi::HPipeline UpdatePipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline ResolvePipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline ClearPipeline = rhi::InvalidPipelineHandle;
	rhi::SGpuBuffer Keys;
	rhi::SGpuBuffer Accumulation;
	rhi::SGpuBuffer Resolved;
	uint32 AllocatedCapacityLog2 = 0u;
	float ClearedCellScale = 0.0f;
	uint64 LastPreparedFrame = 0u;       // Context.FrameNumber of the last Prepare (gap = clear)
	bool bSupported = false;
	bool bNeedsClear = true;
	bool bWasActive = false;
};
}
