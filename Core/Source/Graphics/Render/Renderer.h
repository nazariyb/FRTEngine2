#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Core.h"
#include "GraphicsCoreTypes.h"
#include "RenderConstants.h"
#include "ShaderAsset.h"


namespace frt
{
class CWindow;
}


using Microsoft::WRL::ComPtr;


namespace frt::graphics
{
class CCamera;


class CRenderer
{
public:
	CRenderer () = delete;
	FRT_CORE_API explicit CRenderer (CWindow* Window);

	void Resize (bool bNewFullscreenState);

	FRT_CORE_API void StartFrame (CCamera& Camera);
	FRT_CORE_API void Tick (float DeltaSeconds);
	FRT_CORE_API void Draw (float DeltaSeconds, CCamera& Camera);

	FRT_CORE_API IDXGIAdapter1* GetAdapter ();
	FRT_CORE_API ID3D12Device* GetDevice ();
	FRT_CORE_API ID3D12CommandQueue* GetCommandQueue ();
	FRT_CORE_API ID3D12GraphicsCommandList* GetCommandList ();
	FRT_CORE_API DX12_Arena& GetBufferArena ();
	FRT_CORE_API DX12_DescriptorHeap& GetDescriptorHeap ();
	FRT_CORE_API SFrameResources& GetCurrentFrameResource ();

	FRT_CORE_API ID3D12Resource* CreateBufferAsset (
		const D3D12_RESOURCE_DESC& Desc,
		D3D12_RESOURCE_STATES InitialState,
		void* BufferData);
	FRT_CORE_API ID3D12Resource* CreateTextureAsset (
		const D3D12_RESOURCE_DESC& Desc,
		D3D12_RESOURCE_STATES InitialState,
		void* Texels);
	FRT_CORE_API void CreateShaderResourceView (
		ID3D12Resource* Texture,
		const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle);

private:
	void CreateSwapChain (bool bFullscreen);
	void FlushCommandQueue ();
	void CreateRootSignature ();
	void CreatePipelineState ();

public:
	DX12_DescriptorHeap ShaderDescriptorHeap;

private:
	CWindow* Window;

	D3D12_VIEWPORT Viewport;
	D3D12_RECT ScissorRect;

	// Pipeline
	ComPtr<IDXGIAdapter1> Adapter;
	ComPtr<ID3D12Device> Device;
	ComPtr<IDXGIFactory4> Factory;

	ComPtr<IDXGISwapChain1> SwapChain;
	ComPtr<ID3D12Resource> FrameBuffer[render::constants::SwapChainBufferCount];
	D3D12_CPU_DESCRIPTOR_HANDLE FrameBufferDescriptors[render::constants::SwapChainBufferCount];
	unsigned CurrentBackBufferIndex;

	SFrameResources FramesResources[render::constants::FrameResourcesBufferCount];
	uint8 CurrentFrameResourceIndex = render::constants::FrameResourcesBufferCount - 1;

	ComPtr<ID3D12CommandQueue> CommandQueue;
	ComPtr<ID3D12CommandAllocator> CommandAllocator;
	ComPtr<ID3D12GraphicsCommandList> CommandList;

	ComPtr<ID3D12RootSignature> RootSignature;
	ComPtr<ID3D12PipelineState> PipelineState;
	CShaderLibrary ShaderLibrary;

	ComPtr<ID3D12Resource> CommonConstantBuffer;
	D3D12_GPU_DESCRIPTOR_HANDLE CommonConstantBufferDescriptor;

	DX12_DescriptorHeap RtvHeap;
	DX12_Arena RtvArena;
	DX12_Arena BufferArena;
	DX12_Arena TextureArena;

	DX12_DescriptorHeap DsvHeap;
	ComPtr<ID3D12Resource> DepthStencilBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilDescriptor;
	// ~Pipeline

	// Synchronization
	ComPtr<ID3D12Fence> Fence;
	unsigned long long FenceValue;
	HANDLE FenceEvent;
	// ~Synchronization
};


inline static uint32 Dx12GetBytesPerPixel (DXGI_FORMAT Format)
{
	switch (Format)
	{
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT: return 16u;

		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT: return 8u;

		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT: return 4u;

		case DXGI_FORMAT_R8G8_TYPELESS:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT: return 2u;

		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
		case DXGI_FORMAT_A8_UNORM:
		default: return 1u;
	}
}
}
