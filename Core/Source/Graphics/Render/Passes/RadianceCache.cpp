#include "Graphics/Render/Passes/RadianceCache.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "Profiler/Profiler.h"


namespace frt::graphics
{
namespace
{
constexpr uint32 kKeyBytes = 8u;
constexpr uint32 kAccumulationBytes = 16u;
constexpr uint32 kResolvedBytes = 16u;
constexpr uint32 kMinCapacityLog2 = 16u;
constexpr uint32 kMaxCapacityLog2 = 24u;
constexpr uint32 kResolveGroupSize = 64u;
// Resolve / clear dispatch a 2D grid (kRcResolveRowEntries in Resolve.hlsl = groups per row *
// group size): 2^22 entries in one row would exceed D3D12's 65535 groups per dimension.
constexpr uint32 kResolveGroupsPerRow = 1024u;
static_assert(((1u << kMinCapacityLog2) / kResolveGroupSize) % kResolveGroupsPerRow == 0u, "capacity must fill whole rows");

// One thread per hash entry.
void DispatchPerEntry (CRenderContext& Context, rhi::HPipeline Pipeline, uint32 Entries)
{
	const uint32 groups = rhi::DivideRoundUp(Entries, kResolveGroupSize);
	Context.Dispatch(Pipeline, kResolveGroupsPerRow, rhi::DivideRoundUp(groups, kResolveGroupsPerRow), 1u);
}

rhi::SGpuBuffer MakeRawUavBuffer (rhi::CDevice& Device, uint64 Size, FStringView Name)
{
	rhi::SBufferDesc desc;
	desc.Size = Size;
	desc.UavKind = rhi::EBufferViewKind::Raw;
	desc.Name = Name;
	return Device.CreateBuffer(desc);
}
}


void CRadianceCache::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines)
{
	Device = &InDevice;
	Pipelines = &InPipelines;

	// The key inserts are 64-bit compare-exchanges on a bindless (descriptor heap) buffer.
	D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11 = {};
	bSupported = SUCCEEDED(Device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof(options11))) &&
		options11.AtomicInt64OnDescriptorHeapResourceSupported && Device->GetCaps().SupportsShaderModel(D3D_SHADER_MODEL_6_6);
	if (!bSupported)
	{
		std::fprintf(stderr, "[radiance cache] 64-bit atomics on descriptor heap resources unsupported: cache disabled\n");
		return;
	}

	UpdatePipeline = Pipelines->CreateCompute("RadianceCache_Update", MakeShaderDesc("RadianceCache/Update.hlsl", "main", "cs_6_6"));
	ResolvePipeline = Pipelines->CreateCompute("RadianceCache_Resolve", MakeShaderDesc("RadianceCache/Resolve.hlsl", "Resolve", "cs_6_6"));
	ClearPipeline = Pipelines->CreateCompute("RadianceCache_Clear", MakeShaderDesc("RadianceCache/Resolve.hlsl", "Clear", "cs_6_6"));
}

void CRadianceCache::Release ()
{
	for (rhi::SGpuBuffer* b : { &Keys, &Accumulation, &Resolved })
	{
		if (b->IsValid())
		{
			Device->Destroy(*b);
		}
	}
	AllocatedCapacityLog2 = 0u;
}

void CRadianceCache::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	Release();
	Device = nullptr;
}

void CRadianceCache::Allocate (uint32 CapacityLog2)
{
	Release();
	const uint64 capacity = 1ull << CapacityLog2;
	Keys = MakeRawUavBuffer(*Device, capacity * kKeyBytes, "RadianceCache.Keys");
	Accumulation = MakeRawUavBuffer(*Device, capacity * kAccumulationBytes, "RadianceCache.Accumulation");
	Resolved = MakeRawUavBuffer(*Device, capacity * kResolvedBytes, "RadianceCache.Resolved");
	AllocatedCapacityLog2 = CapacityLog2;
	bNeedsClear = true;
}

void CRadianceCache::Prepare (CRenderContext& Context, gpu::SRadianceCacheParams& Out)
{
	Out = {};
	Out.KeyBuffer = Out.AccumulationBuffer = Out.ResolvedBuffer = FRT_INVALID_INDEX;
	if (!bSupported || !Settings.bEnabled)
	{
		bWasActive = false;
		return;
	}

	const uint32 capacityLog2 = (std::clamp)(Settings.CapacityLog2, kMinCapacityLog2, kMaxCapacityLog2);
	if (capacityLog2 != AllocatedCapacityLog2 || !Keys.IsValid())
	{
		Allocate(capacityLog2);
	}
	// Keys encode positions in cells of the current scale: a new scale invalidates everything.
	// Re-enabling after a pause would serve arbitrarily old radiance: start over too.
	const float cellScale = (std::max)(Settings.CellScale, 1e-3f);
	// Frames without a real-time dispatch (raster mode, a window that was not rendering) leave
	// the cells frozen while the lighting may have moved on: also start over.
	if (cellScale != ClearedCellScale || !bWasActive || Context.FrameNumber != LastPreparedFrame + 1u)
	{
		bNeedsClear = true;
	}
	bWasActive = true;
	LastPreparedFrame = Context.FrameNumber;

	Out.CameraPosition = Context.Frame->CameraPosition;
	Out.CellScale = cellScale;
	Out.KeyBuffer = Keys.Uav;
	Out.AccumulationBuffer = Accumulation.Uav;
	Out.ResolvedBuffer = Resolved.Uav;
	Out.CapacityMask = static_cast<uint32>((1ull << AllocatedCapacityLog2) - 1u);
	Out.RoughnessThreshold = Settings.RoughnessThreshold;
	Out.FootprintScale = (std::max)(Settings.FootprintScale, 0.0f);
	Out.MinSampleWeight = (std::max)(Settings.MinSampleWeight, 1.0f);
	Out.SampleClamp = (std::max)(Settings.SampleClamp, 1e-3f);
	Out.UpdateTileSize = (std::clamp)(Settings.UpdateTileSize, 1u, 64u);
	Out.AccumulationFrames = (std::clamp)(Settings.AccumulationFrames, 1u, 4096u);
	Out.StaleFrames = (std::clamp)(Settings.StaleFrames, 1u, 65535u);
	Out.FeedbackMaxDepth = (std::clamp)(Settings.FeedbackMaxDepth, 1u, 16u);
	Out.SpecularShareMax = (std::clamp)(Settings.SpecularShareMax, 0.0f, 1.0f);
	Out.Flags = gpu::RadianceCacheFlag_Query | gpu::RadianceCacheFlag_Valid;
	if (Settings.bFeedback) { Out.Flags |= gpu::RadianceCacheFlag_Feedback; }
	if (Settings.bJitter) { Out.Flags |= gpu::RadianceCacheFlag_Jitter; }

	for (rhi::SGpuBuffer* b : { &Keys, &Accumulation, &Resolved })
	{
		Context.Barriers.Transition(*b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	if (bNeedsClear)
	{
		Clear(Context, Out);
		ClearedCellScale = cellScale;
		bNeedsClear = false;
	}
}

void CRadianceCache::Clear (CRenderContext& Context, const gpu::SRadianceCacheParams& Params)
{
	FRT_GPU_SCOPE(Context.CommandList, "RC_Clear");
	gpu::SPathTracerConstants constants = {};
	constants.Cache = Params;
	Context.PushPassConstants(constants);
	DispatchPerEntry(Context, ClearPipeline, Params.CapacityMask + 1u);
	for (rhi::SGpuBuffer* b : { &Keys, &Accumulation, &Resolved })
	{
		Context.Barriers.Uav(*b);
	}
}

void CRadianceCache::Update (CRenderContext& Context, const gpu::SPathTracerConstants& Constants, uint32 RenderWidth, uint32 RenderHeight)
{
	if (!IsActive() || Constants.Cache.Flags == 0u)
	{
		return;
	}
	// Last frame's path tracer read keys / resolved radiance; this frame writes them.
	for (rhi::SGpuBuffer* b : { &Keys, &Accumulation, &Resolved })
	{
		Context.Barriers.Uav(*b);
	}
	Context.PushPassConstants(Constants);

	{
		FRT_GPU_SCOPE(Context.CommandList, "RC_Update");
		const uint32 tile = Constants.Cache.UpdateTileSize;
		const uint32 tilesX = rhi::DivideRoundUp(RenderWidth, tile);
		const uint32 tilesY = rhi::DivideRoundUp(RenderHeight, tile);
		Context.Dispatch(UpdatePipeline, rhi::DivideRoundUp(tilesX, 8u), rhi::DivideRoundUp(tilesY, 8u), 1u);
	}
	for (rhi::SGpuBuffer* b : { &Keys, &Accumulation, &Resolved })
	{
		Context.Barriers.Uav(*b);
	}
	{
		FRT_GPU_SCOPE(Context.CommandList, "RC_Resolve");
		DispatchPerEntry(Context, ResolvePipeline, Constants.Cache.CapacityMask + 1u);
	}
	for (rhi::SGpuBuffer* b : { &Keys, &Accumulation, &Resolved })
	{
		Context.Barriers.Uav(*b);
	}
	Context.FlushBarriers();
}

void CRadianceCache::DrawSettingsUi (SRadianceCacheSettings& Edit)
{
	if (!bSupported)
	{
		ImGui::TextDisabled("Radiance cache unavailable (needs 64-bit atomics on heap resources)");
		return;
	}
	ImGui::Checkbox("Radiance cache", &Edit.bEnabled);
	ImGui::BeginDisabled(!Edit.bEnabled);
	int capacity = static_cast<int>(Edit.CapacityLog2);
	if (ImGui::SliderInt("Capacity (log2 entries)", &capacity, static_cast<int>(kMinCapacityLog2), static_cast<int>(kMaxCapacityLog2)))
	{
		Edit.CapacityLog2 = static_cast<uint32>(capacity);
	}
	ImGui::SliderFloat("Cell size at 1 m", &Edit.CellScale, 0.005f, 0.25f, "%.3f m", ImGuiSliderFlags_Logarithmic);
	int tile = static_cast<int>(Edit.UpdateTileSize);
	if (ImGui::SliderInt("Update tile (1 path / tile^2)", &tile, 1, 16))
	{
		Edit.UpdateTileSize = static_cast<uint32>(tile);
	}
	int accumulation = static_cast<int>(Edit.AccumulationFrames);
	if (ImGui::SliderInt("Accumulation frames", &accumulation, 1, 256))
	{
		Edit.AccumulationFrames = static_cast<uint32>(accumulation);
	}
	int stale = static_cast<int>(Edit.StaleFrames);
	if (ImGui::SliderInt("Stale frames", &stale, 1, 1024))
	{
		Edit.StaleFrames = static_cast<uint32>(stale);
	}
	ImGui::SliderFloat("Roughness threshold", &Edit.RoughnessThreshold, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Footprint scale", &Edit.FootprintScale, 0.0f, 8.0f, "%.2f");
	ImGui::SliderFloat("Min sample weight", &Edit.MinSampleWeight, 1.0f, 256.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Update sample clamp", &Edit.SampleClamp, 1.0f, 16384.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Max specular share", &Edit.SpecularShareMax, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox("Multi-bounce feedback", &Edit.bFeedback);
	int depth = static_cast<int>(Edit.FeedbackMaxDepth);
	if (ImGui::SliderInt("Feedback max depth", &depth, 1, 8))
	{
		Edit.FeedbackMaxDepth = static_cast<uint32>(depth);
	}
	ImGui::Checkbox("Jittered lookups", &Edit.bJitter);
	if (ImGui::Button("Clear cache"))
	{
		bNeedsClear = true;
	}
	ImGui::EndDisabled();
}
}
