#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"


namespace frt::profiler
{
// Ray-counter slot layout. Mirror of the indices used in Hit.hlsl / RayGen.hlsl
// (gCounters.InterlockedAdd at slot*4). Keep both sides in sync.
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

	RC_Count               = 8u
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
};
}
