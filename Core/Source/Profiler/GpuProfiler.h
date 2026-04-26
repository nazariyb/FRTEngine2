#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"


namespace frt::profiler
{
struct SGpuScopeResult
{
	const char* Name = nullptr;
	double DurationMs = 0.0;
};


// D3D12 timestamp-query based GPU profiler.
//
// Lifecycle per frame:
//   BeginFrame()       — bumps ring index, reads back the slot we're about to overwrite
//                        (which holds results from FrameCount frames ago, guaranteed done).
//   BeginScope/EndScope — records timestamp queries on the command list.
//   ResolveFrame()     — emits a ResolveQueryData call so the GPU writes results into the
//                        readback buffer for this frame slot.
//
// Read back results via GetLastResults() — the most recent frame whose data has been read.
class FRT_CORE_API CGpuProfiler
{
public:
	static CGpuProfiler* GetCurrent ();

	bool Init (
		ID3D12Device* Device,
		ID3D12CommandQueue* DirectQueue,
		uint32 MaxScopesPerFrame,
		uint32 FrameCount);
	void Shutdown ();

	void BeginFrame ();

	// Returns scope index used to pair with EndScope. -1 if profiler is full this frame.
	int32 BeginScope (ID3D12GraphicsCommandList* CmdList, const char* Name);
	void EndScope (ID3D12GraphicsCommandList* CmdList, int32 ScopeIndex);

	// Emits ResolveQueryData. Call once per frame after all scopes are recorded.
	void ResolveFrame (ID3D12GraphicsCommandList* CmdList);

	const TArray<SGpuScopeResult>& GetLastResults () const { return LastResults; }

private:
	void ReadbackFrame (uint32 FrameSlot);

	struct SFrameData
	{
		TArray<const char*> ScopeNames;
		uint32 ScopeCount = 0u;
		bool bRecorded = false;
	};

	Microsoft::WRL::ComPtr<ID3D12QueryHeap> QueryHeap;
	TArray<Microsoft::WRL::ComPtr<ID3D12Resource>> ReadbackBuffers;
	TArray<SFrameData> Frames;

	TArray<SGpuScopeResult> LastResults;

	uint64 TimestampFrequency = 0u;
	uint32 MaxScopesPerFrame = 0u;
	uint32 FrameCount = 0u;
	uint32 CurrentFrame = 0u;
	bool bWarmedUp = false;
};
}
