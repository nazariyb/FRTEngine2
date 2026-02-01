#include "DXRUtils.h"

#include "Exception.h"


namespace frt::graphics::raytracing
{
void CBottomLevelASGenerator::AddVertexBuffer (
	ID3D12Resource* VertexBuffer,
	uint64 VertexBufferOffset,
	uint32 VertexCount,
	uint32 VertexSize,
	ID3D12Resource* IndexBuffer,
	uint64 IndexBufferOffset,
	uint32 IndexCount,
	ID3D12Resource* TransformBuffer,
	uint64 TransformBufferOffset,
	bool bOpaque)
{
	VertexBuffers.Add(
		D3D12_RAYTRACING_GEOMETRY_DESC
		{
			.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
			.Flags = bOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
			.Triangles = D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC
			{
				.Transform3x4 = TransformBuffer ? TransformBuffer->GetGPUVirtualAddress() + TransformBufferOffset : 0,
				.IndexFormat = DXGI_FORMAT_R32_UINT,
				.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
				.IndexCount = IndexCount,
				.VertexCount = VertexCount,
				.IndexBuffer = IndexBuffer->GetGPUVirtualAddress() + IndexBufferOffset,
				.VertexBuffer = D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE
				{
					.StartAddress = VertexBuffer->GetGPUVirtualAddress() + VertexBufferOffset,
					.StrideInBytes = VertexSize
				}
			}
		});
}

void CBottomLevelASGenerator::ComputeASBufferSizes (
	ID3D12Device5* Device,
	bool bAllowUpdate,
	uint64* OutScratchSizeInBytes,
	uint64* OutResultSizeInBytes)
{
	Flags = bAllowUpdate
				? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
				: D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ASinputs = {
		.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
		.Flags = Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
		.NumDescs = VertexBuffers.Count(),
		.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
		.pGeometryDescs = VertexBuffers.GetData()
	};

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ASinfo = {};
	Device->GetRaytracingAccelerationStructurePrebuildInfo(&ASinputs, &ASinfo);

	*OutScratchSizeInBytes = ScratchSizeInBytes =
							memory::AlignAddress(
								ASinfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
	*OutResultSizeInBytes = ResultSizeInBytes =
							memory::AlignAddress(
								ASinfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
}

void CBottomLevelASGenerator::Generate (
	ID3D12GraphicsCommandList4* CommandList,
	ID3D12Resource* ScratchBuffer,
	ID3D12Resource* ResultBuffer,
	bool bUpdateOnly,
	ID3D12Resource* PreviousResult)
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = Flags;
	if (flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && bUpdateOnly)
	{
		flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	}

	frt_assert(Flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE || !bUpdateOnly);
	frt_assert(!bUpdateOnly || PreviousResult != nullptr);
	frt_assert(ResultSizeInBytes > 0 && ScratchSizeInBytes > 0);

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc
	{
		.DestAccelerationStructureData = ResultBuffer->GetGPUVirtualAddress(),
		.Inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
		{
			.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
			.Flags = flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
			.NumDescs = VertexBuffers.Count(),
			.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
			.pGeometryDescs = VertexBuffers.GetData()
		},
		.SourceAccelerationStructureData = PreviousResult ? PreviousResult->GetGPUVirtualAddress() : 0,
		.ScratchAccelerationStructureData = ScratchBuffer->GetGPUVirtualAddress()
	};

	CommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	const D3D12_RESOURCE_BARRIER uavBarrier
	{
		.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
		.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
		.UAV = ResultBuffer
	};
	CommandList->ResourceBarrier(1, &uavBarrier);
}


void CTopLevelASGenerator::AddInstance (
	ID3D12Resource* BottomLevelAS,
	const DirectX::XMMATRIX& Transform,
	uint32 InstanceID,
	uint32 HitGroupIndex)
{
	Instances.Add(
		SInstance
		{
			.BottomLevelAS = BottomLevelAS,
			.Transform = Transform,
			.InstanceID = InstanceID,
			.HitGroupIndex = HitGroupIndex
		});
}

void CTopLevelASGenerator::ComputeASBufferSizes (
	ID3D12Device5* Device,
	bool bAllowUpdate,
	uint64* OutScratchSizeInBytes,
	uint64* OutResultSizeInBytes,
	uint64* OutDescriptorSizeInBytes)
{
	Flags = bAllowUpdate
				? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
				: D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ASinputs
	{
		.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
		.Flags = Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
		.NumDescs = Instances.Count(),
		.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
	};

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	Device->GetRaytracingAccelerationStructurePrebuildInfo(&ASinputs, &info);

	*OutScratchSizeInBytes = ScratchSizeInBytes =
							memory::AlignAddress(
								info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
	*OutResultSizeInBytes = ResultSizeInBytes =
							memory::AlignAddress(
								info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
	*OutDescriptorSizeInBytes = DescriptorSizeInBytes =
								memory::AlignAddress(
									static_cast<uint64>(Instances.Count()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
									D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
}

void CTopLevelASGenerator::Generate (
	ID3D12GraphicsCommandList4* CommandList,
	ID3D12Resource* ScratchBuffer,
	ID3D12Resource* ResultBuffer,
	ID3D12Resource* DescriptorsBuffer,
	bool bUpdateOnly,
	ID3D12Resource* PreviousResult)
{
	D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs;
	DescriptorsBuffer->Map(0, nullptr, reinterpret_cast<void**>(&instanceDescs));
	frt_assert(instanceDescs);

	memset(instanceDescs, 0, DescriptorSizeInBytes);

	for (uint32 i = 0; i < Instances.Count(); ++i)
	{
		instanceDescs[i].InstanceID = Instances[i].InstanceID;
		instanceDescs[i].InstanceMask = 0xFF;
		instanceDescs[i].InstanceContributionToHitGroupIndex = Instances[i].HitGroupIndex;
		instanceDescs[i].AccelerationStructure = Instances[i].BottomLevelAS->GetGPUVirtualAddress();
		instanceDescs[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
		DirectX::XMFLOAT3X4 transform3x4;
		DirectX::XMStoreFloat3x4(&transform3x4, Instances[i].Transform);
		memcpy(instanceDescs[i].Transform, &transform3x4, sizeof(transform3x4));
	}

	DescriptorsBuffer->Unmap(0, nullptr);

	D3D12_GPU_VIRTUAL_ADDRESS sourceAS = bUpdateOnly ? PreviousResult->GetGPUVirtualAddress() : 0;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = Flags;
	if (flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && bUpdateOnly)
	{
		flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	}

	frt_assert(Flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE || !bUpdateOnly);
	frt_assert(!bUpdateOnly || PreviousResult != nullptr);

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc
	{
		.DestAccelerationStructureData = ResultBuffer->GetGPUVirtualAddress(),
		.Inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
		{
			.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
			.Flags = flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
			.NumDescs = Instances.Count(),
			.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
			.InstanceDescs = DescriptorsBuffer->GetGPUVirtualAddress(),
		},
		.SourceAccelerationStructureData = sourceAS,
		.ScratchAccelerationStructureData = ScratchBuffer->GetGPUVirtualAddress(),
	};

	CommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	const D3D12_RESOURCE_BARRIER uavBarrier
	{
		.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
		.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
		.UAV = ResultBuffer
	};
	CommandList->ResourceBarrier(1, &uavBarrier);
}

ID3D12Resource* CreateBuffer (
	ID3D12Device* Device,
	uint64 SizeInBytes,
	D3D12_RESOURCE_FLAGS Flags,
	D3D12_RESOURCE_STATES InitialState,
	const D3D12_HEAP_PROPERTIES& HeapProps)
{
	if ((Flags & D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE) ||
		InitialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
	{
		Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
		Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	const D3D12_RESOURCE_DESC bufDesc
	{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment = 0,
		.Width = SizeInBytes,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = DXGI_SAMPLE_DESC
		{
			.Count = 1,
			.Quality = 0
		},
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = Flags
	};

	ID3D12Resource* buffer;
	D3D12_RESOURCE_STATES createState = InitialState;
	if ((Flags & D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE) ||
		InitialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
	{
		createState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	}
	THROW_IF_FAILED(
		Device->CreateCommittedResource(
			&HeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, createState, nullptr, IID_PPV_ARGS(&buffer)));
	return buffer;
}
}
