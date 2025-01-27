#include "Renderer.h"

#include <complex>
#include <DirectXMath.h>
#include <iostream>

#include "Camera.h"
#include "Exception.h"
#include "GameInstance.h"
#include "GraphicsUtility.h"
#include "Model.h"
#include "Timer.h"
#include "Window.h"
#include "Math/Transform.h"
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

Renderer::Renderer(Window* Window)
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
		THROW_IF_FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
	}
#endif

	const auto [renderWidth, renderHeight] = _window->GetWindowSize();

	Viewport =
		{
			.TopLeftX = 0,
			.TopLeftY = 0,
			.Width = static_cast<float>(renderWidth),
			.Height = static_cast<float>(renderHeight),
			.MinDepth = 0.0f,
			.MaxDepth = 1.0f
		};

	ScissorRect =
		{
			.left = 0,
			.top = 0,
			.right = static_cast<long>(renderWidth),
			.bottom = static_cast<long>(renderHeight)
		};

	// Initialization

	THROW_IF_FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory)));

	GetHardwareAdapter(Factory.Get(), &_adapter);

	if (FAILED(D3D12CreateDevice(_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device))))
	{
		IDXGIAdapter* pWarpAdapter = nullptr;
		THROW_IF_FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));
		THROW_IF_FAILED(D3D12CreateDevice(pWarpAdapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device)));
	}

	// Check supported features

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisampleQualityLevels = {};
	multisampleQualityLevels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	multisampleQualityLevels.SampleCount = 4;
	multisampleQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	multisampleQualityLevels.NumQualityLevels = 0;
	THROW_IF_FAILED(_device->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&multisampleQualityLevels,
		sizeof(multisampleQualityLevels)));

	// TODO: save as member
	uint32 msaaQuality = multisampleQualityLevels.NumQualityLevels;
	frt_assert(msaaQuality > 0);

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	THROW_IF_FAILED(_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue)));

	// Command allocator and list

	THROW_IF_FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_commandAllocator)));
	THROW_IF_FAILED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _commandAllocator.Get(), nullptr, IID_PPV_ARGS(&_commandList)));
	THROW_IF_FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)));
	_commandList->Close();

	// Describe and create swap chain
	CreateSwapChain();

	_rtvArena = DX12_Arena(_device.Get(), D3D12_HEAP_TYPE_DEFAULT, 50 * memory::MegaByte,
						D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES);
	_dsvHeap = DX12_DescriptorHeap(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);

	_uploadArena = DX12_UploadArena(_device.Get(), 1 * memory::GigaByte);
	_bufferArena = DX12_Arena(_device.Get(), D3D12_HEAP_TYPE_DEFAULT, 500 * memory::MegaByte,
							D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
	_textureArena = DX12_Arena(_device.Get(), D3D12_HEAP_TYPE_DEFAULT, 500 * memory::MegaByte,
								D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);

	ShaderDescriptorHeap = DX12_DescriptorHeap(
		_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 50, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	_rtvHeap = DX12_DescriptorHeap(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameBufferSize);

	for (unsigned frameIndex = 0; frameIndex < FrameBufferSize; ++frameIndex)
	{
		_rtvHeap.Allocate(&_frameBufferDescriptors[frameIndex], nullptr);
	}

	_dsvHeap.Allocate(&_depthStencilDescriptor, nullptr);

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

			THROW_IF_FAILED(D3D12SerializeRootSignature(
				&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serializedRootSignature, &errorMessage));

			THROW_IF_FAILED(_device->CreateRootSignature(
				0,
				serializedRootSignature->GetBufferPointer(),
				serializedRootSignature->GetBufferSize(),
				IID_PPV_ARGS(&_rootSignature)));

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
		psoDesc.pRootSignature = _rootSignature.Get();

		psoDesc.VS = Dx12LoadShader(R"(..\Core\Content\Shaders\Bin\VertexShader.shader)");
		psoDesc.PS = Dx12LoadShader(R"(..\Core\Content\Shaders\Bin\PixelShader.shader)");

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
		psoDesc.DepthStencilState.StencilEnable = false;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

		D3D12_INPUT_ELEMENT_DESC inputElementDescs[2] = {};
		inputElementDescs[0].SemanticName = "POSITION";
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

		THROW_IF_FAILED(_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState)));
	}

	{
		D3D12_RESOURCE_DESC cbDesc = {};
		cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cbDesc.Width = AlignAddress(sizeof(math::STransform), 256);
		cbDesc.Height = 1;
		cbDesc.DepthOrArraySize = 1;
		cbDesc.MipLevels = 1;
		cbDesc.Format = DXGI_FORMAT_UNKNOWN;
		cbDesc.SampleDesc.Count = 1;
		cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		_transformBuffer = _bufferArena.Allocate(cbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbViewDesc = {};
		cbViewDesc.BufferLocation = _transformBuffer->GetGPUVirtualAddress();
		cbViewDesc.SizeInBytes = (uint32)cbDesc.Width * cbDesc.Height * cbDesc.DepthOrArraySize;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuDesc = {};
		ShaderDescriptorHeap.Allocate(&cpuDesc, &_transformBufferDescriptor);
		_device->CreateConstantBufferView(&cbViewDesc, cpuDesc);
	}

	{
		D3D12_RESOURCE_DESC cbDesc = {};
		cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cbDesc.Width = AlignAddress(sizeof(math::STransform), 256);
		cbDesc.Height = 1;
		cbDesc.DepthOrArraySize = 1;
		cbDesc.MipLevels = 1;
		cbDesc.Format = DXGI_FORMAT_UNKNOWN;
		cbDesc.SampleDesc.Count = 1;
		cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		CommonConstantBuffer = _bufferArena.Allocate(cbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbViewDesc = {};
		cbViewDesc.BufferLocation = CommonConstantBuffer->GetGPUVirtualAddress();
		cbViewDesc.SizeInBytes = (uint32)cbDesc.Width * cbDesc.Height * cbDesc.DepthOrArraySize;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuDesc = {};
		ShaderDescriptorHeap.Allocate(&cpuDesc, &CommonConstantBufferDescriptor);
		_device->CreateConstantBufferView(&cbViewDesc, cpuDesc);
	}
}

void Renderer::Resize()
{
	// Flush before changing any resources.
	FlushCommandQueue();

	THROW_IF_FAILED(_commandList->Reset(_commandAllocator.Get(), nullptr));

	// Release the previous resources we will be recreating.
	for (int i = 0; i < FrameBufferSize; ++i)
	{
		_frameBuffer[i].Reset();
	}

	_depthStencilBuffer.Reset();

	Vector2f drawRect = _window->GetWindowSize();

	// Resize the swap chain.
	THROW_IF_FAILED(_swapChain->ResizeBuffers(
		FrameBufferSize, 
		drawRect.x, drawRect.y, 
		DXGI_FORMAT_R8G8B8A8_UNORM, 
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

	_currentFrameBufferIndex = 0;

	for (unsigned frameIndex = 0; frameIndex < FrameBufferSize; ++frameIndex)
	{
		_swapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&_frameBuffer[frameIndex]));
		_device->CreateRenderTargetView(_frameBuffer[frameIndex].Get(), nullptr, _frameBufferDescriptors[frameIndex]);
	}

	{
		// depth stencil

		D3D12_RESOURCE_DESC resourceDesc =
			{
				.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
				.Alignment = 0,
				.Width = static_cast<UINT>(drawRect.x),
				.Height = static_cast<UINT>(drawRect.y),
				.DepthOrArraySize = 1,
				.MipLevels = 1,
				.Format = DXGI_FORMAT_D32_FLOAT,
				.SampleDesc = DXGI_SAMPLE_DESC{ .Count = 1, .Quality = 0 }, // TODO
				.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
				.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
			};

		D3D12_CLEAR_VALUE clearValue =
			{
				.Format = resourceDesc.Format,
				.DepthStencil = D3D12_DEPTH_STENCIL_VALUE{ .Depth = 1.0f, .Stencil = 0 },
			};

		_rtvArena.Free();
		_depthStencilBuffer = _rtvArena.Allocate(resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue);

		_device->CreateDepthStencilView(_depthStencilBuffer.Get(), nullptr, _depthStencilDescriptor);
	}

	// Execute the resize commands.
	THROW_IF_FAILED(_commandList->Close());
	ID3D12CommandList* cmdsLists[] = { _commandList.Get() };
	_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until resize is complete.
	FlushCommandQueue();

	// Update the viewport transform to cover the client area.
	Viewport.TopLeftX = 0;
	Viewport.TopLeftY = 0;
	Viewport.Width = drawRect.x;
	Viewport.Height = drawRect.y;
	Viewport.MinDepth = 0.0f;
	Viewport.MaxDepth = 1.0f;

	ScissorRect.left = 0;
	ScissorRect.top = 0;
	ScissorRect.right = drawRect.x;
	ScissorRect.bottom = drawRect.y;
}

void Renderer::StartFrame(CCamera& Camera)
{
	THROW_IF_FAILED(_commandAllocator->Reset());
	THROW_IF_FAILED(_commandList->Reset(_commandAllocator.Get(), nullptr));

	const float TimeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	const double scale = std::cos(TimeElapsed) / 8.f + .25f;
	_transformTemp.SetScale(.5);
	_transformTemp.SetTranslation(.5f, 0.f, 0.f);
	_transformTemp.SetRotation(0.f, math::PI_OVER_FOUR * TimeElapsed, 0.f);

	const auto [renderWidth, renderHeight] = _window->GetWindowSize();

	DirectX::XMMATRIX view = Camera.GetViewMatrix();
	DirectX::XMMATRIX projection = Camera.GetProjectionMatrix(90.f, (float)renderWidth / renderHeight, 1.f, 10'000.f);

	DirectX::XMFLOAT4X4 mvp;
	DirectX::XMStoreFloat4x4(&mvp, DirectX::XMLoadFloat4x4(&_transformTemp.GetMatrix()) * view * projection);

	CopyDataToBuffer(
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		(void*)mvp.m,
		sizeof(math::STransform::RawType),
		_transformBuffer.Get()
		);

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = _frameBuffer[_currentFrameBufferIndex].Get();
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	_commandList->RSSetViewports(1, &Viewport);
	_commandList->RSSetScissorRects(1, &ScissorRect);

	FLOAT Color[] = { 0.1f, 0.3f, 0.3f, 1.f };
	_commandList->ClearRenderTargetView(_frameBufferDescriptors[_currentFrameBufferIndex], Color, 0, nullptr);
	_commandList->ClearDepthStencilView(
		_depthStencilDescriptor, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

	_commandList->OMSetRenderTargets(1, &_frameBufferDescriptors[_currentFrameBufferIndex], false, &_depthStencilDescriptor);

	_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	_commandList->SetGraphicsRootSignature(_rootSignature.Get());
	_commandList->SetPipelineState(_pipelineState.Get());

	_commandList->SetDescriptorHeaps(1, &ShaderDescriptorHeap._heap);
	_commandList->SetGraphicsRootDescriptorTable(1, _transformBufferDescriptor);
}

void Renderer::Draw(float DeltaSeconds, CCamera& Camera)
{
	// 

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = _frameBuffer[_currentFrameBufferIndex].Get();
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	THROW_IF_FAILED(_commandList->Close());
	_commandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)_commandList.GetAddressOf());

	_swapChain->Present(1, 0);
	_currentFrameBufferIndex = (_currentFrameBufferIndex + 1) % FrameBufferSize;

	FlushCommandQueue();

	_uploadArena.Clear();
}

IDXGIAdapter1* Renderer::GetAdapter()
{
	return _adapter.Get();
}

ID3D12Device* Renderer::GetDevice()
{
	return _device.Get();
}

ID3D12CommandQueue* Renderer::GetCommandQueue()
{
	return _commandQueue.Get();
}

ID3D12GraphicsCommandList* Renderer::GetCommandList()
{
	return _commandList.Get();
}

ID3D12Resource* Renderer::CreateBufferAsset(
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

ID3D12Resource* Renderer::CreateTextureAsset(
	const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState, void* Texels)
{
	D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = _device->GetResourceAllocationInfo(0, 1, &Desc);
	uint64 bytesPerPixel = Dx12GetBytesPerPixel(Desc.Format);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT placedFootprint;
	{
		frt_assert(Desc.DepthOrArraySize == 1);
		D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
		footprint.Format = Desc.Format;
		footprint.Width = (uint32)Desc.Width;
		footprint.Height = Desc.Height;
		footprint.Depth = 1;
		footprint.RowPitch = (uint32)AlignAddress(footprint.Width * bytesPerPixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
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

void Renderer::CreateShaderResourceView(ID3D12Resource* Texture, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc,
	D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
{
	ShaderDescriptorHeap.Allocate(OutCpuHandle, OutGpuHandle);
	_device->CreateShaderResourceView(Texture, &Desc, *OutCpuHandle);
}

void Renderer::CopyDataToBuffer(
	D3D12_RESOURCE_STATES StartState,
	D3D12_RESOURCE_STATES EndState,
	void* Data, uint64 DataSize,
	ID3D12Resource* GpuBuffer)
{
	uint64 uploadOffset = 0;
	{
		uint8* dest = _uploadArena.Allocate(DataSize, &uploadOffset);
		memcpy(dest, Data, DataSize);
	}

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = GpuBuffer;
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = StartState;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}

	_commandList->CopyBufferRegion(
		GpuBuffer, 0, _uploadArena.GetGPUBuffer(), uploadOffset, DataSize);

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = GpuBuffer;
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		resourceBarrier.Transition.StateAfter = EndState;
		_commandList->ResourceBarrier(1, &resourceBarrier);
	}
}

void Renderer::CreateSwapChain()
{
	_swapChain.Reset();

	Vector2f drawRect = _window->GetWindowSize();

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc =
		{
			.Width = static_cast<unsigned int>(drawRect.x),
			.Height = static_cast<unsigned int>(drawRect.y),
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.Stereo = false,
			.SampleDesc = DXGI_SAMPLE_DESC { .Count = 1, .Quality = 0 }, // TODO: use sampling
			.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
			.BufferCount = FrameBufferSize,
			.Scaling = DXGI_SCALING_NONE,
			.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
			.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
			.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH,
		};

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc =
		{
			.RefreshRate = 60, // TODO
			.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
			.Scaling = DXGI_MODE_SCALING_UNSPECIFIED,
			.Windowed = true, // TODO
		};

	THROW_IF_FAILED(Factory->CreateSwapChainForHwnd(
		_commandQueue.Get(), _window->GetHandle(), &swapChainDesc, &fullscreenDesc, nullptr, &_swapChain));
}

void Renderer::FlushCommandQueue()
{
	_fenceValue++;
	_commandQueue->Signal(_fence.Get(), _fenceValue);

	if (_fence->GetCompletedValue() < _fenceValue)
	{
		_fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
		WaitForSingleObject(_fenceEvent, INFINITE);
	}
}
}
