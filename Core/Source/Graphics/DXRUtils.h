#pragma once

#include <DirectXMath.h>

#include "Containers/Array.h"
#include "Graphics/Render/RenderResourceAllocators.h"


namespace frt::graphics::raytracing
{
class CBottomLevelASGenerator
{
public:
	void AddVertexBuffer (
		ID3D12Resource* VertexBuffer,
		uint64 VertexBufferOffset,
		uint32 VertexCount,
		uint32 VertexSize,
		ID3D12Resource* IndexBuffer,
		uint64 IndexBufferOffset,
		uint32 IndexCount,
		ID3D12Resource* TransformBuffer,
		uint64 TransformBufferOffset,
		bool bOpaque = true);

	void ComputeASBufferSizes (
		ID3D12Device5* Device,
		bool bAllowUpdate,
		uint64* OutScratchSizeInBytes,
		uint64* OutResultSizeInBytes);

	void Generate (
		ID3D12GraphicsCommandList4* CommandList,
		ID3D12Resource* ScratchBuffer,
		ID3D12Resource* ResultBuffer,
		bool bUpdateOnly = false,
		ID3D12Resource* PreviousResult = nullptr);

private:
	TArray<D3D12_RAYTRACING_GEOMETRY_DESC> VertexBuffers;
	uint64 ScratchSizeInBytes = 0;
	uint64 ResultSizeInBytes = 0;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
};


class CTopLevelASGenerator
{
public:
	void AddInstance (
		ID3D12Resource* BottomLevelAS,
		const DirectX::XMMATRIX& Transform,
		uint32 InstanceID,
		uint32 HitGroupIndex);

	void ComputeASBufferSizes (
		ID3D12Device5* Device,
		bool bAllowUpdate,
		uint64* OutScratchSizeInBytes,
		uint64* OutResultSizeInBytes,
		uint64* OutDescriptorSizeInBytes);

	void Generate (
		ID3D12GraphicsCommandList4* CommandList,
		ID3D12Resource* ScratchBuffer,
		ID3D12Resource* ResultBuffer,
		ID3D12Resource* DescriptorsBuffer,
		bool bUpdateOnly = false,
		ID3D12Resource* PreviousResult = nullptr);

private:
	struct SInstance
	{
		ID3D12Resource* BottomLevelAS = nullptr;
		DirectX::XMMATRIX Transform = {};
		uint32 InstanceID = 0;
		uint32 HitGroupIndex = 0;
	};


	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	TArray<SInstance> Instances;

	uint64 ScratchSizeInBytes = 0;
	uint64 ResultSizeInBytes = 0;
	uint64 DescriptorSizeInBytes = 0;
};

inline static const D3D12_HEAP_PROPERTIES DefaultHeapProps =
{
	D3D12_HEAP_TYPE_DEFAULT,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0,
	0
};
inline static const D3D12_HEAP_PROPERTIES UploadHeapProps =
{
	D3D12_HEAP_TYPE_UPLOAD,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0,
	0
};

ID3D12Resource* CreateBuffer (
	ID3D12Device* Device,
	uint64 SizeInBytes,
	D3D12_RESOURCE_FLAGS Flags,
	D3D12_RESOURCE_STATES InitialState,
	const D3D12_HEAP_PROPERTIES& HeapProps);
}
