#include "GraphicsCoreTypes.h"

#include "Exception.h"
#include "Memory/Memory.h"


namespace frt::graphics
{
SFrameResources::SFrameResources (
	ID3D12Device* Device,
	uint32 PassCount,
	uint32 ObjectCount,
	DX12_Arena& BufferArena,
	DX12_DescriptorHeap& DescriptorHeap)
{
	Init(Device, PassCount, ObjectCount, BufferArena, DescriptorHeap);
}

SFrameResources::~SFrameResources ()
{}

void SFrameResources::Init (
	ID3D12Device* Device,
	uint32 PassCount,
	uint32 ObjectCount,
	DX12_Arena& BufferArena,
	DX12_DescriptorHeap& DescriptorHeap)
{
	THROW_IF_FAILED(
		Device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(CommandListAllocator.GetAddressOf())));

	UploadArena = DX12_UploadArena(Device, 200 * memory::MegaByte);

	PassCB = SConstantBuffer<SPassConstants>(Device, BufferArena, DescriptorHeap, PassCount);
	ObjectCB = SConstantBuffer<SObjectConstants>(Device, BufferArena, DescriptorHeap, ObjectCount);
}
}
