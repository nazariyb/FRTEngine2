#pragma once

#include <d3d12.h>

#include "Asserts.h"
#include "CoreTypes.h"
#include "Exception.h"
#include "RenderConstants.h"


struct ID3D12Heap;

namespace frt::graphics
{
	#pragma pack(push, 1)
	struct Vertex
	{
		float position[3];
		float uv[2];
	};
	#pragma pack(pop)

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
			const D3D12_CLEAR_VALUE* ClearValue);

		void Free();

	private:
		uint64 _sizeTotal;
		uint64 _sizeUsed;
		ID3D12Heap* _heap;
		ID3D12Device* _device;
	};

	struct DX12_DescriptorHeap
	{
		friend class Renderer;

		DX12_DescriptorHeap() : _stepSize(0), _maxNum(0), _currNum(0), _heap(nullptr) {}
		DX12_DescriptorHeap(DX12_DescriptorHeap&&) = default;
		DX12_DescriptorHeap& operator=(DX12_DescriptorHeap&&) = default;
		DX12_DescriptorHeap(const DX12_DescriptorHeap&) = delete;
		DX12_DescriptorHeap& operator=(const DX12_DescriptorHeap&) = delete;

		DX12_DescriptorHeap(
			ID3D12Device* Device,
			D3D12_DESCRIPTOR_HEAP_TYPE Type,
			uint32 Num,
			D3D12_DESCRIPTOR_HEAP_FLAGS Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
		~DX12_DescriptorHeap();

		void Allocate(D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle);
		ID3D12DescriptorHeap* GetHeap() const { return _heap; }

	private:
		uint64 _stepSize;
		uint64 _maxNum;
		uint64 _currNum;
		ID3D12DescriptorHeap* _heap;
	};

	template<int NFrameCount = render::constants::FrameBufferCount>
	struct DX12_UploadArena
	{
		DX12_UploadArena()
			: SizeTotal(0)
			, SizeUsed(0)
			, GpuBuffer(nullptr)
			, CpuBuffer(nullptr)
			, CurrentFrame(0)
			, AllocationOffsets{}
		{
		}

		DX12_UploadArena(DX12_UploadArena&&) = default;
		DX12_UploadArena& operator=(DX12_UploadArena&&) = default;
		DX12_UploadArena(const DX12_UploadArena&) = delete;
		DX12_UploadArena& operator=(const DX12_UploadArena&) = delete;

		DX12_UploadArena(ID3D12Device* Device, uint64 SizeForFrame);
		~DX12_UploadArena();

		void BeginFrame(uint8 InCurrentFrame);
		uint8* Allocate(uint64 Size, uint64* OutOffset);
		void Clear();
		uint64 GetSizeUsedAligned() const;

		ID3D12Resource* GetGPUBuffer() const { return GpuBuffer; }

	private:
		uint64 SizeTotal;
		uint64 SizeUsed;
		ID3D12Resource* GpuBuffer;
		uint8* CpuBuffer;

		uint8 CurrentFrame;
		uint64 AllocationOffsets[NFrameCount];
	};

	inline uint64 AlignAddress(uint64 Location, uint64 Alignment)
	{
		return (Location + Alignment - 1) & ~(Alignment - 1);
	}

}

namespace frt::graphics
{
	template <int NFrameCount>
	DX12_UploadArena<NFrameCount>::DX12_UploadArena(ID3D12Device* Device, uint64 SizeForFrame)
		: SizeTotal(SizeForFrame * NFrameCount)
		, SizeUsed(0)
		, GpuBuffer(nullptr)
		, CpuBuffer(nullptr)
		, CurrentFrame(0)
	{
		for (int i = 0; i < NFrameCount; ++i)
		{
			AllocationOffsets[i] = SizeForFrame * i;
		}

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width = SizeTotal;
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		THROW_IF_FAILED(Device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&GpuBuffer)));

		GpuBuffer->Map(0, nullptr, reinterpret_cast<void**>(&CpuBuffer));
	}

	template <int NFrameCount>
	DX12_UploadArena<NFrameCount>::~DX12_UploadArena()
	{
		GpuBuffer->Unmap(0, nullptr);
		GpuBuffer = nullptr;
		CpuBuffer = nullptr;
	}

	template <int NFrameCount>
	void DX12_UploadArena<NFrameCount>::BeginFrame(uint8 InCurrentFrame)
	{
		frt_assert(InCurrentFrame < NFrameCount, "InCurrentFrame out of range");
		CurrentFrame = InCurrentFrame;
		SizeUsed = 0;
	}

	template <int NFrameCount>
	uint8* DX12_UploadArena<NFrameCount>::Allocate(uint64 Size, uint64* OutOffset)
	{
		const uint64 alignedMemory = AlignAddress(SizeUsed, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		frt_assert(alignedMemory + Size < SizeTotal / NFrameCount);

		const uint64 frameOffset = AllocationOffsets[CurrentFrame];
		uint8* result = CpuBuffer + frameOffset + alignedMemory;
		SizeUsed = alignedMemory + Size;

		*OutOffset = frameOffset + alignedMemory;

		return result;
	}

	template <int NFrameCount>
	void DX12_UploadArena<NFrameCount>::Clear()
	{
		SizeUsed = 0;
	}

	template <int NFrameCount>
	uint64 DX12_UploadArena<NFrameCount>::GetSizeUsedAligned() const
	{
		return AlignAddress(SizeUsed, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	}
}
