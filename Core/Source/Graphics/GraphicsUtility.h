#pragma once

#include <d3d12.h>

#include "CoreTypes.h"


struct ID3D12Heap;

namespace frt::graphics
{

	static constexpr uint64 KiloByte = 1024LL;
	static constexpr uint64 MegaByte = KiloByte * 1024LL;
	static constexpr uint64 GigaByte = MegaByte * 1024LL;
	static constexpr uint64 TeraByte = GigaByte * 1024LL;

	struct DX12_Arena
	{
		DX12_Arena() : _sizeTotal(0), _sizeUsed(0), _heap(nullptr), _device(nullptr) {}
		DX12_Arena(DX12_Arena&&) = default;
		DX12_Arena& operator=(DX12_Arena&&) = default;
		DX12_Arena(const DX12_Arena&) = delete;
		DX12_Arena& operator=(const DX12_Arena&) = delete;

		DX12_Arena(ID3D12Device* Device, D3D12_HEAP_TYPE HeapType, uint64 InSize, D3D12_HEAP_FLAGS Flags);
		~DX12_Arena();

		ID3D12Resource* Allocate(
			const D3D12_RESOURCE_DESC& ResourceDesc,
			D3D12_RESOURCE_STATES InitialState,
			const D3D12_CLEAR_VALUE& ClearValue);

	private:
		uint64 _sizeTotal;
		uint64 _sizeUsed;
		ID3D12Heap* _heap;
		ID3D12Device* _device;
	};

	struct DX12_DescriptorHeap
	{
		DX12_DescriptorHeap() : _stepSize(0), _maxNum(0), _currNum(0), _heap(nullptr) {}
		DX12_DescriptorHeap(DX12_DescriptorHeap&&) = default;
		DX12_DescriptorHeap& operator=(DX12_DescriptorHeap&&) = default;
		DX12_DescriptorHeap(const DX12_DescriptorHeap&) = delete;
		DX12_DescriptorHeap& operator=(const DX12_DescriptorHeap&) = delete;

		DX12_DescriptorHeap(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 Num);
		~DX12_DescriptorHeap();

		D3D12_CPU_DESCRIPTOR_HANDLE Allocate();

	private:
		uint64 _stepSize;
		uint64 _maxNum;
		uint64 _currNum;
		ID3D12DescriptorHeap* _heap;
	};

	struct DX12_UploadArena
	{
		DX12_UploadArena() : _sizeTotal(0), _sizeUsed(0), _gpuBuffer(nullptr), _cpuBuffer(nullptr) {}
		DX12_UploadArena(DX12_UploadArena&&) = default;
		DX12_UploadArena& operator=(DX12_UploadArena&&) = default;
		DX12_UploadArena(const DX12_UploadArena&) = delete;
		DX12_UploadArena& operator=(const DX12_UploadArena&) = delete;

		DX12_UploadArena(ID3D12Device* Device, uint64 Size);
		~DX12_UploadArena();

		uint8* Allocate(uint64 Size);

	private:
		uint64 _sizeTotal;
		uint64 _sizeUsed;
		ID3D12Resource* _gpuBuffer;
		uint8* _cpuBuffer;
	};

	inline uint64 AlignAddress(uint64 Location, uint64 Alignment)
	{
		return (Location + Alignment - 1) & ~(Alignment - 1);
	}

}
