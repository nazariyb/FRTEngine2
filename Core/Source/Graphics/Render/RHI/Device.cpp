#include "Graphics/Render/RHI/Device.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>


namespace frt::graphics::rhi
{
namespace
{
const D3D12_HEAP_PROPERTIES& HeapProps (D3D12_HEAP_TYPE Type)
{
	static D3D12_HEAP_PROPERTIES props[4] = {};
	static bool bInit = false;
	if (!bInit)
	{
		for (uint32 i = 0; i < 4; ++i)
		{
			props[i].Type = static_cast<D3D12_HEAP_TYPE>(i + 1);
			props[i].CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			props[i].MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			props[i].CreationNodeMask = 1;
			props[i].VisibleNodeMask = 1;
		}
		bInit = true;
	}
	return props[static_cast<uint32>(Type) - 1];
}

constexpr uint64 kStagingChunkSize = 128ull * 1024ull * 1024ull;

bool IsDepthFormat (DXGI_FORMAT Format)
{
	switch (Format)
	{
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			return true;
		default:
			return false;
	}
}
}


uint32 GetFormatBytesPerPixel (DXGI_FORMAT Format)
{
	switch (Format)
	{
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			return 16u;
		case DXGI_FORMAT_R32G32B32_FLOAT:
			return 12u;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
			return 8u;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_D32_FLOAT:
			return 4u;
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
			return 2u;
		case DXGI_FORMAT_R8_UNORM:
			return 1u;
		default:
			return 0u;
	}
}

DXGI_FORMAT ToSrgbFormat (DXGI_FORMAT Format)
{
	switch (Format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
		case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
		case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
		default: return Format;
	}
}

uint32 ComputeMipCount (uint32 Width, uint32 Height)
{
	uint32 levels = 1u;
	uint32 size = (std::max)(Width, Height);
	while (size > 1u)
	{
		size >>= 1u;
		++levels;
	}
	return levels;
}


// ---------------------------------------------------------------------------------------------
// CUploadRing
// ---------------------------------------------------------------------------------------------

void CUploadRing::Init (ID3D12Device* Device, uint64 InBytesPerFrame)
{
	BytesPerFrame = InBytesPerFrame;
	for (uint32 i = 0; i < MaxFramesInFlight; ++i)
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = BytesPerFrame;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		THROW_IF_FAILED(Device->CreateCommittedResource(
			&HeapProps(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffers[i])));
		SetDebugName(Buffers[i].Get(), "UploadRing");
		const D3D12_RANGE noRead = { 0, 0 };
		THROW_IF_FAILED(Buffers[i]->Map(0, &noRead, reinterpret_cast<void**>(&Mapped[i])));
	}
	Cursor = 0u;
	FrameSlot = 0u;
}

void CUploadRing::Shutdown ()
{
	for (uint32 i = 0; i < MaxFramesInFlight; ++i)
	{
		if (Buffers[i])
		{
			Buffers[i]->Unmap(0, nullptr);
		}
		Buffers[i].Reset();
		Mapped[i] = nullptr;
	}
}

void CUploadRing::BeginFrame (uint32 InFrameSlot)
{
	FrameSlot = InFrameSlot;
	Cursor = 0u;
}

SUploadAllocation CUploadRing::Allocate (uint64 Size, uint64 Alignment)
{
	std::lock_guard lock(Mutex);
	const uint64 offset = AlignUp(Cursor, Alignment);
	if (offset + Size > BytesPerFrame)
	{
		frt_assert(false);
		throw std::runtime_error("Upload ring exhausted for this frame");
	}
	Cursor = offset + Size;

	SUploadAllocation result;
	result.Cpu = Mapped[FrameSlot] + offset;
	result.Gpu = Buffers[FrameSlot]->GetGPUVirtualAddress() + offset;
	result.Resource = Buffers[FrameSlot].Get();
	result.Offset = offset;
	result.Size = Size;
	return result;
}

SUploadAllocation CUploadRing::PushBytes (const void* Data, uint64 Size, uint64 Alignment)
{
	SUploadAllocation alloc = Allocate((std::max<uint64>)(Size, 16u), Alignment);
	if (Data && Size > 0u)
	{
		memcpy(alloc.Cpu, Data, Size);
	}
	return alloc;
}


// ---------------------------------------------------------------------------------------------
// CBarrierBatch
// ---------------------------------------------------------------------------------------------

void CBarrierBatch::Transition (SGpuTexture& Texture, D3D12_RESOURCE_STATES After)
{
	if (!Texture.Resource || Texture.State == After)
	{
		return;
	}
	Transition(Texture.Resource.Get(), Texture.State, After);
	Texture.State = After;
}

void CBarrierBatch::Transition (SGpuBuffer& Buffer, D3D12_RESOURCE_STATES After)
{
	if (!Buffer.Resource || Buffer.State == After)
	{
		return;
	}
	Transition(Buffer.Resource.Get(), Buffer.State, After);
	Buffer.State = After;
}

void CBarrierBatch::Transition (
	ID3D12Resource* Resource,
	D3D12_RESOURCE_STATES Before,
	D3D12_RESOURCE_STATES After,
	uint32 Subresource)
{
	if (!Resource || Before == After)
	{
		return;
	}
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = Resource;
	barrier.Transition.Subresource = Subresource;
	barrier.Transition.StateBefore = Before;
	barrier.Transition.StateAfter = After;
	Barriers.push_back(barrier);
}

void CBarrierBatch::Uav (const SGpuTexture& Texture)
{
	Uav(Texture.Resource.Get());
}

void CBarrierBatch::Uav (const SGpuBuffer& Buffer)
{
	Uav(Buffer.Resource.Get());
}

void CBarrierBatch::Uav (ID3D12Resource* Resource)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = Resource;
	Barriers.push_back(barrier);
}

void CBarrierBatch::Flush (ID3D12GraphicsCommandList* CommandList)
{
	if (!Barriers.empty())
	{
		CommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
		Barriers.clear();
	}
}


// ---------------------------------------------------------------------------------------------
// CDevice
// ---------------------------------------------------------------------------------------------

void CDevice::Init (ID3D12Device5* InDevice, ID3D12CommandQueue* InQueue, const SDeviceCaps& InCaps)
{
	Device = InDevice;
	Queue = InQueue;
	Caps = InCaps;

	BindlessHeap.Init(Device, 65536u, 4096u);
	RtvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256u);
	DsvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64u);
	UploadRing.Init(Device, 64ull * 1024ull * 1024ull);
}

void CDevice::Shutdown ()
{
	ProcessDeferredReleases(~0ull);
	CurrentStaging = {};
	UploadRing.Shutdown();
	DsvHeap.Shutdown();
	RtvHeap.Shutdown();
	BindlessHeap.Shutdown();
}

void CDevice::BeginFrame (uint32 InFrameSlot, uint64 SubmissionFenceValue)
{
	FrameSlot = InFrameSlot;
	SubmissionFence = SubmissionFenceValue;
	BindlessHeap.BeginFrame(InFrameSlot);
	UploadRing.BeginFrame(InFrameSlot);
	StagingBytesThisFrame = 0u;
}

void CDevice::ProcessDeferredReleases (uint64 CompletedFenceValue)
{
	std::lock_guard lock(ReleaseMutex);
	while (!PendingReleases.empty() && PendingReleases.front().FenceValue <= CompletedFenceValue)
	{
		PendingReleases.pop_front();
	}
	while (!PendingDescriptors.empty() && PendingDescriptors.front().FenceValue <= CompletedFenceValue)
	{
		const SPendingDescriptor& entry = PendingDescriptors.front();
		switch (entry.Heap)
		{
			case 0: BindlessHeap.FreePersistent(entry.Index); break;
			case 1: RtvHeap.Free(entry.Index); break;
			case 2: DsvHeap.Free(entry.Index); break;
			default: break;
		}
		PendingDescriptors.pop_front();
	}
}

void CDevice::DeferRelease (ComPtr<IUnknown> Object)
{
	if (!Object)
	{
		return;
	}
	std::lock_guard lock(ReleaseMutex);
	PendingReleases.push_back({ SubmissionFence, std::move(Object) });
}

void CDevice::DeferRelease (ComPtr<ID3D12Resource> Resource)
{
	ComPtr<IUnknown> unknown;
	if (Resource)
	{
		Resource.As(&unknown);
	}
	DeferRelease(std::move(unknown));
}

void CDevice::DeferFreeDescriptor (uint32 BindlessIndex)
{
	if (BindlessIndex == InvalidDescriptorIndex)
	{
		return;
	}
	std::lock_guard lock(ReleaseMutex);
	PendingDescriptors.push_back({ SubmissionFence, BindlessIndex, 0u });
}

void CDevice::DeferFreeRtv (uint32 Index)
{
	if (Index == InvalidDescriptorIndex)
	{
		return;
	}
	std::lock_guard lock(ReleaseMutex);
	PendingDescriptors.push_back({ SubmissionFence, Index, 1u });
}

void CDevice::DeferFreeDsv (uint32 Index)
{
	if (Index == InvalidDescriptorIndex)
	{
		return;
	}
	std::lock_guard lock(ReleaseMutex);
	PendingDescriptors.push_back({ SubmissionFence, Index, 2u });
}

void CDevice::Destroy (SGpuBuffer& Buffer)
{
	if (Buffer.Mapped && Buffer.Resource)
	{
		Buffer.Resource->Unmap(0, nullptr);
	}
	DeferFreeDescriptor(Buffer.Srv);
	DeferFreeDescriptor(Buffer.Uav);
	DeferRelease(std::move(Buffer.Resource));
	Buffer = {};
}

void CDevice::Destroy (SGpuTexture& Texture)
{
	DeferFreeDescriptor(Texture.Srv);
	for (uint32 mip = 0; mip < MaxTextureMips; ++mip)
	{
		DeferFreeDescriptor(Texture.MipUavs[mip]);
	}
	DeferFreeRtv(Texture.Rtv);
	DeferFreeDsv(Texture.Dsv);
	DeferRelease(std::move(Texture.Resource));
	Texture = {};
}

SGpuBuffer CDevice::CreateBuffer (const SBufferDesc& Desc)
{
	frt_assert(Desc.Size > 0u);

	SGpuBuffer buffer;
	buffer.Size = Desc.Size;
	buffer.Stride = Desc.Stride;

	D3D12_RESOURCE_DESC resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = AlignUp(Desc.Size, 16u);
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = Desc.Flags;
	if (Desc.UavKind != EBufferViewKind::None || Desc.bAccelerationStructure)
	{
		resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	if (Desc.bAccelerationStructure)
	{
		resourceDesc.Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
	}

	D3D12_RESOURCE_STATES initialState = Desc.InitialState;
	if (Desc.HeapType == D3D12_HEAP_TYPE_UPLOAD)
	{
		initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
	}
	else if (Desc.HeapType == D3D12_HEAP_TYPE_READBACK)
	{
		initialState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	else if (Desc.bAccelerationStructure)
	{
		initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	}
	else
	{
		// Buffers are always created in COMMON by the runtime; record what the runtime does.
		initialState = D3D12_RESOURCE_STATE_COMMON;
	}

	THROW_IF_FAILED(Device->CreateCommittedResource(
		&HeapProps(Desc.HeapType), D3D12_HEAP_FLAG_NONE, &resourceDesc,
		initialState, nullptr, IID_PPV_ARGS(&buffer.Resource)));
	buffer.State = initialState;
	SetDebugName(buffer.Resource.Get(), Desc.Name);

	if (Desc.HeapType == D3D12_HEAP_TYPE_UPLOAD)
	{
		const D3D12_RANGE noRead = { 0, 0 };
		THROW_IF_FAILED(buffer.Resource->Map(0, &noRead, reinterpret_cast<void**>(&buffer.Mapped)));
	}
	else if (Desc.HeapType == D3D12_HEAP_TYPE_READBACK)
	{
		THROW_IF_FAILED(buffer.Resource->Map(0, nullptr, reinterpret_cast<void**>(&buffer.Mapped)));
	}

	if (Desc.bAccelerationStructure)
	{
		buffer.Srv = BindlessHeap.AllocatePersistent();
		WriteAccelerationStructureSrv(buffer.Srv, buffer.Resource->GetGPUVirtualAddress());
	}
	else if (Desc.SrvKind != EBufferViewKind::None)
	{
		buffer.Srv = BindlessHeap.AllocatePersistent();
		WriteBufferSrv(buffer.Srv, buffer.Resource.Get(), Desc.SrvKind, Desc.Size, Desc.Stride, Desc.Format);
	}
	if (Desc.UavKind != EBufferViewKind::None)
	{
		buffer.Uav = BindlessHeap.AllocatePersistent();
		WriteBufferUav(buffer.Uav, buffer.Resource.Get(), Desc.UavKind, Desc.Size, Desc.Stride, Desc.Format);
	}
	return buffer;
}

void CDevice::WriteBufferSrv (
	uint32 Index,
	ID3D12Resource* Resource,
	EBufferViewKind Kind,
	uint64 Size,
	uint32 Stride,
	DXGI_FORMAT Format,
	uint64 FirstByte)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	switch (Kind)
	{
		case EBufferViewKind::Raw:
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.Buffer.FirstElement = FirstByte / 4u;
			desc.Buffer.NumElements = static_cast<UINT>((Size - FirstByte) / 4u);
			desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			break;
		case EBufferViewKind::Structured:
			frt_assert(Stride > 0u);
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.Buffer.FirstElement = FirstByte / Stride;
			desc.Buffer.NumElements = static_cast<UINT>((Size - FirstByte) / Stride);
			desc.Buffer.StructureByteStride = Stride;
			break;
		case EBufferViewKind::Typed:
		{
			desc.Format = Format;
			const uint32 bpp = (std::max)(1u, GetFormatBytesPerPixel(Format));
			desc.Buffer.FirstElement = FirstByte / bpp;
			desc.Buffer.NumElements = static_cast<UINT>((Size - FirstByte) / bpp);
			break;
		}
		default:
			return;
	}
	Device->CreateShaderResourceView(Resource, &desc, BindlessHeap.GetCpuHandle(Index));
}

void CDevice::WriteBufferUav (
	uint32 Index,
	ID3D12Resource* Resource,
	EBufferViewKind Kind,
	uint64 Size,
	uint32 Stride,
	DXGI_FORMAT Format)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	switch (Kind)
	{
		case EBufferViewKind::Raw:
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.Buffer.NumElements = static_cast<UINT>(Size / 4u);
			desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
			break;
		case EBufferViewKind::Structured:
			frt_assert(Stride > 0u);
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.Buffer.NumElements = static_cast<UINT>(Size / Stride);
			desc.Buffer.StructureByteStride = Stride;
			break;
		case EBufferViewKind::Typed:
			desc.Format = Format;
			desc.Buffer.NumElements = static_cast<UINT>(Size / (std::max)(1u, GetFormatBytesPerPixel(Format)));
			break;
		default:
			return;
	}
	Device->CreateUnorderedAccessView(Resource, nullptr, &desc, BindlessHeap.GetCpuHandle(Index));
}

void CDevice::WriteAccelerationStructureSrv (uint32 Index, D3D12_GPU_VIRTUAL_ADDRESS Location)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.RaytracingAccelerationStructure.Location = Location;
	Device->CreateShaderResourceView(nullptr, &desc, BindlessHeap.GetCpuHandle(Index));
}

SGpuTexture CDevice::CreateTexture (const STextureDesc& Desc)
{
	SGpuTexture texture;
	texture.Format = Desc.Format;
	texture.Width = Desc.Width;
	texture.Height = Desc.Height;
	texture.DepthOrArraySize = Desc.DepthOrArraySize;
	texture.MipLevels = Desc.MipLevels;

	D3D12_RESOURCE_DESC resourceDesc = {};
	resourceDesc.Dimension = Desc.Dimension;
	resourceDesc.Width = Desc.Width;
	resourceDesc.Height = Desc.Height;
	resourceDesc.DepthOrArraySize = Desc.DepthOrArraySize;
	resourceDesc.MipLevels = Desc.MipLevels;
	resourceDesc.Format = Desc.Format;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resourceDesc.Flags = Desc.Flags;
	if (Desc.bUav)
	{
		resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	if (Desc.bRtv)
	{
		resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	}
	if (Desc.bDsv)
	{
		resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		if (!Desc.bSrv)
		{
			resourceDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
		}
	}

	THROW_IF_FAILED(Device->CreateCommittedResource(
		&HeapProps(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &resourceDesc,
		Desc.InitialState, Desc.ClearValue, IID_PPV_ARGS(&texture.Resource)));
	texture.State = Desc.InitialState;
	SetDebugName(texture.Resource.Get(), Desc.Name);

	const bool bArray = Desc.DepthOrArraySize > 1u && Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;

	if (Desc.bSrv)
	{
		texture.Srv = BindlessHeap.AllocatePersistent();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = Desc.SrvFormat != DXGI_FORMAT_UNKNOWN ? Desc.SrvFormat : Desc.Format;
		if (srv.Format == DXGI_FORMAT_D32_FLOAT)
		{
			srv.Format = DXGI_FORMAT_R32_FLOAT;
		}
		if (Desc.bCube)
		{
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			srv.TextureCube.MipLevels = Desc.MipLevels;
		}
		else if (Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			srv.Texture3D.MipLevels = Desc.MipLevels;
		}
		else if (bArray)
		{
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			srv.Texture2DArray.MipLevels = Desc.MipLevels;
			srv.Texture2DArray.ArraySize = Desc.DepthOrArraySize;
		}
		else
		{
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = Desc.MipLevels;
		}
		Device->CreateShaderResourceView(texture.Resource.Get(), &srv, BindlessHeap.GetCpuHandle(texture.Srv));
	}

	if (Desc.bUav)
	{
		const uint32 mipCount = (std::min<uint32>)(Desc.MipLevels, MaxTextureMips);
		for (uint32 mip = 0; mip < mipCount; ++mip)
		{
			const uint32 slot = BindlessHeap.AllocatePersistent();
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
			uav.Format = Desc.UavFormat != DXGI_FORMAT_UNKNOWN ? Desc.UavFormat : Desc.Format;
			if (Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
			{
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
				uav.Texture3D.MipSlice = mip;
				uav.Texture3D.WSize = Desc.DepthOrArraySize;
			}
			else if (bArray || Desc.bCube)
			{
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				uav.Texture2DArray.MipSlice = mip;
				uav.Texture2DArray.ArraySize = Desc.DepthOrArraySize;
			}
			else
			{
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uav.Texture2D.MipSlice = mip;
			}
			Device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, &uav, BindlessHeap.GetCpuHandle(slot));
			texture.MipUavs[mip] = slot;
		}
		texture.Uav = texture.MipUavs[0];
	}

	if (Desc.bRtv)
	{
		texture.Rtv = RtvHeap.Allocate();
		D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
		rtv.Format = Desc.RtvFormat != DXGI_FORMAT_UNKNOWN ? Desc.RtvFormat : Desc.Format;
		rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		Device->CreateRenderTargetView(texture.Resource.Get(), &rtv, RtvHeap.GetCpuHandle(texture.Rtv));
	}

	if (Desc.bDsv)
	{
		texture.Dsv = DsvHeap.Allocate();
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = Desc.DsvFormat != DXGI_FORMAT_UNKNOWN ? Desc.DsvFormat
					: (IsDepthFormat(Desc.Format) ? Desc.Format : DXGI_FORMAT_D32_FLOAT);
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		Device->CreateDepthStencilView(texture.Resource.Get(), &dsv, DsvHeap.GetCpuHandle(texture.Dsv));
	}

	return texture;
}

SUploadAllocation CDevice::AllocateStaging (uint64 Size, uint64 Alignment)
{
	std::lock_guard lock(StagingMutex);
	uint64 offset = AlignUp(CurrentStaging.Cursor, Alignment);
	if (!CurrentStaging.Buffer || offset + Size > CurrentStaging.Size)
	{
		// Retire the current chunk: it stays alive until this frame's fence passes.
		if (CurrentStaging.Buffer)
		{
			CurrentStaging.Buffer->Unmap(0, nullptr);
			DeferRelease(std::move(CurrentStaging.Buffer));
		}
		CurrentStaging = {};
		CurrentStaging.Size = (std::max)(kStagingChunkSize, AlignUp(Size, 65536u));

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = CurrentStaging.Size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		THROW_IF_FAILED(Device->CreateCommittedResource(
			&HeapProps(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CurrentStaging.Buffer)));
		SetDebugName(CurrentStaging.Buffer.Get(), "StagingChunk");
		const D3D12_RANGE noRead = { 0, 0 };
		THROW_IF_FAILED(CurrentStaging.Buffer->Map(0, &noRead, reinterpret_cast<void**>(&CurrentStaging.Mapped)));
		offset = 0u;
	}

	CurrentStaging.Cursor = offset + Size;
	StagingBytesThisFrame += Size;

	SUploadAllocation result;
	result.Cpu = CurrentStaging.Mapped + offset;
	result.Gpu = CurrentStaging.Buffer->GetGPUVirtualAddress() + offset;
	result.Resource = CurrentStaging.Buffer.Get();
	result.Offset = offset;
	result.Size = Size;

	// The chunk must survive until every copy recorded into it this frame has executed, even
	// if it is never retired by a later allocation. One reference per frame that used it is the
	// simplest guarantee; the chunk itself keeps being reused across frames.
	if (CurrentStaging.KeepAliveFence != SubmissionFence)
	{
		CurrentStaging.KeepAliveFence = SubmissionFence;
		ComPtr<ID3D12Resource> keepAlive = CurrentStaging.Buffer;
		DeferRelease(std::move(keepAlive));
	}
	return result;
}

void CDevice::UploadBuffer (
	ID3D12GraphicsCommandList* CommandList,
	SGpuBuffer& Destination,
	const void* Data,
	uint64 Size,
	uint64 DestinationOffset,
	D3D12_RESOURCE_STATES FinalState)
{
	frt_assert(Destination.Resource);
	if (Size == 0u)
	{
		return;
	}
	SUploadAllocation staging = AllocateStaging(Size, 16u);
	if (Data)
	{
		memcpy(staging.Cpu, Data, Size);
	}
	else
	{
		memset(staging.Cpu, 0, Size);
	}

	CBarrierBatch barriers;
	barriers.Transition(Destination, D3D12_RESOURCE_STATE_COPY_DEST);
	barriers.Flush(CommandList);
	CommandList->CopyBufferRegion(Destination.Resource.Get(), DestinationOffset, staging.Resource, staging.Offset, Size);
	barriers.Transition(Destination, FinalState);
	barriers.Flush(CommandList);
}

void CDevice::UploadTexture (
	ID3D12GraphicsCommandList* CommandList,
	SGpuTexture& Destination,
	const void* const* MipData,
	uint32 MipCount,
	D3D12_RESOURCE_STATES FinalState)
{
	frt_assert(Destination.Resource);
	const D3D12_RESOURCE_DESC desc = Destination.Resource->GetDesc();

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[MaxTextureMips] = {};
	UINT rowCounts[MaxTextureMips] = {};
	UINT64 rowSizes[MaxTextureMips] = {};
	UINT64 totalBytes = 0u;
	MipCount = (std::min<uint32>)(MipCount, MaxTextureMips);
	Device->GetCopyableFootprints(&desc, 0, MipCount, 0, footprints, rowCounts, rowSizes, &totalBytes);

	SUploadAllocation staging = AllocateStaging(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

	CBarrierBatch barriers;
	barriers.Transition(Destination, D3D12_RESOURCE_STATE_COPY_DEST);
	barriers.Flush(CommandList);

	for (uint32 mip = 0; mip < MipCount; ++mip)
	{
		const uint8* src = static_cast<const uint8*>(MipData[mip]);
		uint8* dst = staging.Cpu + footprints[mip].Offset;
		for (UINT row = 0; row < rowCounts[mip]; ++row)
		{
			memcpy(dst + row * footprints[mip].Footprint.RowPitch, src + row * rowSizes[mip], rowSizes[mip]);
		}

		D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
		dstLocation.pResource = Destination.Resource.Get();
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = mip;

		D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
		srcLocation.pResource = staging.Resource;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint = footprints[mip];
		srcLocation.PlacedFootprint.Offset += staging.Offset;

		CommandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
	}

	barriers.Transition(Destination, FinalState);
	barriers.Flush(CommandList);
}
}
