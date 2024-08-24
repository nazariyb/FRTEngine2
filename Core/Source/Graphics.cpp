#include "Graphics.h"

#include "Window.h"

NAMESPACE_FRT_START

static void GetHardwareAdapter(IDXGIFactory4* pFactory, IDXGIAdapter1** ppAdapter)
{
	*ppAdapter = nullptr;
	for (UINT adapterIndex = 0; ; ++adapterIndex)
	{
		IDXGIAdapter1* pAdapter = nullptr;
		if (DXGI_ERROR_NOT_FOUND == pFactory->EnumAdapters1(adapterIndex, &pAdapter))
		{
			// No more adapters to enumerate.
			break;
		}

		// Check to see if the adapter supports Direct3D 12, but don't create the
		// actual device yet.
		if (SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
		{
			*ppAdapter = pAdapter;
			return;
		}
		pAdapter->Release();
	}
}


Graphics::Graphics(Window* Window)
	: _window(Window)
	, _adapter(nullptr)
	, _device(nullptr)
	, _swapChain(nullptr)
	, _currentFrameBufferIndex(0)
	, _commandQueue(nullptr)
	, _commandAllocator(nullptr)
	, _commandList(nullptr)
	, _fenceValue(0)
	, _fenceEvent(nullptr)
{
#if defined(DEBUG) || defined(_DEBUG)
	// Enable the D3D12 debug layer.
	{
		ID3D12Debug* debugController = nullptr;
		/*ThrowIfFailed*/(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
	}
#endif

	IDXGIFactory4* factory = nullptr;
	CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	// ThrowIfFailed

	GetHardwareAdapter(factory, &_adapter);

	D3D12CreateDevice(_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device));

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	/*THROW_IF_FAILED*/
	(_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue)));

	// Describe and create swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameBufferSize;
	swapChainDesc.Width = 800;
	swapChainDesc.Height = 600;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	// Throw if failed
	factory->CreateSwapChainForHwnd(
		_commandQueue, _window->GetHandle(), &swapChainDesc, nullptr, nullptr, &_swapChain);

	for (UINT frameIndex = 0; frameIndex < FrameBufferSize; ++frameIndex)
	{
		_swapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&_frameBuffer[frameIndex]));
	}

	// Command allocator and list

	/*Throw*/
	_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_commandAllocator));
	/*Throw*/
	_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _commandAllocator, 0, IID_PPV_ARGS(&_commandList));
	/*Throw*/
	_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));

	{
		// Describe and create a render target view (RTV) descriptor heap.
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = FrameBufferSize;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		/*THROW_IF_FAILED*/
		(_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_rtvHeap)));

		_rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		for (unsigned frameIndex = 0; frameIndex < FrameBufferSize; ++frameIndex)
		{
			if (frameIndex == 0)
			{
				_frameBufferDescriptors[frameIndex] = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
			}
			else
			{
				_frameBufferDescriptors[frameIndex].ptr = _frameBufferDescriptors[frameIndex - 1].ptr +
					_rtvDescriptorSize;
			}
			_device->CreateRenderTargetView(_frameBuffer[frameIndex], nullptr, _frameBufferDescriptors[frameIndex]);
		}
	}
}

void Graphics::Draw()
{
	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = _frameBuffer[_currentFrameBufferIndex];
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	FLOAT Color[] = { 0.1f, 0.2f, 0.3f, 1.f };
	_commandList->ClearRenderTargetView(_frameBufferDescriptors[_currentFrameBufferIndex], Color, 0,  nullptr);

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = _frameBuffer[_currentFrameBufferIndex];
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	_commandList->Close();
	_commandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&_commandList);

	_swapChain->Present(1, 0);

	_fenceValue++;
	_commandQueue->Signal(_fence, _fenceValue);

	if (_fence->GetCompletedValue() < _fenceValue)
	{
		_fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
		WaitForSingleObject(_fenceEvent, INFINITE);
	}

	/*Throw*/_commandAllocator->Reset();
	/*Throw*/_commandList->Reset(_commandAllocator, nullptr);

	_currentFrameBufferIndex = (_currentFrameBufferIndex + 1) % FrameBufferSize;
}

NAMESPACE_FRT_END
