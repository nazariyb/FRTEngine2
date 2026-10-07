#pragma once

#include <mutex>
#include <vector>

#include "Graphics/Render/RHI/RHICommon.h"


namespace frt::graphics::rhi
{
/**
 * The one shader-visible CBV/SRV/UAV heap. Every SRV/UAV a shader can see lives here and is
 * addressed by index through SM 6.6 ResourceDescriptorHeap[], so no pass needs its own
 * descriptor tables and the heap is bound once per command list.
 *
 * Layout: [0, PersistentCapacity) persistent slots handed out from a free list, followed by
 * MaxFramesInFlight transient rings of TransientPerFrame slots each. Transient slots are valid
 * for the frame that allocated them and are recycled when that frame slot comes round again,
 * which the fence wait in BeginFrame guarantees is safe.
 *
 * Persistent slots must be returned through CDeferredRelease (never Free() directly while a
 * submitted command list may still reference them).
 */
class FRT_CORE_API CBindlessHeap
{
public:
	void Init (ID3D12Device* Device, uint32 PersistentCapacity, uint32 TransientPerFrame);
	void Shutdown ();

	uint32 AllocatePersistent ();
	void FreePersistent (uint32 Index);

	void BeginFrame (uint32 FrameSlot);
	// Returns the first of Count consecutive transient slots.
	uint32 AllocateTransient (uint32 Count = 1u);

	D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle (uint32 Index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle (uint32 Index) const;
	ID3D12DescriptorHeap* GetHeap () const { return Heap.Get(); }

	uint32 GetPersistentInUse () const { return PersistentInUse; }
	uint32 GetPersistentCapacity () const { return PersistentCapacity; }

	// Descriptor writers. Each writes into an already-allocated slot.
	void WriteNullTexture2DSrv (uint32 Index, DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM);

private:
	ComPtr<ID3D12DescriptorHeap> Heap;
	ID3D12Device* Device = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuStart = {};
	D3D12_GPU_DESCRIPTOR_HANDLE GpuStart = {};
	uint32 Increment = 0u;

	uint32 PersistentCapacity = 0u;
	uint32 TransientPerFrame = 0u;
	uint32 PersistentInUse = 0u;
	std::vector<uint32> FreeList;
	uint32 NextFresh = 0u;

	uint32 CurrentFrameSlot = 0u;
	uint32 TransientCursor = 0u;

	std::mutex Mutex;
};


/**
 * CPU-only heap for RTV/DSV descriptors with a free list. Not shader visible, so slots can be
 * rewritten or freed as soon as the CPU stops recording commands that reference them.
 */
class FRT_CORE_API CCpuDescriptorHeap
{
public:
	void Init (ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 Capacity);
	void Shutdown ();

	uint32 Allocate ();
	void Free (uint32 Index);
	D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle (uint32 Index) const;

private:
	ComPtr<ID3D12DescriptorHeap> Heap;
	D3D12_CPU_DESCRIPTOR_HANDLE CpuStart = {};
	uint32 Increment = 0u;
	uint32 Capacity = 0u;
	uint32 NextFresh = 0u;
	std::vector<uint32> FreeList;
};
}
