#include "Graphics.h"

#include "Model.h"
#include "Window.h"
#include "Memory/MemoryCoreTypes.h"

namespace frt::graphics
{

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
	, _vertexBuffer(nullptr)
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

	Vector2f drawRect = _window->GetWindowSize();

	// Describe and create swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameBufferSize;
	swapChainDesc.Width = static_cast<unsigned int>(drawRect.x);
	swapChainDesc.Height = static_cast<unsigned int>(drawRect.y);
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
		_rtvArena = DX12_Arena(_device, D3D12_HEAP_TYPE_DEFAULT, 50 * memory::MegaByte, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES);
		_dsvHeap = DX12_DescriptorHeap(_device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);

		D3D12_RESOURCE_DESC resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		resourceDesc.Width = static_cast<UINT>(drawRect.x);
		resourceDesc.Height = static_cast<UINT>(drawRect.y);
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_D32_FLOAT;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE clearValue = {};
		clearValue.Format = resourceDesc.Format;
		clearValue.DepthStencil.Depth = 1.0f;

		_depthStencilBuffer = _rtvArena.Allocate(resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue);

		_depthStencilDescriptor = _dsvHeap.Allocate();
		_device->CreateDepthStencilView(_depthStencilBuffer, nullptr, _depthStencilDescriptor);
	}

	_uploadArena = DX12_UploadArena(_device, 1 * memory::GigaByte);
	_bufferArena = DX12_Arena(_device, D3D12_HEAP_TYPE_DEFAULT, 500 * memory::MegaByte, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
	_textureArena = DX12_Arena(_device, D3D12_HEAP_TYPE_DEFAULT, 500 * memory::MegaByte, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);

	{
		_rtvHeap = DX12_DescriptorHeap(_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameBufferSize);

		for (unsigned frameIndex = 0; frameIndex < FrameBufferSize; ++frameIndex)
		{
			_frameBufferDescriptors[frameIndex] = _rtvHeap.Allocate();
			_device->CreateRenderTargetView(_frameBuffer[frameIndex], nullptr, _frameBufferDescriptors[frameIndex]);
		}
	}

	// TEMP: for test
	// struct Vertex
	// {
	// 	float Position[3];
	// 	float Color[4];
	// };
	//
	// const float aspectRatio = 1.f;
	// const Vertex vertices[] =
	// 	{
	// 	{ {   0.0f,  0.25f * aspectRatio, 0.f }, { 1.f, 0.f, 0.f, 1.f } },
	// 	{ {  0.25f, -0.25f * aspectRatio, 0.f }, { 1.f, 0.f, 0.f, 1.f } },
	// 	{ { -0.25f, -0.25f * aspectRatio, 0.f }, { 1.f, 0.f, 0.f, 1.f } },
	// };
	//
	// const uint32 vertexBufferSize = sizeof(Vertex) * 3;
	//
	// D3D12_HEAP_PROPERTIES heapProperties = {};
	// heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
	// heapProperties.CreationNodeMask = 1;
	// heapProperties.VisibleNodeMask = 1;
	// heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	// heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	//
	// D3D12_RESOURCE_DESC resourceDesc = {};
	// resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	// resourceDesc.Alignment = 0;
	// resourceDesc.Width = vertexBufferSize;
	// resourceDesc.Height = 1;
	// resourceDesc.DepthOrArraySize = 1;
	// resourceDesc.MipLevels = 1;
	// resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	// resourceDesc.SampleDesc.Count = 1;
	// resourceDesc.SampleDesc.Quality = 0;
	// resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	// resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	//
	// _device->CreateCommittedResource(
	// 	&heapProperties,
	// 	D3D12_HEAP_FLAG_NONE,
	// 	&resourceDesc,
	// 	D3D12_RESOURCE_STATE_GENERIC_READ,
	// 	nullptr,
	// 	IID_PPV_ARGS(&_vertexBuffer));
	//
	// uint8* vertexData;
	// D3D12_RANGE readRange = {};
	// readRange.Begin = 0;
	// readRange.End = 0;
	// _vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&vertexData));
	// memcpy(vertexData, vertices, vertexBufferSize);
	// _vertexBuffer->Unmap(0, nullptr);
	//
	// _vertexBufferView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
	// _vertexBufferView.StrideInBytes = sizeof(Vertex);
	// _vertexBufferView.SizeInBytes = vertexBufferSize;
	// ~TEMP: for test
}

void Graphics::LoadAssets()
{
	Model skull = Model::LoadFromFile("P:\\Edu\\FRTEngine2\\Core\\Content\\Models\\Skull\\scene.gltf");

	{
		_commandList->Close();
		_commandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&_commandList);

		_fenceValue++;
		_commandQueue->Signal(_fence, _fenceValue);

		if (_fence->GetCompletedValue() < _fenceValue)
		{
			_fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
			WaitForSingleObject(_fenceEvent, INFINITE);
		}

		/*Throw*/_commandAllocator->Reset();
		/*Throw*/_commandList->Reset(_commandAllocator, nullptr);

		_uploadArena.Clear();
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
	_commandList->ClearDepthStencilView(_depthStencilDescriptor, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);

	// TEMP: for test
	// _commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	// _commandList->IASetVertexBuffers(0, 1, &_vertexBufferView);
	// _commandList->DrawInstanced(3, 1, 0, 0);
	// ~TEMP: for test

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

ID3D12Resource* Graphics::CreateBufferAsset(
	const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState, void* BufferData)
{
	uint64 uploadOffset = 0;
	uint8* dest = _uploadArena.Allocate(Desc.Width, &uploadOffset);
	memcpy(dest, BufferData, Desc.Width);

	ID3D12Resource* resource = _bufferArena.Allocate(Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
	_commandList->CopyBufferRegion(resource, 0, _uploadArena.GetGPUBuffer(), uploadOffset, Desc.Width);

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = resource;
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		resourceBarrier.Transition.StateAfter = InitialState;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	return resource;
}

ID3D12Resource* Graphics::CreateTextureAsset(
	const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState, void* Texels)
{
	D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = _device->GetResourceAllocationInfo(0, 1, &Desc);
	uint64 bytesPerPixel = Dx12GetBytesPerPixel(Desc.Format);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT placedFootprint;
	{
		frt_assert(Desc.DepthOrArraySize == 1);
		D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
		footprint.Format = Desc.Format;
		footprint.Width = Desc.Width;
		footprint.Height = Desc.Height;
		footprint.Depth = 1;
		footprint.RowPitch = AlignAddress(footprint.Width * bytesPerPixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		placedFootprint.Footprint = footprint;

		uint8* destTexels = _uploadArena.Allocate(footprint.Height * footprint.RowPitch, &placedFootprint.Offset);

		for (uint32 Y = 0; Y < Desc.Height; ++Y)
		{
			uint8* src = reinterpret_cast<uint8*>(reinterpret_cast<uint64>(Texels) + ((uint64)Y * (uint64)Desc.Width) * (uint64)bytesPerPixel);
			uint8* dest = reinterpret_cast<uint8*>(reinterpret_cast<uint64>(destTexels) + (uint64)Y * (uint64)footprint.RowPitch);
			memcpy(dest, src, Desc.Width * bytesPerPixel);
		}
	}

	ID3D12Resource* resoruce = _textureArena.Allocate(Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

	{
		D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
		srcLocation.pResource = _uploadArena.GetGPUBuffer();
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint = placedFootprint;

		D3D12_TEXTURE_COPY_LOCATION destLocation = {};
		destLocation.pResource = resoruce;
		destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destLocation.SubresourceIndex = 0;

		_commandList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
	}

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = resoruce;
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		resourceBarrier.Transition.StateAfter = InitialState;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	return resoruce;
}
}
