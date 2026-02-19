#include "Renderer.h"

#include <complex>
#include <cstdio>
#include <execution>
#include <filesystem>
#include <utility>

#include "d3dx12.h"
#include "DXRHelper.h"
#include "Exception.h"
#include "Timer.h"
#include "Window.h"
#include "Graphics/Model.h"
#include "Graphics/Render/Material.h"


namespace frt::graphics
{
namespace
{
D3D12_STATIC_SAMPLER_DESC BuildLinearWrapStaticSamplerDesc ()
{
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias = 0.f;
	sampler.MaxAnisotropy = 1u;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	sampler.MinLOD = 0.f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0u;
	sampler.RegisterSpace = 0u;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	return sampler;
}
}

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

#pragma region Core
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

	HRESULT hr = D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&Device));
	if (FAILED(hr))
	{
		hr = D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&Device));
	}
	if (FAILED(hr))
	{
		hr = D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device));
	}
	if (FAILED(hr))
	{
		ComPtr<IDXGIAdapter> warpAdapter;
		THROW_IF_FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
		THROW_IF_FAILED(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&Device)));
	}

	// Check supported features
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	THROW_IF_FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
	frt_assert(options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);

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
		FramesResources[i].Init(
			Device.Get(),
			render::constants::InitialPassCount,
			render::constants::InitialObjectCount,
			render::constants::InitialMaterialCount,
			BufferArena,
			GetDescriptorHeap());
	}

	for (unsigned frameIndex = 0; frameIndex < render::constants::SwapChainBufferCount; ++frameIndex)
	{
		RtvHeap.Allocate(&FrameBufferDescriptors[frameIndex], nullptr);
	}

	DsvHeap.Allocate(&DepthStencilDescriptor, nullptr);

	CreateDefaultWhiteTexture();
	CreateRootSignature();
	CreatePipelineState();

	CreateRaytracingPipeline();
	CreateRaytracingOutputBuffer();
	// CreateShaderResourceHeap();

	MaterialLibrary.SetRenderer(this);
}

void CRenderer::Resize (bool bNewFullscreenState)
{
	// Flush before changing any resources.
	FlushCommandQueue();

	THROW_IF_FAILED(CommandList->Reset(CommandAllocator.Get(), nullptr));
	bCommandListRecording = true;

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

		if (bWasInFullscreen && bNewFullscreenState)
		{
			DXGI_MODE_DESC modeDesc =
			{
				.Width = static_cast<unsigned int>(drawRect.x),
				.Height = static_cast<unsigned int>(drawRect.y),
				.RefreshRate = DisplayRefreshRate,
				.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
				.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
				.Scaling = DXGI_MODE_SCALING_UNSPECIFIED,
			};
			SwapChain->ResizeTarget(&modeDesc);
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

		CreateRaytracingOutputBuffer();
		InitializeRaytracingResources();
	}
	else
	{
		SwapChain->SetFullscreenState(bNewFullscreenState, nullptr);
		SwapChain.Reset();
	}

	// Execute the resize commands.
	THROW_IF_FAILED(CommandList->Close());
	bCommandListRecording = false;
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

IDXGIAdapter1* CRenderer::GetAdapter ()
{
	return Adapter.Get();
}

ID3D12Device5* CRenderer::GetDevice ()
{
	return Device.Get();
}

ID3D12CommandQueue* CRenderer::GetCommandQueue ()
{
	return CommandQueue.Get();
}

ID3D12GraphicsCommandList4* CRenderer::GetCommandList ()
{
	return CommandList.Get();
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
		.RefreshRate = DisplayRefreshRate,
		.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
		.Scaling = DXGI_MODE_SCALING_UNSPECIFIED,
		.Windowed = !bFullscreen,
	};

	THROW_IF_FAILED(
		Factory->CreateSwapChainForHwnd(
			CommandQueue.Get(), Window->GetHandle(), &swapChainDesc, &fullscreenDesc, nullptr, &SwapChain));
}

void CRenderer::BeginInitializationCommands ()
{
	auto commandListAllocator = GetCurrentFrameResource().CommandListAllocator;
	THROW_IF_FAILED(commandListAllocator->Reset());
	THROW_IF_FAILED(CommandList->Reset(commandListAllocator.Get(), nullptr));
	bCommandListRecording = true;
	ProcessPendingResourceUploads();
}

void CRenderer::EndInitializationCommands ()
{
	ProcessPendingResourceUploads();

	THROW_IF_FAILED(CommandList->Close());
	bCommandListRecording = false;
	ID3D12CommandList* commandLists[] = { CommandList.Get() };
	CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
	FlushCommandQueue();

	GetCurrentFrameResource().UploadArena.Clear();
}

void CRenderer::InitializeRendering ()
{
	BeginInitializationCommands();
	EndInitializationCommands();
}

void CRenderer::FlushCommandQueue ()
{
	FenceValue++;
	CommandQueue->Signal(Fence.Get(), FenceValue);

	WaitForFenceValue(FenceValue);
}

void CRenderer::WaitForFenceValue (uint64 Value)
{
	if (Fence->GetCompletedValue() >= Value)
	{
		return;
	}

	HANDLE eventHandle = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
	THROW_IF_FAILED(Fence->SetEventOnCompletion(Value, eventHandle));
	WaitForSingleObject(eventHandle, INFINITE);
	CloseHandle(eventHandle);
}
#pragma endregion Core

#pragma region Frame lifecycle
void CRenderer::ResetCurrentFrameCommandList ()
{
	auto commandListAllocator = GetCurrentFrameResource().CommandListAllocator;
	THROW_IF_FAILED(commandListAllocator->Reset());

	// TODO: assign different PSO
	THROW_IF_FAILED(CommandList->Reset(commandListAllocator.Get(), nullptr));
	bCommandListRecording = true;
}

void CRenderer::ReloadModifiedAssetsIfNeeded ()
{
#ifndef RELEASE
	const bool bMaterialsReloaded = MaterialLibrary.ReloadModifiedMaterials();
	const bool bShadersReloaded = ShaderLibrary.ReloadModifiedShaders();
	if (bMaterialsReloaded || bShadersReloaded)
	{
		bPendingPipelineStateRebuild = true;
	}
#endif
}

void CRenderer::SetRenderMode (ERenderMode Mode)
{
	RenderMode = Mode;
}

ERenderMode CRenderer::GetRenderMode () const
{
	return RenderMode;
}

bool CRenderer::IsRaytracingReady () const
{
	return RtStateObject
			&& RtOutputResource
			&& SrvUavHeap
			&& SbtStorage
			&& TopLevelASBuffers.Result;
}

bool CRenderer::ShouldRenderRaster () const
{
	// fallback to raster
	return RenderMode == ERenderMode::Raster || !IsRaytracingReady() || RenderMode == ERenderMode::Hybrid;
}

bool CRenderer::ShouldRenderRaytracing () const
{
	return RenderMode == ERenderMode::Raytracing && IsRaytracingReady();
}

void CRenderer::StartFrame ()
{
	ResetCurrentFrameCommandList();
	ProcessPendingResourceUploads();
	ReloadModifiedAssetsIfNeeded();
	const bool bRenderRaster = ShouldRenderRaster();
	const bool bRenderRaytracing = ShouldRenderRaytracing();

	if (bRenderRaster)
	{
		PrepareRasterPass();
	}
	else
	{
		TransitionCurrentBackBufferToRenderTarget();
	}
}

void CRenderer::PrepareCurrentPass ()
{
	const bool bRenderRaytracing = ShouldRenderRaytracing();
	if (bRenderRaytracing)
	{
		if (bRaytracingSbtDirty)
		{
			CreateShaderBindingTable();
			bRaytracingSbtDirty = false;
		}
		DispatchRaytracingToCurrentFrameBuffer();
		SetupCurrentBackBufferRenderTargetNoClear();
	}
}

void CRenderer::Tick ()
{
	CurrentFrameResourceIndex = (CurrentFrameResourceIndex + 1) % render::constants::FrameResourcesBufferCount;

	if (GetCurrentFrameResource().FenceValue != 0 && Fence->GetCompletedValue() < GetCurrentFrameResource().FenceValue)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
		THROW_IF_FAILED(Fence->SetEventOnCompletion(GetCurrentFrameResource().FenceValue, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void CRenderer::Draw ()
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
	bCommandListRecording = false;
	CommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)CommandList.GetAddressOf());

	SwapChain->Present(bVSyncEnabled, 0);
	CurrentBackBufferIndex = (CurrentBackBufferIndex + 1) % render::constants::FrameResourcesBufferCount;

	// FlushCommandQueue();

	++FenceValue;
	GetCurrentFrameResource().FenceValue = FenceValue;
	CommandQueue->Signal(Fence.Get(), FenceValue);

#ifndef RELEASE
	if (bPendingPipelineStateRebuild)
	{
		WaitForFenceValue(FenceValue);
		CreatePipelineState();
		bPendingPipelineStateRebuild = false;
	}
#endif

	GetCurrentFrameResource().UploadArena.Clear();
}

SFrameResources& CRenderer::GetCurrentFrameResource ()
{
	return FramesResources[CurrentFrameResourceIndex];
}

#pragma endregion Frame lifecycle

#pragma region Resource uploads
DX12_Arena& CRenderer::GetBufferArena ()
{
	return BufferArena;
}

void CRenderer::RecordBufferUpload (
	ID3D12Resource* Resource,
	uint64 SizeInBytes,
	const void* BufferData,
	D3D12_RESOURCE_STATES FinalState)
{
	frt_assert(Resource);

	uint64 uploadOffset = 0;
	uint8* dest = GetCurrentFrameResource().UploadArena.Allocate(SizeInBytes, &uploadOffset);
	if (BufferData && SizeInBytes > 0u)
	{
		memcpy(dest, BufferData, SizeInBytes);
	}
	else if (SizeInBytes > 0u)
	{
		memset(dest, 0, SizeInBytes);
	}

	CommandList->CopyBufferRegion(
		Resource, 0, GetCurrentFrameResource().UploadArena.GetGPUBuffer(), uploadOffset, SizeInBytes);

	D3D12_RESOURCE_BARRIER resourceBarrier = {};
	resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	resourceBarrier.Transition.pResource = Resource;
	resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	resourceBarrier.Transition.StateAfter = FinalState;
	CommandList->ResourceBarrier(1, &resourceBarrier);
}

uint32 CRenderer::BuildPackedTextureData (
	const D3D12_RESOURCE_DESC& Desc,
	const void* Texels,
	TArray<uint8>& OutPackedTexels) const
{
	frt_assert(Texels);
	frt_assert(Desc.DepthOrArraySize == 1);

	const uint64 bytesPerPixel = Dx12GetBytesPerPixel(Desc.Format);
	const uint64 sourceRowBytes = static_cast<uint64>(Desc.Width) * bytesPerPixel;
	const uint32 rowPitch = static_cast<uint32>(AlignAddress(sourceRowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
	const uint64 totalBytes = static_cast<uint64>(rowPitch) * static_cast<uint64>(Desc.Height);
	frt_assert(totalBytes <= UINT32_MAX);

	OutPackedTexels.SetSizeUninitialized<true>(static_cast<uint32>(totalBytes));

	const uint8* sourceTexels = reinterpret_cast<const uint8*>(Texels);
	uint8* packedTexels = OutPackedTexels.GetData();
	for (uint32 y = 0; y < Desc.Height; ++y)
	{
		memcpy(
			packedTexels + static_cast<uint64>(y) * static_cast<uint64>(rowPitch),
			sourceTexels + static_cast<uint64>(y) * sourceRowBytes,
			sourceRowBytes);
	}

	return rowPitch;
}

void CRenderer::RecordTextureUpload (
	ID3D12Resource* Resource,
	const D3D12_RESOURCE_DESC& Desc,
	uint32 RowPitch,
	const void* PackedTexels,
	D3D12_RESOURCE_STATES FinalState)
{
	frt_assert(Resource);
	frt_assert(PackedTexels);

	const uint64 packedBytes = static_cast<uint64>(RowPitch) * static_cast<uint64>(Desc.Height);
	uint64 uploadOffset = 0;
	uint8* destTexels = GetCurrentFrameResource().UploadArena.Allocate(packedBytes, &uploadOffset);
	memcpy(destTexels, PackedTexels, packedBytes);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT placedFootprint = {};
	placedFootprint.Offset = uploadOffset;
	placedFootprint.Footprint.Format = Desc.Format;
	placedFootprint.Footprint.Width = static_cast<uint32>(Desc.Width);
	placedFootprint.Footprint.Height = Desc.Height;
	placedFootprint.Footprint.Depth = 1;
	placedFootprint.Footprint.RowPitch = RowPitch;

	D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
	srcLocation.pResource = GetCurrentFrameResource().UploadArena.GetGPUBuffer();
	srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	srcLocation.PlacedFootprint = placedFootprint;

	D3D12_TEXTURE_COPY_LOCATION destLocation = {};
	destLocation.pResource = Resource;
	destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destLocation.SubresourceIndex = 0;

	CommandList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);

	D3D12_RESOURCE_BARRIER resourceBarrier = {};
	resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	resourceBarrier.Transition.pResource = Resource;
	resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	resourceBarrier.Transition.StateAfter = FinalState;
	CommandList->ResourceBarrier(1, &resourceBarrier);
}

void CRenderer::ProcessPendingResourceUploads ()
{
	if (PendingBufferUploads.IsEmpty() && PendingTextureUploads.IsEmpty())
	{
		return;
	}

	frt_assert(bCommandListRecording);

	for (uint32 i = 0; i < PendingBufferUploads.Count(); ++i)
	{
		SPendingBufferUpload& upload = PendingBufferUploads[i];
		if (!upload.Resource)
		{
			continue;
		}

		RecordBufferUpload(
			upload.Resource,
			static_cast<uint64>(upload.Data.Count()),
			upload.Data.GetData(),
			upload.FinalState);
	}
	PendingBufferUploads.Clear();

	for (uint32 i = 0; i < PendingTextureUploads.Count(); ++i)
	{
		SPendingTextureUpload& upload = PendingTextureUploads[i];
		if (!upload.Resource || upload.PackedTexels.IsEmpty())
		{
			continue;
		}

		RecordTextureUpload(
			upload.Resource,
			upload.Desc,
			upload.RowPitch,
			upload.PackedTexels.GetData(),
			upload.FinalState);
	}
	PendingTextureUploads.Clear();
}

ID3D12Resource* CRenderer::CreateBufferAsset (const D3D12_RESOURCE_DESC& Desc)
{
	ID3D12Resource* resource = BufferArena.Allocate(Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
	frt_assert(resource);
	return resource;
}

void CRenderer::EnqueueBufferUpload (
	ID3D12Resource* Resource,
	uint64 SizeInBytes,
	const void* BufferData,
	D3D12_RESOURCE_STATES FinalState)
{
	frt_assert(Resource);
	frt_assert(SizeInBytes <= UINT32_MAX);

	SPendingBufferUpload& pendingUpload = PendingBufferUploads.Add();
	pendingUpload.Resource = Resource;
	pendingUpload.FinalState = FinalState;
	pendingUpload.Data.SetSizeUninitialized<true>(static_cast<uint32>(SizeInBytes));
	if (BufferData && SizeInBytes > 0u)
	{
		memcpy(pendingUpload.Data.GetData(), BufferData, SizeInBytes);
	}
	else if (SizeInBytes > 0u)
	{
		memset(pendingUpload.Data.GetData(), 0, SizeInBytes);
	}
}

ID3D12Resource* CRenderer::CreateTextureAsset (const D3D12_RESOURCE_DESC& Desc)
{
	ID3D12Resource* resource = TextureArena.Allocate(Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
	frt_assert(resource);
	return resource;
}

void CRenderer::EnqueueTextureUpload (
	ID3D12Resource* Resource,
	const D3D12_RESOURCE_DESC& Desc,
	const void* Texels,
	D3D12_RESOURCE_STATES FinalState)
{
	frt_assert(Resource);

	TArray<uint8> packedTexels;
	const uint32 rowPitch = BuildPackedTextureData(Desc, Texels, packedTexels);

	SPendingTextureUpload& pendingUpload = PendingTextureUploads.Add();
	pendingUpload.Resource = Resource;
	pendingUpload.Desc = Desc;
	pendingUpload.FinalState = FinalState;
	pendingUpload.RowPitch = rowPitch;
	pendingUpload.PackedTexels = std::move(packedTexels);
}

#pragma endregion Resource uploads

#pragma region Rasterization
void CRenderer::CreateRootSignature ()
{
	CD3DX12_ROOT_PARAMETER rootParameters[render::constants::RootParamCount];

	CD3DX12_DESCRIPTOR_RANGE texTable0(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		render::constants::RootRegister_BaseColorTexture,
		render::constants::RootSpace_Global);

	rootParameters[render::constants::RootParam_BaseColorTexture].InitAsDescriptorTable(1, &texTable0);

	CD3DX12_DESCRIPTOR_RANGE objCbvTable0(
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
		1,
		render::constants::RootRegister_ObjectCbv,
		render::constants::RootSpace_Global);
	CD3DX12_DESCRIPTOR_RANGE materialCbvTable0(
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
		1,
		render::constants::RootRegister_MaterialCbv,
		render::constants::RootSpace_Global);
	CD3DX12_DESCRIPTOR_RANGE passCbvTable0(
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
		1,
		render::constants::RootRegister_PassCbv,
		render::constants::RootSpace_Global);

	rootParameters[render::constants::RootParam_ObjectCbv].InitAsDescriptorTable(1, &objCbvTable0);
	rootParameters[render::constants::RootParam_MaterialCbv].InitAsDescriptorTable(1, &materialCbvTable0);
	rootParameters[render::constants::RootParam_PassCbv].InitAsDescriptorTable(1, &passCbvTable0);

	CD3DX12_DESCRIPTOR_RANGE materialTextureTable0(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		render::constants::RootMaterialTextureCount,
		render::constants::RootRegister_MaterialTextureStart,
		render::constants::RootSpace_Material);
	rootParameters[render::constants::RootParam_MaterialTextures].InitAsDescriptorTable(
		1,
		&materialTextureTable0);

	const D3D12_STATIC_SAMPLER_DESC samplerDesc = BuildLinearWrapStaticSamplerDesc();

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(
		render::constants::RootParamCount, rootParameters, 1, &samplerDesc,
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

static std::string MakePipelineStateKey (
	const std::string& VertexShaderName,
	const std::string& PixelShaderName,
	D3D12_CULL_MODE CullMode,
	D3D12_COMPARISON_FUNC DepthFunc,
	bool bDepthEnable,
	bool bDepthWrite,
	bool bAlphaBlend)
{
	return VertexShaderName + "|" + PixelShaderName
			+ "|" + std::to_string(static_cast<uint32>(CullMode))
			+ "|" + std::to_string(static_cast<uint32>(DepthFunc))
			+ "|" + (bDepthEnable ? "1" : "0")
			+ "|" + (bDepthWrite ? "1" : "0")
			+ "|" + (bAlphaBlend ? "1" : "0");
}

static uint64 HashDefines (const TArray<SShaderDefine>& Defines)
{
	constexpr uint64 offsetBasis = 1469598103934665603ull;
	constexpr uint64 prime = 1099511628211ull;

	uint64 hash = offsetBasis;
	for (uint32 i = 0; i < Defines.Count(); ++i)
	{
		const SShaderDefine& define = Defines[i];
		for (unsigned char ch : define.Name)
		{
			hash ^= ch;
			hash *= prime;
		}
		hash ^= static_cast<unsigned char>('=');
		hash *= prime;
		for (unsigned char ch : define.Value)
		{
			hash ^= ch;
			hash *= prime;
		}
		hash ^= static_cast<unsigned char>(';');
		hash *= prime;
	}

	return hash;
}

static std::string MakeShaderPermutationName (const std::string& BaseName, const TArray<SShaderDefine>& Defines)
{
	if (Defines.IsEmpty())
	{
		return BaseName;
	}

	const uint64 hash = HashDefines(Defines);
	char buffer[17] = {};
#if defined(_MSC_VER)
	_snprintf_s(buffer, sizeof(buffer), "%016llx", hash);
#else
	std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
#endif
	return BaseName + "_perm_" + buffer;
}

static void BuildShaderDefines (
	const SMaterial& Material,
	EShaderStage Stage,
	TArray<SShaderDefine>& OutDefines)
{
	OutDefines.Clear();

	if (Stage == EShaderStage::Pixel && (Material.Flags && EMaterialFlags::UseBaseColorTexture))
	{
		SShaderDefine& define = OutDefines.Add();
		define.Name = "FRT_HAS_BASE_COLOR_TEXTURE";
		define.Value = "1";
	}
}

CRenderer::SShaderPermutation CRenderer::BuildShaderPermutation (
	const std::string& BaseName,
	const SMaterial& Material,
	EShaderStage Stage) const
{
	SShaderPermutation permutation;
	BuildShaderDefines(Material, Stage, permutation.Defines);
	permutation.Name = MakeShaderPermutationName(BaseName, permutation.Defines);

	const std::filesystem::path shaderSourceDir = R"(..\Core\Content\Shaders)";
	const std::filesystem::path shaderBinDir = shaderSourceDir / "Bin";
	permutation.CompiledPath = shaderBinDir / (permutation.Name + ".shader");
	permutation.SourcePath = shaderSourceDir / (BaseName + ".hlsl");
	return permutation;
}

ComPtr<ID3D12PipelineState> CRenderer::BuildPipelineState (
	const SShaderPermutation& VertexShader,
	const SShaderPermutation& PixelShader,
	D3D12_CULL_MODE CullMode,
	D3D12_COMPARISON_FUNC DepthFunc,
	bool bDepthEnable,
	bool bDepthWrite,
	bool bAlphaBlend)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = RootSignature.Get();

	const std::filesystem::path shaderSourceDir = R"(..\Core\Content\Shaders)";

	const SShaderAsset* vertexShader = ShaderLibrary.LoadShader(
		VertexShader.Name,
		VertexShader.CompiledPath,
		VertexShader.SourcePath,
		shaderSourceDir,
		"main",
		"vs_6_0",
		EShaderStage::Vertex,
		VertexShader.Defines);
	const SShaderAsset* pixelShader = ShaderLibrary.LoadShader(
		PixelShader.Name,
		PixelShader.CompiledPath,
		PixelShader.SourcePath,
		shaderSourceDir,
		"main",
		"ps_6_0",
		EShaderStage::Pixel,
		PixelShader.Defines);

	psoDesc.VS = vertexShader->GetBytecode();
	psoDesc.PS = pixelShader->GetBytecode();

	psoDesc.BlendState.RenderTarget[0].BlendEnable = bAlphaBlend;
	psoDesc.BlendState.RenderTarget[0].LogicOpEnable = false;
	psoDesc.BlendState.RenderTarget[0].SrcBlend = bAlphaBlend ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
	psoDesc.BlendState.RenderTarget[0].DestBlend = bAlphaBlend ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
	psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = bAlphaBlend ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
	psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode = CullMode;
	psoDesc.RasterizerState.FrontCounterClockwise = false;
	psoDesc.RasterizerState.DepthClipEnable = true;

	psoDesc.DepthStencilState.DepthEnable = bDepthEnable;
	psoDesc.DepthStencilState.StencilEnable = false;
	psoDesc.DepthStencilState.DepthWriteMask =
		bDepthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.DepthFunc = DepthFunc;

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[6] = {};
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

	inputElementDescs[4].SemanticName = "BITANGENT";
	inputElementDescs[4].SemanticIndex = 0;
	inputElementDescs[4].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[4].InputSlot = 0;
	inputElementDescs[4].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[4].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

	inputElementDescs[5].SemanticName = "COLOR";
	inputElementDescs[5].SemanticIndex = 0;
	inputElementDescs[5].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[5].InputSlot = 0;
	inputElementDescs[5].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[5].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

	psoDesc.InputLayout.pInputElementDescs = inputElementDescs;
	psoDesc.InputLayout.NumElements = 6;

	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

	ComPtr<ID3D12PipelineState> pipelineState;
	THROW_IF_FAILED(Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)));
	return pipelineState;
}

void CRenderer::CreatePipelineState ()
{
	PipelineStateCache.clear();
	const SMaterial defaultMaterial = {};
	const SShaderPermutation vertexShader = BuildShaderPermutation(
		"VertexShader", defaultMaterial, EShaderStage::Vertex);
	const SShaderPermutation pixelShader = BuildShaderPermutation("PixelShader", defaultMaterial, EShaderStage::Pixel);
	PipelineState = BuildPipelineState(
		vertexShader,
		pixelShader,
		defaultMaterial.CullMode,
		defaultMaterial.DepthFunc,
		defaultMaterial.bDepthEnable,
		defaultMaterial.bDepthWrite,
		defaultMaterial.bAlphaBlend);
	PipelineStateCache.emplace(
		MakePipelineStateKey(
			vertexShader.Name,
			pixelShader.Name,
			defaultMaterial.CullMode,
			defaultMaterial.DepthFunc,
			defaultMaterial.bDepthEnable,
			defaultMaterial.bDepthWrite,
			defaultMaterial.bAlphaBlend),
		PipelineState);
}

void CRenderer::TransitionCurrentBackBufferToRenderTarget ()
{
	D3D12_RESOURCE_BARRIER resourceBarrier = {};
	resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	resourceBarrier.Transition.pResource = FrameBuffer[CurrentBackBufferIndex].Get();
	resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	CommandList->ResourceBarrier(1, &resourceBarrier);
}

void CRenderer::SetupCurrentBackBufferRenderTarget ()
{
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &ScissorRect);

	FLOAT clearColor[] = { 0.1f, 0.3f, 0.3f, 1.f };
	CommandList->ClearRenderTargetView(FrameBufferDescriptors[CurrentBackBufferIndex], clearColor, 0, nullptr);
	CommandList->ClearDepthStencilView(
		DepthStencilDescriptor, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

	CommandList->OMSetRenderTargets(
		1, &FrameBufferDescriptors[CurrentBackBufferIndex], false, &DepthStencilDescriptor);
}

void CRenderer::SetupCurrentBackBufferRenderTargetNoClear ()
{
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &ScissorRect);
	CommandList->OMSetRenderTargets(
		1, &FrameBufferDescriptors[CurrentBackBufferIndex], false, &DepthStencilDescriptor);
}

void CRenderer::BindDefaultRasterState ()
{
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(PipelineState.Get());

	CommandList->SetDescriptorHeaps(1, &ShaderDescriptorHeap._heap);
}

void CRenderer::PrepareRasterPass ()
{
	TransitionCurrentBackBufferToRenderTarget();
	SetupCurrentBackBufferRenderTarget();
	BindDefaultRasterState();
}

CMaterialLibrary& CRenderer::GetMaterialLibrary ()
{
	return MaterialLibrary;
}

ID3D12PipelineState* CRenderer::GetPipelineStateForMaterial (const SMaterial& Material)
{
	const std::string vertexShaderBase = Material.VertexShaderName.empty()
											? "VertexShader"
											: Material.VertexShaderName;
	const std::string pixelShaderBase = Material.PixelShaderName.empty()
											? "PixelShader"
											: Material.PixelShaderName;

	const SShaderPermutation vertexShader = BuildShaderPermutation(vertexShaderBase, Material, EShaderStage::Vertex);
	const SShaderPermutation pixelShader = BuildShaderPermutation(pixelShaderBase, Material, EShaderStage::Pixel);

	const std::string key = MakePipelineStateKey(
		vertexShader.Name,
		pixelShader.Name,
		Material.CullMode,
		Material.DepthFunc,
		Material.bDepthEnable,
		Material.bDepthWrite,
		Material.bAlphaBlend);
	auto it = PipelineStateCache.find(key);
	if (it != PipelineStateCache.end())
	{
		return it->second.Get();
	}

	ComPtr<ID3D12PipelineState> pipelineState = BuildPipelineState(
		vertexShader,
		pixelShader,
		Material.CullMode,
		Material.DepthFunc,
		Material.bDepthEnable,
		Material.bDepthWrite,
		Material.bAlphaBlend);
	ID3D12PipelineState* pipelineRaw = pipelineState.Get();
	PipelineStateCache.emplace(key, std::move(pipelineState));
	return pipelineRaw;
}

D3D12_GPU_DESCRIPTOR_HANDLE CRenderer::GetDefaultWhiteTextureGpu () const
{
	return DefaultWhiteTexture.GpuDescriptor;
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

	bRaytracingSbtDirty = true;
}

void CRenderer::EnsureMaterialConstantCapacity (uint32 MaterialCount)
{
	if (MaterialCount == 0u)
	{
		return;
	}

	const uint32 currentMaterialCount = FramesResources[0].MaterialCB.ObjectCount;
	if (MaterialCount <= currentMaterialCount)
	{
		return;
	}

	const uint32 frameCount = render::constants::FrameResourcesBufferCount;
	const uint32 requiredTotal =
		static_cast<uint32>(ShaderDescriptorHeap.GetCount()) + frameCount * MaterialCount;

	EnsureShaderDescriptorCapacity(requiredTotal);

	for (uint32 i = 0; i < render::constants::FrameResourcesBufferCount; ++i)
	{
		FramesResources[i].EnsureMaterialCapacity(Device.Get(), MaterialCount, BufferArena, ShaderDescriptorHeap);
	}

	bRaytracingSbtDirty = true;
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
		FramesResources[i].MaterialCB.RebuildDescriptors(Device.Get(), ShaderDescriptorHeap);
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

void CRenderer::CreateDefaultWhiteTexture ()
{
	constexpr uint32 whiteTexel = 0xFFFFFFFFu;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = 1;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	DefaultWhiteTexture.GpuTexture = CreateTextureAsset(desc);
	DefaultWhiteTexture.Width = 1;
	DefaultWhiteTexture.Height = 1;
	DefaultWhiteTexture.Texels = nullptr;

	EnqueueTextureUpload(
		DefaultWhiteTexture.GpuTexture, desc, &whiteTexel,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.PlaneSlice = 0;

	D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
	CreateShaderResourceView(
		DefaultWhiteTexture.GpuTexture, srvDesc, &cpuDescriptor, &DefaultWhiteTexture.GpuDescriptor);

	// Upload is queued; it will be processed during initialization or the first frame.
}


DX12_DescriptorHeap& CRenderer::GetDescriptorHeap ()
{
	return ShaderDescriptorHeap;
}

#pragma endregion Rasterization

#pragma region Raytracing
void CRenderer::InitializeRaytracingResources ()
{
	if (!TopLevelASBuffers.Result)
	{
		return;
	}

	CreateShaderBindingTable();
}

void CRenderer::SetRaytracingMaterialTextureSets (const TArray<SRaytracingMaterialTextureSet>& MaterialTextureSets)
{
	bool bChanged = RaytracingMaterialTextureSets.Count() != MaterialTextureSets.Count();
	if (!bChanged)
	{
		for (uint32 i = 0; i < MaterialTextureSets.Count(); ++i)
		{
			for (uint32 slot = 0; slot < render::constants::RootMaterialTextureCount; ++slot)
			{
				if (RaytracingMaterialTextureSets[i].Textures[slot] != MaterialTextureSets[i].Textures[slot])
				{
					bChanged = true;
					break;
				}
			}
			if (bChanged)
			{
				break;
			}
		}
	}

	if (!bChanged)
	{
		return;
	}

	RaytracingMaterialTextureSets = MaterialTextureSets;
	bRaytracingSbtDirty = true;
}

void CRenderer::SetRaytracingHitGroupEntries (
	const TArray<SRaytracingHitGroupEntry>& HitGroupEntries)
{
	bool bChanged = RaytracingHitGroupEntries.Count() != HitGroupEntries.Count();
	if (!bChanged)
	{
		for (uint32 i = 0; i < HitGroupEntries.Count(); ++i)
		{
			const SRaytracingHitGroupEntry& currentEntry = RaytracingHitGroupEntries[i];
			const SRaytracingHitGroupEntry& newEntry = HitGroupEntries[i];
			if (currentEntry.MaterialIndex != newEntry.MaterialIndex ||
				currentEntry.VertexBuffer != newEntry.VertexBuffer ||
				currentEntry.IndexBuffer != newEntry.IndexBuffer)
			{
				bChanged = true;
				break;
			}
		}
	}

	if (!bChanged)
	{
		return;
	}

	RaytracingHitGroupEntries = HitGroupEntries;
	bRaytracingSbtDirty = true;
}

ComPtr<ID3D12RootSignature> CRenderer::CreateRayGenSignature ()
{
	raytracing::CRootSignatureGenerator rsc;
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, render::constants::RootRegister_PassCbv, 0);
	rsc.AddHeapRangesParameter(
	{
		{
			0 /*u0*/,
			1 /*1 descriptor */,
			0 /*use the implicit register space 0*/,
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV /* UAV representing the output buffer*/,
			0 /*heap slot where the UAV is defined*/
		},
		{
			0 /*t0*/,
			1,
			0,
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Top-level acceleration structure*/,
			1
		}
	});

	return rsc.Generate(Device.Get(), true);
}

ComPtr<ID3D12RootSignature> CRenderer::CreateMissSignature ()
{
	raytracing::CRootSignatureGenerator rsc;
	return rsc.Generate(Device.Get(), true);
}

ComPtr<ID3D12RootSignature> CRenderer::CreateHitSignature ()
{
	raytracing::CRootSignatureGenerator rsc;
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, render::constants::RootRegister_MaterialCbv, 0);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, render::constants::RootRegister_PassCbv, 0);
	rsc.AddHeapRangesParameter(
	{
		{
			0 /*t0*/,
			1,
			0 /*space0*/,
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			0
		}
	});
	rsc.AddHeapRangesParameter(
	{
		{
			render::constants::RaytracingRegister_MaterialTextureStart,
			render::constants::RootMaterialTextureCount,
			0 /*space0*/,
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			0
		}
	});
	rsc.AddRootParameter(
		D3D12_ROOT_PARAMETER_TYPE_SRV,
		render::constants::RaytracingRegister_VertexBufferSrv,
		0);
	rsc.AddRootParameter(
		D3D12_ROOT_PARAMETER_TYPE_SRV,
		render::constants::RaytracingRegister_IndexBufferSrv,
		0);

	const D3D12_STATIC_SAMPLER_DESC linearWrapSampler = BuildLinearWrapStaticSamplerDesc();
	rsc.AddStaticSampler(linearWrapSampler);

	return rsc.Generate(Device.Get(), true);
}

void CRenderer::CreateRaytracingPipeline ()
{
	raytracing::CRayTracingPipelineGenerator pipeline(Device.Get());

	RayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"..\\Core\\Content\\Shaders\\DXR\\RayGen.hlsl");
	MissLibrary = nv_helpers_dx12::CompileShaderLibrary(L"..\\Core\\Content\\Shaders\\DXR\\Miss.hlsl");
	HitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"..\\Core\\Content\\Shaders\\DXR\\Hit.hlsl");
	ShadowLibrary = nv_helpers_dx12::CompileShaderLibrary(L"..\\Core\\Content\\Shaders\\DXR\\ShadowRay.hlsl");

	// In a way similar to DLLs, each library is associated with a number of
	// exported symbols. This
	// has to be done explicitly in the lines below. Note that a single library
	// can contain an arbitrary number of symbols, whose semantic is given in HLSL
	// using the [shader("xxx")] syntax
	pipeline.AddLibrary(RayGenLibrary.Get(), { L"RayGen" });
	pipeline.AddLibrary(MissLibrary.Get(), { L"Miss" });
	pipeline.AddLibrary(HitLibrary.Get(), { L"ClosestHit" });
	pipeline.AddLibrary(ShadowLibrary.Get(), { L"ShadowClosestHit", L"ShadowMiss" });

	RayGenSignature = CreateRayGenSignature();
	MissSignature = CreateMissSignature();
	HitSignature = CreateHitSignature();
	ShadowSignature = CreateHitSignature();

	// Note that for triangular geometry the intersection shader is built-in. An
	// empty any-hit shader is also defined by default, so in our simple case each
	// hit group contains only the closest hit shader. Note that since the
	// exported symbols are defined above the shaders can be simply referred to by
	// name.

	// Hit group for the triangles, with a shader simply interpolating vertex
	// colors
	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
	pipeline.AddHitGroup(L"ShadowHitGroup", L"ShadowClosestHit");

	// The following section associates the root signature to each shader. Note
	// that we can explicitly show that some shaders share the same root signature
	// (eg. Miss and ShadowMiss). Note that the hit shaders are now only referred
	// to as hit groups, meaning that the underlying intersection, any-hit and
	// closest-hit shaders share the same root signature.
	pipeline.AddRootSignatureAssociation(RayGenSignature.Get(), { L"RayGen" });
	pipeline.AddRootSignatureAssociation(MissSignature.Get(), { L"Miss" });
	pipeline.AddRootSignatureAssociation(HitSignature.Get(), { L"HitGroup" });
	pipeline.AddRootSignatureAssociation(ShadowSignature.Get(), { L"ShadowHitGroup" });
	pipeline.AddRootSignatureAssociation(MissSignature.Get(), { L"ShadowMiss" });

	// The payload size defines the maximum size of the data carried by the rays,
	// ie. the the data
	// exchanged between shaders, such as the HitInfo structure in the HLSL code.
	// It is important to keep this value as low as possible as a too high value
	// would result in unnecessary memory consumption and cache trashing.
	pipeline.SetMaxPayloadSize(4 * sizeof(float) + 2 * sizeof(uint32)); // RGB + distance + depth + rngState

	// Upon hitting a surface, DXR can provide several attributes to the hit. In
	// our sample we just use the barycentric coordinates defined by the weights
	// u,v of the last two vertices of the triangle. The actual barycentrics can
	// be obtained using float3 barycentrics = float3(1.f-u-v, u, v);
	pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates

	// The raytracing process can shoot rays from existing hit points, resulting
	// in nested TraceRay calls. Our sample code traces only primary rays, which
	// then requires a trace depth of 1. Note that this recursion depth should be
	// kept to a minimum for best performance. Path tracing algorithms can be
	// easily flattened into a simple loop in the ray generation.
	pipeline.SetMaxRecursionDepth(30);

	// Compile the pipeline for execution on the GPU
	RtStateObject = pipeline.Generate();

	// Cast the state object into a properties object, allowing to later access
	// the shader pointers by name
	THROW_IF_FAILED(
		RtStateObject->QueryInterface(IID_PPV_ARGS(&RtStateObjectProperties)));
}

void CRenderer::CreateRaytracingOutputBuffer ()
{
	Vector2f drawRect = Window->GetWindowSize();

	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.DepthOrArraySize = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	// The backbuffer is actually DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, but sRGB
	// formats cannot be used with UAVs. For accuracy we should convert to sRGB
	// ourselves in the shader
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	resDesc.Width = static_cast<uint64>(drawRect.x);
	resDesc.Height = static_cast<uint64>(drawRect.y);
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;
	THROW_IF_FAILED(
		Device->CreateCommittedResource(
			&raytracing::DefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
			IID_PPV_ARGS(&RtOutputResource)));
}

void CRenderer::CreateShaderResourceHeap ()
{
	const uint32 materialCountFromCb = FramesResources[0].MaterialCB.ObjectCount;
	const uint32 materialCountFromSet = RaytracingMaterialTextureSets.Count();
	const uint32 materialCount = materialCountFromCb > materialCountFromSet
									? materialCountFromCb
									: materialCountFromSet;
	RaytracingMaterialCapacity = materialCount > 0u ? materialCount : 1u;

	const uint32 textureDescriptorCount =
		RaytracingMaterialCapacity * render::constants::RootMaterialTextureCount;
	const uint32 descriptorCount =
		render::constants::RaytracingHeapSlot_MaterialTextures + textureDescriptorCount;

	SrvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(
		Device.Get(), descriptorCount, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

	const uint32 descriptorSize = Device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = SrvUavHeap->GetCPUDescriptorHandleForHeapStart();
	auto descriptorAt = [&] (uint32 descriptorIndex)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = heapStart;
		handle.ptr += static_cast<SIZE_T>(descriptorSize) * descriptorIndex;
		return handle;
	};

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	Device->CreateUnorderedAccessView(
		RtOutputResource.Get(), nullptr, &uavDesc,
		descriptorAt(render::constants::RaytracingHeapSlot_OutputUav));

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location =
		TopLevelASBuffers.Result->GetGPUVirtualAddress();
	Device->CreateShaderResourceView(
		nullptr, &srvDesc, descriptorAt(render::constants::RaytracingHeapSlot_TlasSrv));

	for (uint32 materialIndex = 0; materialIndex < RaytracingMaterialCapacity; ++materialIndex)
	{
		const SRaytracingMaterialTextureSet* textureSet = materialIndex < RaytracingMaterialTextureSets.Count()
															? &RaytracingMaterialTextureSets[materialIndex]
															: nullptr;
		for (uint32 textureSlot = 0; textureSlot < render::constants::RootMaterialTextureCount; ++textureSlot)
		{
			const uint32 descriptorIndex = render::constants::RaytracingHeapSlot_MaterialTextures +
											materialIndex * render::constants::RootMaterialTextureCount +
											textureSlot;
			ID3D12Resource* texture = DefaultWhiteTexture.GpuTexture;
			if (textureSet && textureSet->Textures[textureSlot])
			{
				texture = textureSet->Textures[textureSlot];
			}

			WriteRaytracingTextureSrv(descriptorAt(descriptorIndex), texture);
		}
	}
}

void CRenderer::WriteRaytracingTextureSrv (
	D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle,
	ID3D12Resource* Texture) const
{
	ID3D12Resource* textureResource = Texture ? Texture : DefaultWhiteTexture.GpuTexture;
	frt_assert(textureResource);

	const D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = textureDesc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = textureDesc.MipLevels > 0 ? textureDesc.MipLevels : 1;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.f;

	Device->CreateShaderResourceView(textureResource, &srvDesc, CpuHandle);
}

void CRenderer::CreateShaderBindingTable ()
{
	CreateShaderResourceHeap();

	// The SBT helper class collects calls to Add*Program.  If called several
	// times, the helper must be emptied before re-adding shaders.
	SbtHelper.Reset();

	const SFrameResources& currentFrameResources = GetCurrentFrameResource();

	// The pointer to the beginning of the heap is the only parameter required by
	// shaders without root parameters
	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle =
		SrvUavHeap->GetGPUDescriptorHandleForHeapStart();

	// The helper treats both root parameter pointers and heap pointers as void*,
	// while DX12 uses the
	// D3D12_GPU_DESCRIPTOR_HANDLE to define heap pointers. The pointer in this
	// struct is a UINT64, which then has to be reinterpreted as a pointer.
	auto heapPointer = reinterpret_cast<void*>(srvUavHeapHandle.ptr);
	const uint32 descriptorSize = Device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	void* tlasDescriptorTablePointer = reinterpret_cast<void*>(
		srvUavHeapHandle.ptr + static_cast<uint64>(descriptorSize) * render::constants::RaytracingHeapSlot_TlasSrv);
	const uint64 materialTextureHeapStart = srvUavHeapHandle.ptr +
											static_cast<uint64>(descriptorSize) *
											render::constants::RaytracingHeapSlot_MaterialTextures;

	// Ray generation uses pass constants and global heap descriptors.
	void* passCbAddress = nullptr;
	if (currentFrameResources.PassCB.GpuResource)
	{
		passCbAddress = reinterpret_cast<void*>(
			currentFrameResources.PassCB.GpuResource->GetGPUVirtualAddress());
	}
	SbtHelper.AddRayGenerationProgram(L"RayGen", { passCbAddress, heapPointer });

	// The miss shader only uses payload data.
	SbtHelper.AddMissProgram(L"Miss", {});
	SbtHelper.AddMissProgram(L"ShadowMiss", {});

	const auto& materialCB = currentFrameResources.MaterialCB;
	if (materialCB.GpuResource)
	{
		for (uint32 i = 0; i < RaytracingHitGroupEntries.Count(); ++i)
		{
			const SRaytracingHitGroupEntry& hitGroupEntry = RaytracingHitGroupEntries[i];
			frt_assert(hitGroupEntry.VertexBuffer && hitGroupEntry.IndexBuffer);

			uint32 materialIndex = hitGroupEntry.MaterialIndex;
			if (materialIndex >= materialCB.ObjectCount)
			{
				materialIndex = 0u;
			}

			const uint64 materialCbAddress =
				materialCB.GpuResource->GetGPUVirtualAddress() + materialIndex * materialCB.DataSize;
			const uint64 materialTextureTableAddress = materialTextureHeapStart +
														static_cast<uint64>(materialIndex) *
														render::constants::RootMaterialTextureCount * descriptorSize;
			SbtHelper.AddHitGroup(
				L"HitGroup",
				{
					reinterpret_cast<void*>(materialCbAddress),
					passCbAddress,
					tlasDescriptorTablePointer,
					reinterpret_cast<void*>(materialTextureTableAddress),
					reinterpret_cast<void*>(hitGroupEntry.VertexBuffer->GetGPUVirtualAddress()),
					reinterpret_cast<void*>(hitGroupEntry.IndexBuffer->GetGPUVirtualAddress())
				});
			SbtHelper.AddHitGroup(
				L"ShadowHitGroup",
				{
					reinterpret_cast<void*>(materialCbAddress),
					passCbAddress,
					tlasDescriptorTablePointer,
					reinterpret_cast<void*>(materialTextureTableAddress),
					reinterpret_cast<void*>(hitGroupEntry.VertexBuffer->GetGPUVirtualAddress()),
					reinterpret_cast<void*>(hitGroupEntry.IndexBuffer->GetGPUVirtualAddress())
				});
		}
	}

	// Compute the size of the SBT given the number of shaders and their
	// parameters
	uint32 sbtSize = SbtHelper.ComputeSBTSize();

	// Create the SBT on the upload heap. This is required as the helper will use
	// mapping to write the SBT contents. After the SBT compilation it could be
	// copied to the default heap for performance.
	SbtStorage = raytracing::CreateBuffer(
		Device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE,
		D3D12_RESOURCE_STATE_GENERIC_READ, raytracing::UploadHeapProps);
	frt_assert(SbtStorage)

	SbtHelper.Generate(SbtStorage.Get(), RtStateObjectProperties.Get());
}

void CRenderer::UpdateRaytracingShaderTableAddresses ()
{
	if (!SbtStorage)
	{
		return;
	}

	const SFrameResources& currentFrameResources = GetCurrentFrameResource();
	if (!currentFrameResources.PassCB.GpuResource && !currentFrameResources.MaterialCB.GpuResource)
	{
		return;
	}

	uint8* sbtData = nullptr;
	THROW_IF_FAILED(SbtStorage->Map(0, nullptr, reinterpret_cast<void**>(&sbtData)));

	if (currentFrameResources.PassCB.GpuResource)
	{
		const uint64 passCbAddress = currentFrameResources.PassCB.GpuResource->GetGPUVirtualAddress();
		const uint64 passCbOffset = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		memcpy(sbtData + passCbOffset, &passCbAddress, sizeof(passCbAddress));
	}

	const auto& materialCB = currentFrameResources.MaterialCB;
	if (materialCB.GpuResource)
	{
		const uint64 hitGroupsSectionOffset =
			static_cast<uint64>(SbtHelper.GetRayGenSectionSize()) +
			static_cast<uint64>(SbtHelper.GetMissSectionSize());
		const uint64 hitRecordCbOffset = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		const uint64 hitRecordPassCbOffset = hitRecordCbOffset + sizeof(uint64);
		const uint32 hitRecordStride = SbtHelper.GetHitGroupEntrySize();
		const uint64 passCbAddress = currentFrameResources.PassCB.GpuResource
										? currentFrameResources.PassCB.GpuResource->GetGPUVirtualAddress()
										: 0u;

		for (uint32 i = 0; i < RaytracingHitGroupEntries.Count(); ++i)
		{
			uint32 materialIndex = RaytracingHitGroupEntries[i].MaterialIndex;
			if (materialIndex >= materialCB.ObjectCount)
			{
				materialIndex = 0u;
			}

			const uint64 materialCbAddress =
				materialCB.GpuResource->GetGPUVirtualAddress() + materialIndex * materialCB.DataSize;
			const uint64 primaryHitRecordOffset =
				hitGroupsSectionOffset + static_cast<uint64>(i * 2u) * hitRecordStride;
			const uint64 shadowHitRecordOffset = primaryHitRecordOffset + hitRecordStride;

			memcpy(sbtData + primaryHitRecordOffset + hitRecordCbOffset, &materialCbAddress, sizeof(materialCbAddress));
			memcpy(sbtData + primaryHitRecordOffset + hitRecordPassCbOffset, &passCbAddress, sizeof(passCbAddress));
			memcpy(sbtData + shadowHitRecordOffset + hitRecordCbOffset, &materialCbAddress, sizeof(materialCbAddress));
			memcpy(sbtData + shadowHitRecordOffset + hitRecordPassCbOffset, &passCbAddress, sizeof(passCbAddress));
		}
	}

	SbtStorage->Unmap(0, nullptr);
}

D3D12_DISPATCH_RAYS_DESC CRenderer::BuildDispatchRaysDesc ()
{
	D3D12_DISPATCH_RAYS_DESC desc = {};

	// The ray generation shaders are always at the beginning of the SBT.
	const uint32_t rayGenerationSectionSizeInBytes = SbtHelper.GetRayGenSectionSize();
	desc.RayGenerationShaderRecord.StartAddress = SbtStorage->GetGPUVirtualAddress();
	desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

	// Miss shaders come immediately after ray generation.
	const uint32_t missSectionSizeInBytes = SbtHelper.GetMissSectionSize();
	desc.MissShaderTable.StartAddress = SbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
	desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
	desc.MissShaderTable.StrideInBytes = SbtHelper.GetMissEntrySize();

	// Hit groups follow miss shaders.
	const uint32_t hitGroupsSectionSize = SbtHelper.GetHitGroupSectionSize();
	desc.HitGroupTable.StartAddress = SbtStorage->GetGPUVirtualAddress() +
									rayGenerationSectionSizeInBytes +
									missSectionSizeInBytes;
	desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
	desc.HitGroupTable.StrideInBytes = SbtHelper.GetHitGroupEntrySize();

	const Vector2f drawRect = Window->GetWindowSize();
	desc.Width = static_cast<uint32>(drawRect.x);
	desc.Height = static_cast<uint32>(drawRect.y);
	desc.Depth = 1;

	return desc;
}

void CRenderer::DispatchRaytracingToCurrentFrameBuffer ()
{
	ID3D12DescriptorHeap* heaps[] = { SrvUavHeap.Get() };
	CommandList->SetDescriptorHeaps(_countof(heaps), heaps);

	UpdateRaytracingShaderTableAddresses();

	CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(
		RtOutputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	CommandList->ResourceBarrier(1, &transition);

	D3D12_DISPATCH_RAYS_DESC dispatchDesc = BuildDispatchRaysDesc();
	CommandList->SetPipelineState1(RtStateObject.Get());
	CommandList->DispatchRays(&dispatchDesc);

	// Copy the DXR output into the active frame buffer used for display.
	transition = CD3DX12_RESOURCE_BARRIER::Transition(
		RtOutputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	CommandList->ResourceBarrier(1, &transition);
	transition = CD3DX12_RESOURCE_BARRIER::Transition(
		FrameBuffer[CurrentBackBufferIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COPY_DEST);
	CommandList->ResourceBarrier(1, &transition);

	CommandList->CopyResource(
		FrameBuffer[CurrentBackBufferIndex].Get(),
		RtOutputResource.Get());

	transition = CD3DX12_RESOURCE_BARRIER::Transition(
		FrameBuffer[CurrentBackBufferIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	CommandList->ResourceBarrier(1, &transition);
}

#pragma endregion Raytracing
}
