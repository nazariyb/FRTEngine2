#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include "Core.h"


namespace frt
{
	class Window;
}

NAMESPACE_FRT_START
	class FRT_CORE_API Graphics
{
public:
	Graphics() = delete;

	explicit Graphics(Window* Window);

	void Draw();

private:
	static constexpr unsigned FrameBufferSize = 2;

	Window* _window;

	// Pipeline
	IDXGIAdapter1* _adapter;
	ID3D12Device* _device;

	IDXGISwapChain1* _swapChain;
	ID3D12Resource* _frameBuffer[FrameBufferSize];
	D3D12_CPU_DESCRIPTOR_HANDLE _frameBufferDescriptors[FrameBufferSize];
	unsigned _currentFrameBufferIndex;

	ID3D12CommandQueue* _commandQueue;
	ID3D12CommandAllocator* _commandAllocator;
	ID3D12GraphicsCommandList* _commandList;

	ID3D12DescriptorHeap* _rtvHeap;
	unsigned _rtvDescriptorSize;
	// ~Pipeline

	// Synchronization
	ID3D12Fence* _fence;
	unsigned long long _fenceValue;
	HANDLE _fenceEvent;
	// ~Synchronization
};

NAMESPACE_FRT_END
