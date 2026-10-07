#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"


namespace frt::profiler
{
// Ray-counter slot layout. Mirror of the RC_* defines in Shaders/Common/RayTracing.hlsli.
// Each slot is 64-bit (low word with carry into the high word, slot * 8). Slots 0-8 keep the
// meaning they had in the profiling CSVs; new slots are only ever appended.
enum ERayCounter : uint32
{
	RC_Primary             = 0u, // camera rays
	RC_Bounce              = 1u, // BRDF indirect-bounce rays
	RC_SunShadow           = 2u, // NEE shadow rays toward the sun (Directional)
	RC_SkyShadowAttempted  = 3u, // sky-NEE samples requested (denominator)
	RC_SkyShadowPortalPass = 4u, // survived pre-filter -> TLAS dispatched
	RC_SkyShadowPortalRej  = 5u, // killed by pre-filter (the saving)
	RC_SkyShadowReachedSky = 6u, // dispatched ray escaped -> sky (useful)
	RC_SkyShadowBlocked    = 7u, // dispatched ray hit geometry (wasted TLAS)
	RC_PortalTests         = 8u, // portal-quad tests summed over all sky shadow rays (thesis E[K])
	RC_OtherShadow         = 9u, // NEE shadow rays toward emissive triangles / local lights
	RC_Escaped             = 10u, // bounce rays that left the scene (environment hits)
	RC_RussianRoulette     = 11u, // paths terminated by Russian roulette
	RC_InvalidSample       = 12u, // NaN / Inf contributions dropped

	RC_Count               = 13u
};

struct SRayCounters
{
	uint64 Values[RC_Count] = {};
};


// GPU ray-counter buffer. Shader does atomic InterlockedAdd into a RWByteAddressBuffer
// bound as a root UAV (u1) in raygen + hit signatures.
//
// Per-frame lifecycle (driven by CRenderer):
//   BeginFrame()   — bump ring, read back the slot we're about to overwrite (N frames old)
//   ClearForFrame()— CopyBufferRegion zero -> counter buffer (root UAVs can't ClearUAVUint)
//   ... DispatchRays: shader increments ...
//   ResolveFrame() — copy counter buffer -> this frame's readback slot
//
// GetGpuVa() feeds the SBT records. GetLastTotals() returns the most recent read-back frame.
class FRT_CORE_API CRayCounters
{
public:
	bool Init (ID3D12Device* Device, uint32 FrameCount);
	void Shutdown ();

	void BeginFrame ();
	void ClearForFrame (ID3D12GraphicsCommandList* CmdList);
	void ResolveFrame (ID3D12GraphicsCommandList* CmdList);

	D3D12_GPU_VIRTUAL_ADDRESS GetGpuVa () const;
	ID3D12Resource* GetResource () const { return CounterBuffer.Get(); }
	static constexpr uint64 GetBufferBytes () { return static_cast<uint64>(RC_Count) * sizeof(uint64); }
	const SRayCounters& GetLastTotals () const { return LastTotals; }

private:
	void ReadbackFrame (uint32 FrameSlot);
	void Barrier (ID3D12GraphicsCommandList* CmdList, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After);

	Microsoft::WRL::ComPtr<ID3D12Resource> CounterBuffer; // default heap, UAV
	Microsoft::WRL::ComPtr<ID3D12Resource> ZeroBuffer;    // upload heap, all-zero source for clear
	TArray<Microsoft::WRL::ComPtr<ID3D12Resource>> ReadbackBuffers;
	TArray<bool> FrameRecorded;

	SRayCounters LastTotals;

	uint32 FrameCount = 0u;
	uint32 CurrentFrame = 0u;
	bool bWarmedUp = false;
	bool bFirstClear = true;
};
}
