#pragma once

#include "CoreTypes.h"
#include "GraphicsCoreTypes.h"


namespace frt::graphics
{
	template<typename TData>
	struct SConstantBuffer
	{
		ID3D12Resource* GpuResource = nullptr;
		D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapHandleGpu;

		SConstantBuffer() = default;
		SConstantBuffer(ID3D12Device* Device, DX12_Arena& BufferArena, DX12_DescriptorHeap& DescriptorHeap);
		void Upload(TData* Data, DX12_UploadArena<>& UploadArena, ID3D12GraphicsCommandList* CommandList);
	};
}


namespace frt::graphics
{
	template <typename TData>
	SConstantBuffer<TData>::SConstantBuffer(ID3D12Device* Device, DX12_Arena& BufferArena, DX12_DescriptorHeap& DescriptorHeap)
	{
		D3D12_RESOURCE_DESC cbDesc = {};
		cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cbDesc.Width = AlignAddress(sizeof(TData), 256);
		cbDesc.Height = 1;
		cbDesc.DepthOrArraySize = 1;
		cbDesc.MipLevels = 1;
		cbDesc.Format = DXGI_FORMAT_UNKNOWN;
		cbDesc.SampleDesc.Count = 1;
		cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		GpuResource = BufferArena.Allocate(cbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbViewDesc = {};
		cbViewDesc.BufferLocation = GpuResource->GetGPUVirtualAddress();
		cbViewDesc.SizeInBytes = (uint32)cbDesc.Width * cbDesc.Height * cbDesc.DepthOrArraySize;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuDesc = {};
		DescriptorHeap.Allocate(&cpuDesc, &DescriptorHeapHandleGpu);
		Device->CreateConstantBufferView(&cbViewDesc, cpuDesc);
	}

	template <typename TData>
	void SConstantBuffer<TData>::Upload(TData* Data, DX12_UploadArena<>& UploadArena, ID3D12GraphicsCommandList* CommandList)
	{
		uint64 DataSize = sizeof(TData);

		uint64 uploadOffset = 0;
		{
			uint8* dest = UploadArena.Allocate(DataSize, &uploadOffset);
			memcpy(dest, Data, DataSize);
		}

		{
			D3D12_RESOURCE_BARRIER resourceBarrier = {};
			resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			resourceBarrier.Transition.pResource = GpuResource;
			resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			CommandList->ResourceBarrier(1, &resourceBarrier);
		}

		CommandList->CopyBufferRegion(GpuResource, 0, UploadArena.GetGPUBuffer(), uploadOffset, DataSize);

		{
			D3D12_RESOURCE_BARRIER resourceBarrier = {};
			resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			resourceBarrier.Transition.pResource = GpuResource;
			resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			CommandList->ResourceBarrier(1, &resourceBarrier);
		}

		CommandList->SetGraphicsRootDescriptorTable(1, DescriptorHeapHandleGpu);
	}
}
