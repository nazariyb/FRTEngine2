#include "Graphics/Render/RHI/DescriptorHeaps.h"


namespace frt::graphics::rhi
{
void CBindlessHeap::Init (ID3D12Device* InDevice, uint32 InPersistentCapacity, uint32 InTransientPerFrame)
{
	frt_assert(InDevice);
	Device = InDevice;
	PersistentCapacity = InPersistentCapacity;
	TransientPerFrame = InTransientPerFrame;

	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = PersistentCapacity + TransientPerFrame * MaxFramesInFlight;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	THROW_IF_FAILED(Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&Heap)));
	SetDebugName(Heap.Get(), L"BindlessHeap");

	CpuStart = Heap->GetCPUDescriptorHandleForHeapStart();
	GpuStart = Heap->GetGPUDescriptorHandleForHeapStart();
	Increment = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	FreeList.clear();
	FreeList.reserve(1024);
	NextFresh = 0u;
	PersistentInUse = 0u;
}

void CBindlessHeap::Shutdown ()
{
	Heap.Reset();
	FreeList.clear();
	Device = nullptr;
}

uint32 CBindlessHeap::AllocatePersistent ()
{
	std::lock_guard lock(Mutex);
	uint32 index = InvalidDescriptorIndex;
	if (!FreeList.empty())
	{
		index = FreeList.back();
		FreeList.pop_back();
	}
	else
	{
		// Running out here means the persistent capacity is too small for the scene; it is a
		// hard configuration error rather than something to limp along with.
		frt_assert(NextFresh < PersistentCapacity);
		if (NextFresh >= PersistentCapacity)
		{
			throw std::runtime_error("Bindless heap: persistent descriptor capacity exhausted");
		}
		index = NextFresh++;
	}
	++PersistentInUse;
	return index;
}

void CBindlessHeap::FreePersistent (uint32 Index)
{
	if (Index == InvalidDescriptorIndex)
	{
		return;
	}
	std::lock_guard lock(Mutex);
	frt_assert(Index < PersistentCapacity);
	frt_assert(PersistentInUse > 0u);
	FreeList.push_back(Index);
	--PersistentInUse;
}

void CBindlessHeap::BeginFrame (uint32 FrameSlot)
{
	frt_assert(FrameSlot < MaxFramesInFlight);
	CurrentFrameSlot = FrameSlot;
	TransientCursor = 0u;
}

uint32 CBindlessHeap::AllocateTransient (uint32 Count)
{
	std::lock_guard lock(Mutex);
	frt_assert(TransientCursor + Count <= TransientPerFrame);
	if (TransientCursor + Count > TransientPerFrame)
	{
		throw std::runtime_error("Bindless heap: transient descriptor ring exhausted this frame");
	}
	const uint32 first = PersistentCapacity + CurrentFrameSlot * TransientPerFrame + TransientCursor;
	TransientCursor += Count;
	return first;
}

D3D12_CPU_DESCRIPTOR_HANDLE CBindlessHeap::GetCpuHandle (uint32 Index) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = CpuStart;
	handle.ptr += static_cast<SIZE_T>(Index) * Increment;
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE CBindlessHeap::GetGpuHandle (uint32 Index) const
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = GpuStart;
	handle.ptr += static_cast<UINT64>(Index) * Increment;
	return handle;
}

void CBindlessHeap::WriteNullTexture2DSrv (uint32 Index, DXGI_FORMAT Format)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = Format;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MipLevels = 1;
	Device->CreateShaderResourceView(nullptr, &desc, GetCpuHandle(Index));
}


void CCpuDescriptorHeap::Init (ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 InCapacity)
{
	Capacity = InCapacity;
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = Type;
	desc.NumDescriptors = Capacity;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	THROW_IF_FAILED(Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&Heap)));
	CpuStart = Heap->GetCPUDescriptorHandleForHeapStart();
	Increment = Device->GetDescriptorHandleIncrementSize(Type);
	NextFresh = 0u;
	FreeList.clear();
}

void CCpuDescriptorHeap::Shutdown ()
{
	Heap.Reset();
	FreeList.clear();
}

uint32 CCpuDescriptorHeap::Allocate ()
{
	if (!FreeList.empty())
	{
		const uint32 index = FreeList.back();
		FreeList.pop_back();
		return index;
	}
	frt_assert(NextFresh < Capacity);
	if (NextFresh >= Capacity)
	{
		throw std::runtime_error("CPU descriptor heap exhausted");
	}
	return NextFresh++;
}

void CCpuDescriptorHeap::Free (uint32 Index)
{
	if (Index != InvalidDescriptorIndex)
	{
		FreeList.push_back(Index);
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE CCpuDescriptorHeap::GetCpuHandle (uint32 Index) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = CpuStart;
	handle.ptr += static_cast<SIZE_T>(Index) * Increment;
	return handle;
}
}
