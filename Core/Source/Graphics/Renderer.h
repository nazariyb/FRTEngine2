#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Core.h"
#include "GraphicsCoreTypes.h"
#include "Model.h"
#include "Math/Transform.h"


namespace frt
{
	class Window;
}

using Microsoft::WRL::ComPtr;

namespace frt::graphics
{
class CCamera;

class Renderer
{
public:

	Renderer() = delete;
	FRT_CORE_API explicit Renderer(Window* Window);

	void Resize();

	FRT_CORE_API void StartFrame(CCamera& Camera);
	FRT_CORE_API void Draw(float DeltaSeconds, CCamera& Camera);

	FRT_CORE_API IDXGIAdapter1* GetAdapter();
	FRT_CORE_API ID3D12Device* GetDevice();
	FRT_CORE_API ID3D12CommandQueue* GetCommandQueue();
	FRT_CORE_API ID3D12GraphicsCommandList* GetCommandList();

	FRT_CORE_API ID3D12Resource* CreateBufferAsset(const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState, void* BufferData);
	FRT_CORE_API ID3D12Resource* CreateTextureAsset(const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState, void* Texels);
	FRT_CORE_API void CreateShaderResourceView(ID3D12Resource* Texture, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle);

	FRT_CORE_API void CopyDataToBuffer(
		D3D12_RESOURCE_STATES StartState,
		D3D12_RESOURCE_STATES EndState,
		void* Data, uint64 DataSize,
		ID3D12Resource* GpuBuffer);

private:
	void CreateSwapChain();
	void FlushCommandQueue();

public:
	static constexpr unsigned FrameBufferSize = 2;

	DX12_DescriptorHeap ShaderDescriptorHeap;

private:
	Window* _window;

	D3D12_VIEWPORT Viewport;
	D3D12_RECT ScissorRect;

	// Pipeline
	IDXGIAdapter1* _adapter;
	ID3D12Device* _device;
	IDXGIFactory4* Factory;

	ComPtr<IDXGISwapChain1> _swapChain;
	ComPtr<ID3D12Resource> _frameBuffer[FrameBufferSize];
	D3D12_CPU_DESCRIPTOR_HANDLE _frameBufferDescriptors[FrameBufferSize];
	unsigned _currentFrameBufferIndex;

	ID3D12CommandQueue* _commandQueue;
	ID3D12CommandAllocator* _commandAllocator;
	ID3D12GraphicsCommandList* _commandList;

	ID3D12RootSignature* _rootSignature;
	ID3D12PipelineState* _pipelineState;
	// temp
	ID3D12Resource* _vertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
	math::STransform _transformTemp;

	ID3D12Resource* _transformBuffer;
	D3D12_GPU_DESCRIPTOR_HANDLE _transformBufferDescriptor;
	// ~temp

	ID3D12Resource* CommonConstantBuffer;
	D3D12_GPU_DESCRIPTOR_HANDLE CommonConstantBufferDescriptor;

	DX12_DescriptorHeap _rtvHeap;
	DX12_Arena _rtvArena;
	DX12_UploadArena _uploadArena;
	DX12_Arena _bufferArena;
	DX12_Arena _textureArena;

	DX12_DescriptorHeap _dsvHeap;
	ComPtr<ID3D12Resource> _depthStencilBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE _depthStencilDescriptor;
	// ~Pipeline

	// Synchronization
	ID3D12Fence* _fence;
	unsigned long long _fenceValue;
	HANDLE _fenceEvent;
	// ~Synchronization
};

	inline static uint32 Dx12GetBytesPerPixel(DXGI_FORMAT Format)
	{
		switch (Format)
		{
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return 16u;

		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return 8u;

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
		case DXGI_FORMAT_R32_SINT:
			return 4u;

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
		case DXGI_FORMAT_R16_SINT:
			return 2u;

		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
		case DXGI_FORMAT_A8_UNORM:
		default: 
			return 1u;
		}
	}
}
