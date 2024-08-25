#include "GraphicsUtility.h"

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

	ID3D12Resource* DX12_Arena::Allocate
	(const D3D12_RESOURCE_DESC& ResourceDesc,
		D3D12_RESOURCE_STATES InitialState,
		const D3D12_CLEAR_VALUE& ClearValue)
	{
		ID3D12Resource* resource = nullptr;

		D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = _device->GetResourceAllocationInfo(0,  1, &ResourceDesc);
		uint64 alignedAddress = AlignAddress(_sizeUsed, allocationInfo.Alignment);
		//assert(alignedAddress + allocationInfo.SizeInBytes < _sizeTotal);

		/*Throw*/_device->CreatePlacedResource(
			_heap, alignedAddress, &ResourceDesc, InitialState, &ClearValue, IID_PPV_ARGS(&resource));

		_sizeUsed = alignedAddress + allocationInfo.SizeInBytes;

		return resource;
	}


	DX12_DescriptorHeap::DX12_DescriptorHeap(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 Num)
		: _stepSize(0)
		, _maxNum(Num)
		, _currNum(0)
		, _heap(nullptr)
	{
		_stepSize = Device->GetDescriptorHandleIncrementSize(Type);

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = Type;
		heapDesc.NumDescriptors = Num;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		/*Throw*/Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap));
	}

	DX12_DescriptorHeap::~DX12_DescriptorHeap()
	{
		_heap = nullptr;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DX12_DescriptorHeap::Allocate()
	{
		//assert(_currNum < _maxNum);

		D3D12_CPU_DESCRIPTOR_HANDLE descHandle = _heap->GetCPUDescriptorHandleForHeapStart();
		descHandle.ptr += _stepSize * _currNum;
		_currNum++;
		return descHandle;
	}
}
