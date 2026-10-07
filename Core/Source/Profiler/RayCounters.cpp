#include "Profiler/RayCounters.h"

#include <cstring>

#include "Asserts.h"


namespace frt::profiler
{
static constexpr uint64 kCounterBytes = CRayCounters::GetBufferBytes();

bool CRayCounters::Init (ID3D12Device* Device, uint32 InFrameCount)
{
	if (!Device || InFrameCount == 0u)
	{
		return false;
	}

	FrameCount = InFrameCount;
	CurrentFrame = FrameCount - 1u; // first BeginFrame wraps to 0
	bWarmedUp = false;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kCounterBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	// Buffers are created in COMMON (the runtime ignores anything else) and promoted to
	// UNORDERED_ACCESS on first use; the explicit transitions below start from UAV.
	if (FAILED(Device->CreateCommittedResource(
			&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(CounterBuffer.GetAddressOf()))))
	{
		return false;
	}

	// All-zero upload buffer used as the clear source (CopyBufferRegion).
	D3D12_RESOURCE_DESC zeroDesc = bufDesc;
	zeroDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	if (FAILED(Device->CreateCommittedResource(
			&uploadHeap, D3D12_HEAP_FLAG_NONE, &zeroDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(ZeroBuffer.GetAddressOf()))))
	{
		Shutdown();
		return false;
	}
	{
		void* mapped = nullptr;
		const D3D12_RANGE noRead = { 0u, 0u };
		if (SUCCEEDED(ZeroBuffer->Map(0, &noRead, &mapped)) && mapped)
		{
			std::memset(mapped, 0, static_cast<size_t>(kCounterBytes));
			ZeroBuffer->Unmap(0, nullptr);
		}
	}

	D3D12_RESOURCE_DESC rbDesc = bufDesc;
	rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_HEAP_PROPERTIES readbackHeap = {};
	readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

	ReadbackBuffers.Clear();
	FrameRecorded.Clear();
	for (uint32 i = 0; i < FrameCount; ++i)
	{
		Microsoft::WRL::ComPtr<ID3D12Resource> rb;
		if (FAILED(Device->CreateCommittedResource(
				&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				IID_PPV_ARGS(rb.GetAddressOf()))))
		{
			Shutdown();
			return false;
		}
		ReadbackBuffers.Add(rb);
		FrameRecorded.Add(false);
	}

	return true;
}

void CRayCounters::Shutdown ()
{
	CounterBuffer.Reset();
	ZeroBuffer.Reset();
	ReadbackBuffers.Clear();
	FrameRecorded.Clear();
	FrameCount = 0u;
}

D3D12_GPU_VIRTUAL_ADDRESS CRayCounters::GetGpuVa () const
{
	return CounterBuffer ? CounterBuffer->GetGPUVirtualAddress() : 0u;
}

void CRayCounters::Barrier (
	ID3D12GraphicsCommandList* CmdList,
	D3D12_RESOURCE_STATES Before,
	D3D12_RESOURCE_STATES After)
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = CounterBuffer.Get();
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = Before;
	b.Transition.StateAfter = After;
	CmdList->ResourceBarrier(1, &b);
}

void CRayCounters::BeginFrame ()
{
	if (FrameCount == 0u)
	{
		return;
	}

	CurrentFrame = (CurrentFrame + 1u) % FrameCount;

	if (bWarmedUp)
	{
		ReadbackFrame(CurrentFrame);
	}
	bWarmedUp = bWarmedUp || (CurrentFrame + 1u == FrameCount);

	FrameRecorded[CurrentFrame] = false;
}

void CRayCounters::ClearForFrame (ID3D12GraphicsCommandList* CmdList)
{
	if (!CmdList || !CounterBuffer)
	{
		return;
	}
	// Counter buffer lives in UAV state; copy zero in, return to UAV for shader writes. The very
	// first clear starts from COMMON (creation state); buffers promote implicitly from COMMON.
	Barrier(CmdList, bFirstClear ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
	bFirstClear = false;
	CmdList->CopyBufferRegion(CounterBuffer.Get(), 0, ZeroBuffer.Get(), 0, kCounterBytes);
	Barrier(CmdList, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void CRayCounters::ResolveFrame (ID3D12GraphicsCommandList* CmdList)
{
	if (!CmdList || !CounterBuffer || FrameCount == 0u)
	{
		return;
	}
	Barrier(CmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	CmdList->CopyBufferRegion(ReadbackBuffers[CurrentFrame].Get(), 0, CounterBuffer.Get(), 0, kCounterBytes);
	Barrier(CmdList, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	FrameRecorded[CurrentFrame] = true;
}

void CRayCounters::ReadbackFrame (uint32 FrameSlot)
{
	if (!FrameRecorded[FrameSlot])
	{
		return;
	}

	const D3D12_RANGE readRange = { 0u, static_cast<SIZE_T>(kCounterBytes) };
	void* mapped = nullptr;
	if (FAILED(ReadbackBuffers[FrameSlot]->Map(0, &readRange, &mapped)) || !mapped)
	{
		return;
	}

	const uint64* src = static_cast<const uint64*>(mapped);
	for (uint32 i = 0; i < RC_Count; ++i)
	{
		LastTotals.Values[i] = src[i];
	}

	const D3D12_RANGE writeRange = { 0u, 0u };
	ReadbackBuffers[FrameSlot]->Unmap(0, &writeRange);
}
}
