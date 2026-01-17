#include "Renderer.h"

#include <complex>

#include "Graphics/Camera.h"
#include "d3dx12.h"
#include "Exception.h"
#include "Graphics/Model.h"
#include "Timer.h"
#include "Window.h"


namespace frt::graphics
{
static void GetHardwareAdapter (IDXGIFactory4* pFactory, IDXGIAdapter1** ppAdapter)
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

using namespace memory::literals;

CRenderer::CRenderer (CWindow* Window)
	: Window(Window)
	, Adapter(nullptr)
	, Device(nullptr)
	, SwapChain(nullptr)
	, CurrentBackBufferIndex(0)
	, CommandQueue(nullptr)
	, CommandAllocator(nullptr)
	, CommandList(nullptr)
	, FenceValue(0)
	, FenceEvent(nullptr)
{
#if defined(DEBUG) || defined(_DEBUG)
	// Enable the D3D12 debug layer.
	{
		ID3D12Debug* debugController = nullptr;
		THROW_IF_FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
	}
#endif

	const auto [renderWidth, renderHeight] = Window->GetWindowSize();

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

	GetHardwareAdapter(Factory.Get(), &Adapter);

	if (FAILED(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device))))
	{
		IDXGIAdapter* pWarpAdapter = nullptr;
		THROW_IF_FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));
		THROW_IF_FAILED(D3D12CreateDevice(pWarpAdapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)));
	}

	// Check supported features

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisampleQualityLevels = {};
	multisampleQualityLevels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	multisampleQualityLevels.SampleCount = 4;
	multisampleQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	multisampleQualityLevels.NumQualityLevels = 0;
	THROW_IF_FAILED(
		Device->CheckFeatureSupport(
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

	THROW_IF_FAILED(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CommandQueue)));

	// Command allocator and list

	THROW_IF_FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CommandAllocator)));
	THROW_IF_FAILED(
		Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&
			CommandList)));
	THROW_IF_FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
	CommandList->Close();

	// Describe and create swap chain
	CreateSwapChain(false); // TODO: create & use startup display settings

	RtvArena = DX12_Arena(
		Device.Get(), D3D12_HEAP_TYPE_DEFAULT, 50_Mb,
		D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES);
	DsvHeap = DX12_DescriptorHeap(Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);

	BufferArena = DX12_Arena(
		Device.Get(), D3D12_HEAP_TYPE_DEFAULT, 500_Mb,
		D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
	TextureArena = DX12_Arena(
		Device.Get(), D3D12_HEAP_TYPE_DEFAULT, 500_Mb,
		D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);

	ShaderDescriptorHeap = DX12_DescriptorHeap(
		Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 50, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	RtvHeap = DX12_DescriptorHeap(
		Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, render::constants::FrameResourcesBufferCount);

	for (uint8 i = 0; i < render::constants::FrameResourcesBufferCount; ++i)
	{
		FramesResources[i].Init(Device.Get(), 1, 2, BufferArena, GetDescriptorHeap());
	}

	for (unsigned frameIndex = 0; frameIndex < render::constants::SwapChainBufferCount; ++frameIndex)
	{
		RtvHeap.Allocate(&FrameBufferDescriptors[frameIndex], nullptr);
	}

	DsvHeap.Allocate(&DepthStencilDescriptor, nullptr);

	CreateRootSignature();
	CreatePipelineState();

	MaterialLibrary.SetRenderer(this);
}

void CRenderer::CreateRootSignature ()
{
	CD3DX12_ROOT_PARAMETER rootParameters[3];

	CD3DX12_DESCRIPTOR_RANGE texTable0(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	rootParameters[0].InitAsDescriptorTable(1, &texTable0);

	CD3DX12_DESCRIPTOR_RANGE objCbvTable0(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
	CD3DX12_DESCRIPTOR_RANGE passCbvTable0(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);

	rootParameters[1].InitAsDescriptorTable(1, &objCbvTable0);
	rootParameters[2].InitAsDescriptorTable(1, &passCbvTable0);

	constexpr D3D12_STATIC_SAMPLER_DESC samplerDesc
	{
		.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
		.ShaderRegister = 0,
		.RegisterSpace = 0,
		.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
	};

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(
		3, rootParameters, 1, &samplerDesc,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ID3DBlob* serializedRootSignature = nullptr;
	ID3DBlob* errorMessage = nullptr;

	THROW_IF_FAILED(
		D3D12SerializeRootSignature(
			&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serializedRootSignature, &errorMessage));

	THROW_IF_FAILED(
		Device->CreateRootSignature(
			0,
			serializedRootSignature->GetBufferPointer(),
			serializedRootSignature->GetBufferSize(),
			IID_PPV_ARGS(&RootSignature)));

	if (serializedRootSignature)
	{
		serializedRootSignature->Release();
	}
	if (errorMessage)
	{
		errorMessage->Release();
	}
}

void CRenderer::CreatePipelineState ()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = RootSignature.Get();

		const std::filesystem::path shaderSourceDir = R"(..\Core\Content\Shaders)";
		const std::filesystem::path shaderBinDir = shaderSourceDir / "Bin";
		const std::filesystem::path vertexShaderPath = shaderBinDir / "VertexShader.shader";
		const std::filesystem::path pixelShaderPath = shaderBinDir / "PixelShader.shader";
		const std::filesystem::path vertexShaderSourcePath = shaderSourceDir / "VertexShader.hlsl";
		const std::filesystem::path pixelShaderSourcePath = shaderSourceDir / "PixelShader.hlsl";

		const SShaderAsset* vertexShader = ShaderLibrary.LoadShader(
			"VertexShader",
			vertexShaderPath,
			vertexShaderSourcePath,
			shaderSourceDir,
			"main",
			"vs_6_0",
			EShaderStage::Vertex);
		const SShaderAsset* pixelShader = ShaderLibrary.LoadShader(
			"PixelShader",
			pixelShaderPath,
			pixelShaderSourcePath,
			shaderSourceDir,
			"main",
			"ps_6_0",
			EShaderStage::Pixel);

	psoDesc.VS = vertexShader->GetBytecode();
	psoDesc.PS = pixelShader->GetBytecode();

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
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.FrontCounterClockwise = false;
	psoDesc.RasterizerState.DepthClipEnable = true;

	psoDesc.DepthStencilState.DepthEnable = true;
	psoDesc.DepthStencilState.StencilEnable = false;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[5] = {};
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

	inputElementDescs[2].SemanticName = "NORMAL";
	inputElementDescs[2].SemanticIndex = 0;
	inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[2].InputSlot = 0;
	inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

	inputElementDescs[3].SemanticName = "TANGENT";
	inputElementDescs[3].SemanticIndex = 0;
	inputElementDescs[3].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[3].InputSlot = 0;
	inputElementDescs[3].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[3].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

	inputElementDescs[4].SemanticName = "COLOR";
	inputElementDescs[4].SemanticIndex = 0;
	inputElementDescs[4].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[4].InputSlot = 0;
	inputElementDescs[4].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[4].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

	psoDesc.InputLayout.pInputElementDescs = inputElementDescs;
	psoDesc.InputLayout.NumElements = 5;

	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

	THROW_IF_FAILED(Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&PipelineState)));
}

void CRenderer::Resize (bool bNewFullscreenState)
{
	// Flush before changing any resources.
	FlushCommandQueue();

	THROW_IF_FAILED(CommandList->Reset(CommandAllocator.Get(), nullptr));

	// Release the previous resources we will be recreating.
	for (int i = 0; i < render::constants::SwapChainBufferCount; ++i)
	{
		FrameBuffer[i].Reset();
	}
	DepthStencilBuffer.Reset();

	// start logic

	BOOL bWasInFullscreen = false;
	if (SwapChain)
	{
		THROW_IF_FAILED(SwapChain->GetFullscreenState(&bWasInFullscreen, nullptr));
	}

	const bool bExitingFullscreen = bWasInFullscreen && !bNewFullscreenState;
	const bool bEnteringFullscreen = !bWasInFullscreen && bNewFullscreenState;

	Vector2f drawRect = Window->GetWindowSize();

	if (SwapChain && bExitingFullscreen)
	{
		SwapChain->SetFullscreenState(bNewFullscreenState, nullptr);
	}

	if (drawRect.x > 0.f && drawRect.y > 0.f)
	{
		if (!SwapChain || (bWasInFullscreen != bNewFullscreenState))
		{
			CreateSwapChain(bNewFullscreenState);
		}
		else
		{
			// Resize the swap chain.
			THROW_IF_FAILED(
				SwapChain->ResizeBuffers(
					render::constants::SwapChainBufferCount,
					drawRect.x, drawRect.y,
					DXGI_FORMAT_R8G8B8A8_UNORM,
					DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
		}

		CurrentBackBufferIndex = 0;

		for (unsigned frameIndex = 0; frameIndex < render::constants::SwapChainBufferCount; ++frameIndex)
		{
			SwapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&FrameBuffer[frameIndex]));
			Device->CreateRenderTargetView(
				FrameBuffer[frameIndex].Get(), nullptr, FrameBufferDescriptors[frameIndex]);
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
				.SampleDesc = DXGI_SAMPLE_DESC{ .Count = 1, .Quality = 0 },
				// TODO
				.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
				.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
			};

			D3D12_CLEAR_VALUE clearValue =
			{
				.Format = resourceDesc.Format,
				.DepthStencil = D3D12_DEPTH_STENCIL_VALUE{ .Depth = 1.0f, .Stencil = 0 },
			};

			RtvArena.Free();
			DepthStencilBuffer = RtvArena.Allocate(resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue);

			Device->CreateDepthStencilView(DepthStencilBuffer.Get(), nullptr, DepthStencilDescriptor);
		}
	}
	else
	{
		SwapChain->SetFullscreenState(bNewFullscreenState, nullptr);
		SwapChain.Reset();
	}

	// Execute the resize commands.
	THROW_IF_FAILED(CommandList->Close());
	ID3D12CommandList* cmdsLists[] = { CommandList.Get() };
	CommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

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

void CRenderer::StartFrame (CCamera& Camera)
{
	auto commandListAllocator = GetCurrentFrameResource().CommandListAllocator;

	THROW_IF_FAILED(commandListAllocator->Reset());

	// TODO: assign different PSO
	THROW_IF_FAILED(CommandList->Reset(commandListAllocator.Get(), nullptr));

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = FrameBuffer[CurrentBackBufferIndex].Get();
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		CommandList->ResourceBarrier(1, &resourceBarrier);
	}

	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &ScissorRect);

	FLOAT Color[] = { 0.1f, 0.3f, 0.3f, 1.f };
	CommandList->ClearRenderTargetView(FrameBufferDescriptors[CurrentBackBufferIndex], Color, 0, nullptr);
	CommandList->ClearDepthStencilView(
		DepthStencilDescriptor, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

	CommandList->OMSetRenderTargets(
		1, &FrameBufferDescriptors[CurrentBackBufferIndex], false, &DepthStencilDescriptor);

	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(PipelineState.Get());

	CommandList->SetDescriptorHeaps(1, &ShaderDescriptorHeap._heap);
}

void CRenderer::Tick (float DeltaSeconds)
{
	CurrentFrameResourceIndex = (CurrentFrameResourceIndex + 1) % render::constants::FrameResourcesBufferCount;

	if (GetCurrentFrameResource().FenceValue != 0 && Fence->GetCompletedValue() < GetCurrentFrameResource().FenceValue)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
		THROW_IF_FAILED(Fence->SetEventOnCompletion(GetCurrentFrameResource().FenceValue, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

#if !defined(RELEASE)
	MaterialLibrary.ReloadModifiedMaterials();
	if (ShaderLibrary.ReloadModifiedShaders())
	{
		CreatePipelineState();
	}
#endif
}

void CRenderer::Draw (float DeltaSeconds, CCamera& Camera)
{
	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = FrameBuffer[CurrentBackBufferIndex].Get();
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		CommandList->ResourceBarrier(1, &resourceBarrier);
	}

	THROW_IF_FAILED(CommandList->Close());
	CommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)CommandList.GetAddressOf());

	SwapChain->Present(1, 0);
	CurrentBackBufferIndex = (CurrentBackBufferIndex + 1) % render::constants::FrameResourcesBufferCount;

	// FlushCommandQueue();

	++FenceValue;
	GetCurrentFrameResource().FenceValue = FenceValue;
	CommandQueue->Signal(Fence.Get(), FenceValue);

	GetCurrentFrameResource().UploadArena.Clear();
}

IDXGIAdapter1* CRenderer::GetAdapter ()
{
	return Adapter.Get();
}

ID3D12Device* CRenderer::GetDevice ()
{
	return Device.Get();
}

ID3D12CommandQueue* CRenderer::GetCommandQueue ()
{
	return CommandQueue.Get();
}

ID3D12GraphicsCommandList* CRenderer::GetCommandList ()
{
	return CommandList.Get();
}

DX12_Arena& CRenderer::GetBufferArena ()
{
	return BufferArena;
}

DX12_DescriptorHeap& CRenderer::GetDescriptorHeap ()
{
	return ShaderDescriptorHeap;
}

SFrameResources& CRenderer::GetCurrentFrameResource ()
{
	return FramesResources[CurrentFrameResourceIndex];
}

CMaterialLibrary& CRenderer::GetMaterialLibrary ()
{
	return MaterialLibrary;
}

void CRenderer::EnsureObjectConstantCapacity (uint32 ObjectCount)
{
	if (ObjectCount == 0u)
	{
		return;
	}

	const uint32 currentObjectCount = FramesResources[0].ObjectCB.ObjectCount;
	if (ObjectCount <= currentObjectCount)
	{
		return;
	}

	const uint32 frameCount = render::constants::FrameResourcesBufferCount;
	const uint32 requiredTotal =
		static_cast<uint32>(ShaderDescriptorHeap.GetCount()) + frameCount * ObjectCount;

	EnsureShaderDescriptorCapacity(requiredTotal);

	for (uint32 i = 0; i < render::constants::FrameResourcesBufferCount; ++i)
	{
		FramesResources[i].EnsureObjectCapacity(Device.Get(), ObjectCount, BufferArena, ShaderDescriptorHeap);
	}
}

void CRenderer::EnsureShaderDescriptorCapacity (uint32 RequiredCount)
{
	const uint32 currentCapacity = static_cast<uint32>(ShaderDescriptorHeap.GetCapacity());
	if (RequiredCount <= currentCapacity)
	{
		return;
	}

	const uint32 grownCapacity = static_cast<uint32>(currentCapacity == 0u ? 1u : currentCapacity);
	uint32 newCapacity = grownCapacity;
	while (newCapacity < RequiredCount)
	{
		newCapacity *= 2u;
	}

	RebuildShaderDescriptorHeap(newCapacity);
}

void CRenderer::RebuildShaderDescriptorHeap (uint32 NewCapacity)
{
	FlushCommandQueue();

	ShaderDescriptorHeap = DX12_DescriptorHeap(
		Device.Get(),
		ShaderDescriptorHeapType,
		NewCapacity,
		ShaderDescriptorHeapFlags);

	RebuildShaderDescriptors();

	OnShaderDescriptorHeapRebuild.Invoke();
}

void CRenderer::RebuildShaderDescriptors ()
{
	for (uint32 i = 0; i < render::constants::FrameResourcesBufferCount; ++i)
	{
		FramesResources[i].PassCB.RebuildDescriptors(Device.Get(), ShaderDescriptorHeap);
		FramesResources[i].ObjectCB.RebuildDescriptors(Device.Get(), ShaderDescriptorHeap);
	}

	for (uint32 i = 0; i < TrackedSrvs.Count(); ++i)
	{
		SShaderResourceViewRecord& record = TrackedSrvs[i];
		if (!record.Resource)
		{
			continue;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
		ShaderDescriptorHeap.Allocate(&cpuHandle, &gpuHandle);

		if (record.GpuHandle)
		{
			*record.GpuHandle = gpuHandle;
		}

		Device->CreateShaderResourceView(record.Resource, &record.Desc, cpuHandle);
	}
}

ID3D12Resource* CRenderer::CreateBufferAsset (
	const D3D12_RESOURCE_DESC& Desc,
	D3D12_RESOURCE_STATES InitialState,
	void* BufferData)
{
	uint64 uploadOffset = 0;
	uint8* dest = GetCurrentFrameResource().UploadArena.Allocate(Desc.Width, &uploadOffset);
	memcpy(dest, BufferData, Desc.Width);

	ID3D12Resource* resource = BufferArena.Allocate(Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
	CommandList->CopyBufferRegion(
		resource, 0, GetCurrentFrameResource().UploadArena.GetGPUBuffer(), uploadOffset, Desc.Width);

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = resource;
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		resourceBarrier.Transition.StateAfter = InitialState;
		CommandList->ResourceBarrier(1, &resourceBarrier);
	}

	return resource;
}

ID3D12Resource* CRenderer::CreateTextureAsset (
	const D3D12_RESOURCE_DESC& Desc,
	D3D12_RESOURCE_STATES InitialState,
	void* Texels)
{
	D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = Device->GetResourceAllocationInfo(0, 1, &Desc);
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

		uint8* destTexels = GetCurrentFrameResource().UploadArena.Allocate(
			footprint.Height * footprint.RowPitch, &placedFootprint.Offset);

		for (uint32 Y = 0; Y < Desc.Height; ++Y)
		{
			auto src = reinterpret_cast<uint8*>(reinterpret_cast<uint64>(Texels) + ((uint64)Y * (uint64)Desc.Width) * (
													uint64)bytesPerPixel);
			auto dest = reinterpret_cast<uint8*>(
				reinterpret_cast<uint64>(destTexels) + (uint64)Y * (uint64)footprint.RowPitch);
			memcpy(dest, src, Desc.Width * bytesPerPixel);
		}
	}

	ID3D12Resource* resoruce = TextureArena.Allocate(Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

	{
		D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
		srcLocation.pResource = GetCurrentFrameResource().UploadArena.GetGPUBuffer();
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint = placedFootprint;

		D3D12_TEXTURE_COPY_LOCATION destLocation = {};
		destLocation.pResource = resoruce;
		destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destLocation.SubresourceIndex = 0;

		CommandList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
	}

	{
		D3D12_RESOURCE_BARRIER resourceBarrier = {};
		resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		resourceBarrier.Transition.pResource = resoruce;
		resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		resourceBarrier.Transition.StateAfter = InitialState;
		CommandList->ResourceBarrier(1, &resourceBarrier);
	}

	return resoruce;
}

void CRenderer::CreateShaderResourceView (
	ID3D12Resource* Texture,
	const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc,
	D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle)
{
	EnsureShaderDescriptorCapacity(static_cast<uint32>(ShaderDescriptorHeap.GetCount() + 1u));

	ShaderDescriptorHeap.Allocate(OutCpuHandle, OutGpuHandle);
	Device->CreateShaderResourceView(Texture, &Desc, *OutCpuHandle);

	if (OutGpuHandle)
	{
		SShaderResourceViewRecord& record = TrackedSrvs.Add();
		record.Resource = Texture;
		record.Desc = Desc;
		record.GpuHandle = OutGpuHandle;
	}
}

void CRenderer::CreateSwapChain (bool bFullscreen)
{
	SwapChain.Reset();

	Vector2f drawRect = Window->GetWindowSize();

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc =
	{
		.Width = static_cast<unsigned int>(drawRect.x),
		.Height = static_cast<unsigned int>(drawRect.y),
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.Stereo = false,
		.SampleDesc = DXGI_SAMPLE_DESC{ .Count = 1, .Quality = 0 },
		// TODO: use sampling
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = render::constants::SwapChainBufferCount,
		.Scaling = DXGI_SCALING_NONE,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
		.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH,
	};

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc =
	{
		.RefreshRate = { 0, 1 },
		// TODO
		.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
		.Scaling = DXGI_MODE_SCALING_UNSPECIFIED,
		.Windowed = !bFullscreen,
	};

	THROW_IF_FAILED(
		Factory->CreateSwapChainForHwnd(
			CommandQueue.Get(), Window->GetHandle(), &swapChainDesc, &fullscreenDesc, nullptr, &SwapChain));
}

void CRenderer::FlushCommandQueue ()
{
	FenceValue++;
	CommandQueue->Signal(Fence.Get(), FenceValue);

	if (Fence->GetCompletedValue() < FenceValue)
	{
		Fence->SetEventOnCompletion(FenceValue, FenceEvent);
		WaitForSingleObject(FenceEvent, INFINITE);
	}
}
}
