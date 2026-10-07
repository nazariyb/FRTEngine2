#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "Graphics/Render/RHI/DescriptorHeaps.h"
#include "Graphics/Render/RHI/GpuResources.h"


namespace frt::graphics::rhi
{
// CPU-visible slice of an upload buffer. Valid until the frame (or staging chunk) recycles it.
struct SUploadAllocation
{
	uint8* Cpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS Gpu = 0u;
	ID3D12Resource* Resource = nullptr;
	uint64 Offset = 0u;
	uint64 Size = 0u;
};


/**
 * Linear allocator over one persistently mapped UPLOAD buffer per frame in flight. Used for
 * everything that is rewritten every frame: constant buffers, instance descriptors, light
 * lists. Reset when its frame slot is reused, after the fence wait made that safe.
 */
class FRT_CORE_API CUploadRing
{
public:
	void Init (ID3D12Device* Device, uint64 BytesPerFrame);
	void Shutdown ();
	void BeginFrame (uint32 FrameSlot);

	SUploadAllocation Allocate (uint64 Size, uint64 Alignment = 256u);

	template <typename T>
	SUploadAllocation Push (const T& Value, uint64 Alignment = 256u)
	{
		SUploadAllocation alloc = Allocate(sizeof(T), Alignment);
		memcpy(alloc.Cpu, &Value, sizeof(T));
		return alloc;
	}

	SUploadAllocation PushBytes (const void* Data, uint64 Size, uint64 Alignment = 16u);

	uint64 GetUsedThisFrame () const { return Cursor; }
	uint64 GetCapacityPerFrame () const { return BytesPerFrame; }

private:
	ComPtr<ID3D12Resource> Buffers[MaxFramesInFlight];
	uint8* Mapped[MaxFramesInFlight] = {};
	uint64 BytesPerFrame = 0u;
	uint64 Cursor = 0u;
	uint32 FrameSlot = 0u;
	std::mutex Mutex;
};


// Accumulates resource barriers and flushes them in one ResourceBarrier call.
class FRT_CORE_API CBarrierBatch
{
public:
	void Transition (SGpuTexture& Texture, D3D12_RESOURCE_STATES After);
	void Transition (SGpuBuffer& Buffer, D3D12_RESOURCE_STATES After);
	void Transition (ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After,
		uint32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	void Uav (const SGpuTexture& Texture);
	void Uav (const SGpuBuffer& Buffer);
	void Uav (ID3D12Resource* Resource);
	void Flush (ID3D12GraphicsCommandList* CommandList);
	bool IsEmpty () const { return Barriers.empty(); }

private:
	std::vector<D3D12_RESOURCE_BARRIER> Barriers;
};


/**
 * Owner of GPU object lifetimes and of the bindless heap.
 *
 * Nothing a submitted command list may still touch is released immediately: DeferRelease
 * parks the reference until the fence value of the frame currently being recorded has been
 * reached. CRenderer drives it - BeginFrame(fence value this frame will signal) and
 * ProcessDeferredReleases(completed fence value) after every fence wait.
 */
class FRT_CORE_API CDevice
{
public:
	void Init (ID3D12Device5* Device, ID3D12CommandQueue* Queue, const SDeviceCaps& Caps);
	void Shutdown ();

	ID3D12Device5* GetDevice () const { return Device; }
	ID3D12CommandQueue* GetQueue () const { return Queue; }
	const SDeviceCaps& GetCaps () const { return Caps; }

	CBindlessHeap& GetBindlessHeap () { return BindlessHeap; }
	CCpuDescriptorHeap& GetRtvHeap () { return RtvHeap; }
	CCpuDescriptorHeap& GetDsvHeap () { return DsvHeap; }
	CUploadRing& GetUploadRing () { return UploadRing; }

	// Frame bookkeeping.
	void BeginFrame (uint32 FrameSlot, uint64 SubmissionFenceValue);
	void ProcessDeferredReleases (uint64 CompletedFenceValue);
	uint64 GetSubmissionFenceValue () const { return SubmissionFence; }
	// After a full queue flush between BeginFrame and the frame's signal: the value BeginFrame
	// set has just been signalled by the flush, so releases deferred from here on must wait for
	// the next one instead (see CRenderer::FlushCommandQueue).
	void SetSubmissionFenceValue (uint64 Value) { SubmissionFence = Value; }
	uint32 GetFrameSlot () const { return FrameSlot; }

	// Lifetime.
	void DeferRelease (ComPtr<IUnknown> Object);
	void DeferRelease (ComPtr<ID3D12Resource> Resource);
	void DeferFreeDescriptor (uint32 BindlessIndex);
	void DeferFreeRtv (uint32 Index);
	void DeferFreeDsv (uint32 Index);
	void Destroy (SGpuBuffer& Buffer);
	void Destroy (SGpuTexture& Texture);

	// Creation. Descriptors (SRV/UAV/RTV/DSV) are created as requested by the desc.
	SGpuBuffer CreateBuffer (const SBufferDesc& Desc);
	SGpuTexture CreateTexture (const STextureDesc& Desc);

	// View writers into explicit bindless slots.
	void WriteBufferSrv (uint32 Index, ID3D12Resource* Resource, EBufferViewKind Kind, uint64 Size, uint32 Stride,
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN, uint64 FirstByte = 0u);
	void WriteBufferUav (uint32 Index, ID3D12Resource* Resource, EBufferViewKind Kind, uint64 Size, uint32 Stride,
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN);
	void WriteAccelerationStructureSrv (uint32 Index, D3D12_GPU_VIRTUAL_ADDRESS Location);

	// Staging uploads. Records copies into CommandList; the staging memory is recycled once the
	// GPU has executed the frame they were recorded in. Final states are set on the objects.
	void UploadBuffer (ID3D12GraphicsCommandList* CommandList, SGpuBuffer& Destination, const void* Data,
		uint64 Size, uint64 DestinationOffset, D3D12_RESOURCE_STATES FinalState);
	// Data holds every subresource (mip-major) tightly packed with RowPitch = width * bpp.
	void UploadTexture (ID3D12GraphicsCommandList* CommandList, SGpuTexture& Destination,
		const void* const* MipData, uint32 MipCount, D3D12_RESOURCE_STATES FinalState);
	SUploadAllocation AllocateStaging (uint64 Size, uint64 Alignment);

	uint64 GetStagingBytesThisFrame () const { return StagingBytesThisFrame; }

private:
	struct SPendingRelease
	{
		uint64 FenceValue = 0u;
		ComPtr<IUnknown> Object;
	};

	struct SPendingDescriptor
	{
		uint64 FenceValue = 0u;
		uint32 Index = InvalidDescriptorIndex;
		uint8 Heap = 0u; // 0 bindless, 1 rtv, 2 dsv
	};

	struct SStagingChunk
	{
		ComPtr<ID3D12Resource> Buffer;
		uint8* Mapped = nullptr;
		uint64 Size = 0u;
		uint64 Cursor = 0u;
		uint64 KeepAliveFence = 0u;
	};

	ID3D12Device5* Device = nullptr;
	ID3D12CommandQueue* Queue = nullptr;
	SDeviceCaps Caps;

	CBindlessHeap BindlessHeap;
	CCpuDescriptorHeap RtvHeap;
	CCpuDescriptorHeap DsvHeap;
	CUploadRing UploadRing;

	std::mutex ReleaseMutex;
	std::deque<SPendingRelease> PendingReleases;
	std::deque<SPendingDescriptor> PendingDescriptors;

	std::mutex StagingMutex;
	SStagingChunk CurrentStaging;
	uint64 StagingBytesThisFrame = 0u;

	uint64 SubmissionFence = 1u;
	uint32 FrameSlot = 0u;
};
}
