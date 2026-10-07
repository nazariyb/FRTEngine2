#pragma once

#include "Graphics/Render/RHI/RHICommon.h"


namespace frt::graphics::rhi
{
enum class EBufferViewKind : uint8
{
	None,
	Raw,         // ByteAddressBuffer / RWByteAddressBuffer
	Structured,  // StructuredBuffer<T> with Stride
	Typed,       // Buffer<T> with Format
};

struct SBufferDesc
{
	uint64 Size = 0u;
	uint32 Stride = 0u;                 // Structured only
	DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN; // Typed only
	EBufferViewKind SrvKind = EBufferViewKind::None;
	EBufferViewKind UavKind = EBufferViewKind::None;
	D3D12_HEAP_TYPE HeapType = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
	bool bAccelerationStructure = false;
	const wchar_t* Name = nullptr;
};

struct FRT_CORE_API SGpuBuffer
{
	ComPtr<ID3D12Resource> Resource;
	uint64 Size = 0u;
	uint32 Stride = 0u;
	D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
	uint32 Srv = InvalidDescriptorIndex;
	uint32 Uav = InvalidDescriptorIndex;
	uint8* Mapped = nullptr;              // upload/readback heaps only

	bool IsValid () const { return Resource != nullptr; }
	D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress () const { return Resource ? Resource->GetGPUVirtualAddress() : 0u; }
};

struct STextureDesc
{
	uint32 Width = 1u;
	uint32 Height = 1u;
	uint16 DepthOrArraySize = 1u;
	uint16 MipLevels = 1u;
	DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;     // resource format (may be TYPELESS)
	DXGI_FORMAT SrvFormat = DXGI_FORMAT_UNKNOWN;          // UNKNOWN = Format
	DXGI_FORMAT UavFormat = DXGI_FORMAT_UNKNOWN;          // UNKNOWN = Format
	DXGI_FORMAT RtvFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT DsvFormat = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
	bool bSrv = true;
	bool bUav = false;          // UAV for every mip (Uav = mip 0, MipUavs[i])
	bool bRtv = false;
	bool bDsv = false;
	bool bCube = false;
	const D3D12_CLEAR_VALUE* ClearValue = nullptr;
	const wchar_t* Name = nullptr;
};

inline constexpr uint32 MaxTextureMips = 16u;

struct FRT_CORE_API SGpuTexture
{
	ComPtr<ID3D12Resource> Resource;
	DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
	uint32 Width = 0u;
	uint32 Height = 0u;
	uint16 DepthOrArraySize = 1u;
	uint16 MipLevels = 1u;
	D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
	uint32 Srv = InvalidDescriptorIndex;
	uint32 Uav = InvalidDescriptorIndex;                 // mip 0
	uint32 MipUavs[MaxTextureMips] = {
		InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex,
		InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex,
		InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex,
		InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex, InvalidDescriptorIndex };
	uint32 Rtv = InvalidDescriptorIndex;                 // CPU RTV heap index
	uint32 Dsv = InvalidDescriptorIndex;                 // CPU DSV heap index

	bool IsValid () const { return Resource != nullptr; }
};

// Bytes per texel for the uncompressed formats the engine creates. 0 for unknown.
FRT_CORE_API uint32 GetFormatBytesPerPixel (DXGI_FORMAT Format);
FRT_CORE_API DXGI_FORMAT ToSrgbFormat (DXGI_FORMAT Format);
FRT_CORE_API uint32 ComputeMipCount (uint32 Width, uint32 Height);
}
