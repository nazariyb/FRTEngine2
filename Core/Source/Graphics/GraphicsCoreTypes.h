#pragma once

#include <wrl/client.h>
#include <DirectXMath.h>

#include "Math/Math.h"
#include "RenderResourceAllocators.h"
#include "ConstantBuffer.h"
#include "CoreUtils.h"

struct ID3D12Heap;


namespace frt::graphics
{
	using Microsoft::WRL::ComPtr;

#pragma pack(push, 1)
	struct Vertex
	{
		Vector3f position;
		Vector2f uv;
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
		Vector2f RenderTargetSize;
		Vector2f RenderTargetSizeInverse;
		float NearPlane = 0.f;
		float FarPlane = 0.f;
		float TotalTime = 0.f;
		float DeltaTime = 0.f;
	};

	struct SFrameResources
	{
		FRT_DELETE_COPY_OPS(SFrameResources)
		FRT_DEFAULT_MOVE_OPS(SFrameResources)

		ComPtr<ID3D12CommandAllocator> CommandListAllocator;
		SConstantBuffer<SPassConstants> PassCB;
		SConstantBuffer<SObjectConstants> ObjectCB;
		DX12_UploadArena UploadArena;

		uint64 FenceValue = 0;

		SFrameResources() = default;
		SFrameResources(
			ID3D12Device* Device,
			uint32 PassCount,
			uint32 ObjectCount,
			DX12_Arena& BufferArena,
			DX12_DescriptorHeap& DescriptorHeap);
		~SFrameResources();

		void Init(
			ID3D12Device* Device,
			uint32 PassCount,
			uint32 ObjectCount,
			DX12_Arena& BufferArena,
			DX12_DescriptorHeap& DescriptorHeap);
	};
}
