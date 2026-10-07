#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/RenderSettings.h"
#include "Shared/PathTracerData.h"

namespace frt::graphics
{

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
	void DrawSettingsUi (SRadianceCacheSettings& Edit);

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
