#include "GraphicsCoreTypes.h"

#include "Asserts.h"

namespace frt::graphics
{

	DX12_Arena::DX12_Arena(
		ID3D12Device* Device, D3D12_HEAP_TYPE HeapType, uint64 InSize, D3D12_HEAP_FLAGS Flags)
		: _sizeTotal(InSize)
		, _sizeUsed(0)
		, _device(Device)
	{
		D3D12_HEAP_DESC heapDesc = {};
		heapDesc.SizeInBytes = _sizeTotal;
		heapDesc.Flags = Flags;
		heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		heapDesc.Properties.Type = HeapType;
		heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		/*Throw*/_device->CreateHeap(&heapDesc, IID_PPV_ARGS(&_heap));
	}

	DX12_Arena::~DX12_Arena()
	{
		_heap = nullptr;
		_device = nullptr;
	}

	ID3D12Resource* DX12_Arena::Allocate(
		const D3D12_RESOURCE_DESC& ResourceDesc,
		D3D12_RESOURCE_STATES InitialState,
		const D3D12_CLEAR_VALUE* ClearValue)
	{
		ID3D12Resource* resource = nullptr;

		D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = _device->GetResourceAllocationInfo(0,  1, &ResourceDesc);
		uint64 alignedAddress = AlignAddress(_sizeUsed, allocationInfo.Alignment);
		frt_assert(alignedAddress + allocationInfo.SizeInBytes < _sizeTotal);

		/*Throw*/_device->CreatePlacedResource(
			_heap, alignedAddress, &ResourceDesc, InitialState, ClearValue, IID_PPV_ARGS(&resource));

		_sizeUsed = alignedAddress + allocationInfo.SizeInBytes;

		return resource;
	}


	DX12_DescriptorHeap::DX12_DescriptorHeap(
		ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 Num, D3D12_DESCRIPTOR_HEAP_FLAGS Flags)
		: _stepSize(0)
		, _maxNum(Num)
		, _currNum(0)
		, _heap(nullptr)
	{
		_stepSize = Device->GetDescriptorHandleIncrementSize(Type);

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = Type;
		heapDesc.NumDescriptors = Num;
		heapDesc.Flags = Flags;
		/*Throw*/Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap));
	}

	DX12_DescriptorHeap::~DX12_DescriptorHeap()
	{
		_heap = nullptr;
	}

	void DX12_DescriptorHeap::Allocate(D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
	{
		frt_assert(_currNum < _maxNum);

		if (OutCpuHandle)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE descHandle = _heap->GetCPUDescriptorHandleForHeapStart();
			descHandle.ptr += _stepSize * _currNum;
			*OutCpuHandle = descHandle;
		}

		if (OutGpuHandle)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE descHandle = _heap->GetGPUDescriptorHandleForHeapStart();
			descHandle.ptr += _stepSize * _currNum;
			*OutGpuHandle = descHandle;
		}

		_currNum++;
	}

	DX12_UploadArena::DX12_UploadArena(ID3D12Device* Device, uint64 Size)
		: _sizeTotal(Size)
		, _sizeUsed(0)
		, _gpuBuffer(nullptr)
		, _cpuBuffer(nullptr)
	{
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width = _sizeTotal;
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		/*Throw*/Device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_gpuBuffer));

		_gpuBuffer->Map(0, nullptr, reinterpret_cast<void**>(&_cpuBuffer));
	}

	DX12_UploadArena::~DX12_UploadArena()
	{
		_gpuBuffer->Unmap(0, nullptr);
		_gpuBuffer = nullptr;
		_cpuBuffer = nullptr;
	}

	uint8* DX12_UploadArena::Allocate(uint64 Size, uint64* OutOffset)
	{
		uint64 alignedMemory = AlignAddress(_sizeUsed, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		frt_assert(alignedMemory + Size < _sizeTotal);

		uint8* result = _cpuBuffer + alignedMemory;
		_sizeUsed = alignedMemory + Size;
		*OutOffset = alignedMemory;

		return result;
	}

	void DX12_UploadArena::Clear()
	{
		_sizeUsed = 0;
	}
}
