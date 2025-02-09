#pragma once

#include <vector>

#include "CoreTypes.h"
#include "RenderResourceAllocators.h"

namespace frt::graphics
{
	template<typename TData>
	struct SConstantBuffer
	{
		static constexpr uint64 DataSize = AlignAddress(sizeof(TData), 256);

		ID3D12Resource* GpuResource = nullptr;
		std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> DescriptorHeapHandleGpu;

		uint64 UploadOffset = 0;
		uint16 ObjectCount = 0;

		SConstantBuffer() = default;
		SConstantBuffer(
			ID3D12Device* Device,
			DX12_Arena& BufferArena,
			DX12_DescriptorHeap& DescriptorHeap,
			uint16 InObjectCount = 1);

		void CopyBunch(TData* Data, DX12_UploadArena& UploadArena);
		void Upload(DX12_UploadArena& UploadArena, ID3D12GraphicsCommandList* CommandList);
	};
}


namespace frt::graphics
{
	template <typename TData>
	SConstantBuffer<TData>::SConstantBuffer(
		ID3D12Device* Device,
		DX12_Arena& BufferArena,
		DX12_DescriptorHeap& DescriptorHeap,
		uint16 InObjectCount)
		: ObjectCount(InObjectCount)
	{
		D3D12_RESOURCE_DESC cbDesc = {};
		cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cbDesc.Width = DataSize * ObjectCount;
		cbDesc.Height = 1;
		cbDesc.DepthOrArraySize = 1;
		cbDesc.MipLevels = 1;
		cbDesc.Format = DXGI_FORMAT_UNKNOWN;
		cbDesc.SampleDesc.Count = 1;
		cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		GpuResource = BufferArena.Allocate(cbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);

		DescriptorHeapHandleGpu.resize(ObjectCount);

		for (uint16 i = 0 ; i < ObjectCount ; ++i)
		{
			const D3D12_CONSTANT_BUFFER_VIEW_DESC cbViewDesc
			{
				.BufferLocation = GpuResource->GetGPUVirtualAddress() + DataSize * i,
				.SizeInBytes = DataSize
			};

			D3D12_CPU_DESCRIPTOR_HANDLE cpuDesc = {};
			DescriptorHeap.Allocate(&cpuDesc, &DescriptorHeapHandleGpu[i]);

			Device->CreateConstantBufferView(&cbViewDesc, cpuDesc);
		}
	}

	template <typename TData>
	void SConstantBuffer<TData>::CopyBunch(TData* Data, DX12_UploadArena& UploadArena)
	{
		if (Data)
		{
			uint8* dest = UploadArena.Allocate(DataSize * ObjectCount, &UploadOffset);
			memcpy(dest, Data, DataSize * ObjectCount);
		}
	}

	template <typename TData>
	void SConstantBuffer<TData>::Upload(
		DX12_UploadArena& UploadArena,
		ID3D12GraphicsCommandList* CommandList)
	{
		{
			D3D12_RESOURCE_BARRIER resourceBarrier = {};
			resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			resourceBarrier.Transition.pResource = GpuResource;
			resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			CommandList->ResourceBarrier(1, &resourceBarrier);
		}

		CommandList->CopyBufferRegion(
			GpuResource,
			0,
			UploadArena.GetGPUBuffer(),
			UploadOffset,
			DataSize * ObjectCount);
		UploadOffset = 0;

		{
			D3D12_RESOURCE_BARRIER resourceBarrier = {};
			resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			resourceBarrier.Transition.pResource = GpuResource;
			resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			CommandList->ResourceBarrier(1, &resourceBarrier);
		}
	}
}
