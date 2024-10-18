#include "Graphics.h"

#include "GraphicsUtility.h"
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
	, _vertexBufferView()
	, _fenceValue(0)
	, _fenceEvent(nullptr)
{
#if defined(DEBUG) || defined(_DEBUG)
	// Enable the D3D12 debug layer.
	{
		ID3D12Debug* debugController = nullptr;
		/*ThrowIfFailed*/
		(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
	}
#endif

	_renderWidth = Window->GetWindowSize().x;
	_renderHeight = Window->GetWindowSize().y;

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
		_rtvArena = DX12_Arena(_device, D3D12_HEAP_TYPE_DEFAULT, 50 * memory::MegaByte,
								D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES);
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

		_dsvHeap.Allocate(&_depthStencilDescriptor, nullptr);
		_device->CreateDepthStencilView(_depthStencilBuffer, nullptr, _depthStencilDescriptor);
	}

	_uploadArena = DX12_UploadArena(_device, 1 * memory::GigaByte);
	_bufferArena = DX12_Arena(_device, D3D12_HEAP_TYPE_DEFAULT, 500 * memory::MegaByte,
							D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
	_textureArena = DX12_Arena(_device, D3D12_HEAP_TYPE_DEFAULT, 500 * memory::MegaByte,
								D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);

	_shaderDescriptorHeap = DX12_DescriptorHeap(
		_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 50, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	{
		_rtvHeap = DX12_DescriptorHeap(_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameBufferSize);

		for (unsigned frameIndex = 0; frameIndex < FrameBufferSize; ++frameIndex)
		{
			_rtvHeap.Allocate(&_frameBufferDescriptors[frameIndex], nullptr);
			_device->CreateRenderTargetView(_frameBuffer[frameIndex], nullptr, _frameBufferDescriptors[frameIndex]);
		}
	}

	{
		{
			D3D12_ROOT_PARAMETER rootParameters[2];

			D3D12_DESCRIPTOR_RANGE tableRange1[1] = {};
			{
				tableRange1[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				tableRange1[0].NumDescriptors = 1;
				tableRange1[0].BaseShaderRegister = 0;
				tableRange1[0].RegisterSpace = 0;
				tableRange1[0].OffsetInDescriptorsFromTableStart = 0;

				rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
				rootParameters[0].DescriptorTable.pDescriptorRanges = tableRange1;
				rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}

			D3D12_DESCRIPTOR_RANGE tableRange2[1] = {};
			{
				tableRange2[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
				tableRange2[0].NumDescriptors = 1;
				tableRange2[0].BaseShaderRegister = 0;
				tableRange2[0].RegisterSpace = 0;
				tableRange2[0].OffsetInDescriptorsFromTableStart = 0;

				rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
				rootParameters[1].DescriptorTable.pDescriptorRanges = tableRange2;
				rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}

			D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
			{
				samplerDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
				samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
				samplerDesc.ShaderRegister = 0;
				samplerDesc.RegisterSpace = 0;
				samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}

			D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
			rootSignatureDesc.NumParameters = 2;
			rootSignatureDesc.pParameters = rootParameters;
			rootSignatureDesc.NumStaticSamplers = 1;
			rootSignatureDesc.pStaticSamplers = &samplerDesc;
			rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

			ID3DBlob* serializedRootSignature = nullptr;
			ID3DBlob* errorMessage = nullptr;
			/*Throw*/
			D3D12SerializeRootSignature(
				&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serializedRootSignature, &errorMessage);

			/*Throw*/
			_device->CreateRootSignature(
				0,
				serializedRootSignature->GetBufferPointer(),
				serializedRootSignature->GetBufferSize(),
				IID_PPV_ARGS(&_rootSignature));

			if (serializedRootSignature)
			{
				serializedRootSignature->Release();
			}
			if (errorMessage)
			{
				errorMessage->Release();
			}
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = _rootSignature;

		psoDesc.VS = Dx12LoadShader(R"(P:\Edu\FRTEngine2\Core\Content\Shaders\Bin\VertexShader.shader)");
		psoDesc.PS = Dx12LoadShader(R"(P:\Edu\FRTEngine2\Core\Content\Shaders\Bin\PixelShader.shader)");

		psoDesc.BlendState.RenderTarget[0].BlendEnable = true;
		psoDesc.BlendState.RenderTarget[0].LogicOpEnable = false;
		psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
		psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
		psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		psoDesc.RasterizerState.FrontCounterClockwise = false;
		psoDesc.RasterizerState.DepthClipEnable = true;

		psoDesc.DepthStencilState.DepthEnable = true;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

		D3D12_INPUT_ELEMENT_DESC inputElementDescs[2] = {};
		inputElementDescs[0].SemanticName = "SV_POSITION";
		inputElementDescs[0].SemanticIndex = 0;
		inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
		inputElementDescs[0].InputSlot = 0;
		inputElementDescs[0].AlignedByteOffset = 0;
		inputElementDescs[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

		inputElementDescs[1].SemanticName = "TEXCOORD";
		inputElementDescs[1].SemanticIndex = 0;
		inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
		inputElementDescs[1].InputSlot = 0;
		inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
		inputElementDescs[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

		psoDesc.InputLayout.pInputElementDescs = inputElementDescs;
		psoDesc.InputLayout.NumElements = 2;

		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		/*Throw*/
		_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
	}

	{
		D3D12_RESOURCE_DESC cbDesc = {};
		cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cbDesc.Width = AlignAddress(sizeof(Vertex/*TODO: actual matrix*/), 256);
		cbDesc.Height = 1;
		cbDesc.DepthOrArraySize = 1;
		cbDesc.MipLevels = 1;
		cbDesc.Format = DXGI_FORMAT_UNKNOWN;
		cbDesc.SampleDesc.Count = 1;
		cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		_transformBuffer = _bufferArena.Allocate(cbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbViewDesc = {};
		cbViewDesc.BufferLocation = _transformBuffer->GetGPUVirtualAddress();
		cbViewDesc.SizeInBytes = cbDesc.Width * cbDesc.Height * cbDesc.DepthOrArraySize;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuDesc = {};
		_shaderDescriptorHeap.Allocate(&cpuDesc, &_transformBufferDescriptor);
		_device->CreateConstantBufferView(&cbViewDesc, cpuDesc);
	}
}

void Graphics::LoadAssets()
{
	_models[0] = Model::LoadFromFile(R"(P:\Edu\FRTEngine2\Core\Content\Models\Skull\scene.gltf)");
	_models[1] = Model::CreateCube();

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

	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.Width = _renderWidth;
	viewport.Height = _renderHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	_commandList->RSSetViewports(1, &viewport);

	D3D12_RECT scissorRect = {};
	scissorRect.left = 0;
	scissorRect.right = _renderWidth;
	scissorRect.top = 0;
	scissorRect.bottom = _renderHeight;
	_commandList->RSSetScissorRects(1, &scissorRect);

	Model curModel = _models[0];
	{
		D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
		indexBufferView.BufferLocation = curModel.indexBuffer->GetGPUVirtualAddress();
		indexBufferView.SizeInBytes = curModel.indices.GetSize();
		indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		_commandList->IASetIndexBuffer(&indexBufferView);
	}
	{
		D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[1] = {};
		vertexBufferViews[0].BufferLocation = curModel.vertexBuffer->GetGPUVirtualAddress();
		vertexBufferViews[0].SizeInBytes = curModel.vertices.GetSize();
		vertexBufferViews[0].StrideInBytes = sizeof(Vertex);
		_commandList->IASetVertexBuffers(0, 1, vertexBufferViews);
	}

	_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_commandList->SetGraphicsRootSignature(_rootSignature);
	_commandList->SetPipelineState(_pipelineState);

	_commandList->SetDescriptorHeaps(1, &_shaderDescriptorHeap._heap);
	_commandList->SetGraphicsRootDescriptorTable(1, _transformBufferDescriptor);

	for (uint32 meshId = 0; meshId < curModel.meshes.GetNum(); ++meshId)
	{
		Mesh& mesh = curModel.meshes[meshId];
		Texture& texture = curModel.textures[mesh.textureIndex];

		_commandList->SetGraphicsRootDescriptorTable(0, texture.gpuDescriptor);
		_commandList->DrawIndexedInstanced(mesh.indexCount, 1, mesh.indexOffset, mesh.vertexOffset, 0);
	}

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = _frameBuffer[_currentFrameBufferIndex];
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	/*Throw*/_commandList->Close();
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

void Graphics::CreateShaderResourceView(ID3D12Resource* Texture, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc,
	D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
{
	_shaderDescriptorHeap.Allocate(OutCpuHandle, OutGpuHandle);
	_device->CreateShaderResourceView(Texture, &Desc, *OutCpuHandle);
}
}
