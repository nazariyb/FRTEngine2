#pragma once

#include <DirectXMath.h>
#include <wrl/client.h>

#include "ConstantBuffer.h"
#include "CoreUtils.h"
#include "Enum.h"
#include "RenderResourceAllocators.h"
#include "RenderConstants.h"
#include "Graphics/SColor.h"
#include "Math/Math.h"

struct ID3D12Heap;


namespace frt::graphics
{
using Microsoft::WRL::ComPtr;

#pragma pack(push, 1)
struct SVertex
{
	Vector3f Position;
	Vector2f Uv;
	Vector3f Normal;
	Vector3f Tangent;
	Vector3f Bitangent;
	DirectX::XMFLOAT4 Color;
};
#pragma pack(pop)

struct SObjectConstants
{
	DirectX::XMFLOAT4X4 World;
};


struct SPassConstants
{
	DirectX::XMFLOAT4X4 View;
	DirectX::XMFLOAT4X4 ViewInverse;
	DirectX::XMFLOAT4X4 Projection;
	DirectX::XMFLOAT4X4 ProjectionInverse;
	DirectX::XMFLOAT4X4 ViewProjection;
	DirectX::XMFLOAT4X4 ViewProjectionInverse;
	Vector3f CameraPosition;
	float Pad1;
	Vector2f RenderTargetSize;
	Vector2f RenderTargetSizeInverse;
	float NearPlane = 0.f;
	float FarPlane = 0.f;
	float TotalTime = 0.f;
	float DeltaTime = 0.f;
	uint32 FrameIndex = 0u;
};


struct SMaterialConstants
{
	SColor DiffuseAlbedo = { 1.f, 1.f, 1.f, 1.f };
	uint32 TextureIndices[render::constants::RootMaterialTextureCount] = {};
	uint32 Flags = 0u;
	float Roughness = 1.f;
	float Metallic = 0.f;
	float EmissiveIntensity = 0.f;
	SColor Emissive = { 0.f, 0.f, 0.f, 1.f };
};


struct SFrameResources
{
	FRT_DELETE_COPY_OPS(SFrameResources)
	FRT_DEFAULT_MOVE_OPS(SFrameResources)

	ComPtr<ID3D12CommandAllocator> CommandListAllocator;
	SConstantBuffer<SPassConstants> PassCB;
	SConstantBuffer<SObjectConstants> ObjectCB;
	SConstantBuffer<SMaterialConstants> MaterialCB;
	DX12_UploadArena UploadArena;

	uint64 FenceValue = 0;

	SFrameResources () = default;
	SFrameResources (
		ID3D12Device* Device,
		uint32 PassCount,
		uint32 ObjectCount,
		uint32 MaterialCount,
		DX12_Arena& BufferArena,
		DX12_DescriptorHeap& DescriptorHeap);
	~SFrameResources ();

	void Init (
		ID3D12Device* Device,
		uint32 PassCount,
		uint32 ObjectCount,
		uint32 MaterialCount,
		DX12_Arena& BufferArena,
		DX12_DescriptorHeap& DescriptorHeap);

	void EnsureObjectCapacity (
		ID3D12Device* Device,
		uint32 ObjectCount,
		DX12_Arena& BufferArena,
		DX12_DescriptorHeap& DescriptorHeap);

	void EnsureMaterialCapacity (
		ID3D12Device* Device,
		uint32 MaterialCount,
		DX12_Arena& BufferArena,
		DX12_DescriptorHeap& DescriptorHeap);
};


enum class ERenderMode : int32
{
	Raster = 0,
	Raytracing,
	Hybrid,
	Count,
};


namespace raytracing
{
	struct SAccelerationStructureBuffers
	{
		ComPtr<ID3D12Resource> Scratch;
		ComPtr<ID3D12Resource> Result;
		ComPtr<ID3D12Resource> InstanceDesc;
	};
}
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::graphics::ERenderMode,
	FRT_ENUM_ENTRY(frt::graphics::ERenderMode, Raster),
	FRT_ENUM_ENTRY(frt::graphics::ERenderMode, Raytracing),
	FRT_ENUM_ENTRY(frt::graphics::ERenderMode, Hybrid));