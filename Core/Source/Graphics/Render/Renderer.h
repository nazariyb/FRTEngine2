#pragma once

#include <d3d12.h>
#include <dxcapi.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <wrl/client.h>

#include "Core.h"
#include "Event.h"
#include "GraphicsCoreTypes.h"
#include "MaterialLibrary.h"
#include "RenderConstants.h"
#include "ShaderAsset.h"
#include "Texture.h"
#include "Containers/Array.h"
#include "nv_helpers_dx12/ShaderBindingTableGenerator.h"


namespace frt
{
class CWindow;
}


using Microsoft::WRL::ComPtr;


namespace frt::graphics
{
class CRenderer
{
	// Core
public:
	CRenderer () = delete;
	explicit CRenderer (CWindow* Window);

	void Resize (bool bNewFullscreenState);

	IDXGIAdapter1* GetAdapter ();
	ID3D12Device5* GetDevice ();
	ID3D12CommandQueue* GetCommandQueue ();
	ID3D12GraphicsCommandList4* GetCommandList ();

	// Initialization & sync
	void InitializeRendering ();
	void BeginInitializationCommands ();
	void EndInitializationCommands ();
	void FlushCommandQueue ();

private:
	void CreateSwapChain (bool bFullscreen);
	void WaitForFenceValue (uint64 Value);

	CWindow* Window;

	// Core device/swapchain
	D3D12_VIEWPORT Viewport;
	D3D12_RECT ScissorRect;
	ComPtr<IDXGIAdapter1> Adapter;
	ComPtr<ID3D12Device5> Device;
	ComPtr<IDXGIFactory4> Factory;

	ComPtr<IDXGISwapChain1> SwapChain;
	ComPtr<ID3D12Resource> FrameBuffer[render::constants::SwapChainBufferCount];
	D3D12_CPU_DESCRIPTOR_HANDLE FrameBufferDescriptors[render::constants::SwapChainBufferCount];
	unsigned CurrentBackBufferIndex;

	SFrameResources FramesResources[render::constants::FrameResourcesBufferCount];
	uint8 CurrentFrameResourceIndex = render::constants::FrameResourcesBufferCount - 1;

	ComPtr<ID3D12CommandQueue> CommandQueue;
	ComPtr<ID3D12CommandAllocator> CommandAllocator;
	ComPtr<ID3D12GraphicsCommandList4> CommandList;

	// Synchronization
	ComPtr<ID3D12Fence> Fence;
	uint64 FenceValue;
	HANDLE FenceEvent;
	bool bCommandListRecording = false;

	// Frame lifecycle
public:
	void StartFrame ();
	void Tick ();
	void Draw ();
	SFrameResources& GetCurrentFrameResource ();

private:
	void ResetCurrentFrameCommandList ();
	void ReloadModifiedAssetsIfNeeded ();

	// Resource uploads
public:
	ID3D12Resource* CreateBufferAsset (const D3D12_RESOURCE_DESC& Desc);
	ID3D12Resource* CreateTextureAsset (const D3D12_RESOURCE_DESC& Desc);
	void EnqueueBufferUpload (
		ID3D12Resource* Resource,
		uint64 SizeInBytes,
		const void* BufferData,
		D3D12_RESOURCE_STATES FinalState);
	void EnqueueTextureUpload (
		ID3D12Resource* Resource,
		const D3D12_RESOURCE_DESC& Desc,
		const void* Texels,
		D3D12_RESOURCE_STATES FinalState);
	DX12_Arena& GetBufferArena ();

private:
	void ProcessPendingResourceUploads ();
	void RecordBufferUpload (
		ID3D12Resource* Resource,
		uint64 SizeInBytes,
		const void* BufferData,
		D3D12_RESOURCE_STATES FinalState);
	void RecordTextureUpload (
		ID3D12Resource* Resource,
		const D3D12_RESOURCE_DESC& Desc,
		uint32 RowPitch,
		const void* PackedTexels,
		D3D12_RESOURCE_STATES FinalState);
	uint32 BuildPackedTextureData (
		const D3D12_RESOURCE_DESC& Desc,
		const void* Texels,
		TArray<uint8>& OutPackedTexels) const;


	struct SPendingBufferUpload
	{
		ID3D12Resource* Resource = nullptr;
		D3D12_RESOURCE_STATES FinalState = D3D12_RESOURCE_STATE_COMMON;
		TArray<uint8> Data;
	};


	struct SPendingTextureUpload
	{
		ID3D12Resource* Resource = nullptr;
		D3D12_RESOURCE_DESC Desc = {};
		D3D12_RESOURCE_STATES FinalState = D3D12_RESOURCE_STATE_COMMON;
		uint32 RowPitch = 0u;
		TArray<uint8> PackedTexels;
	};


	DX12_Arena BufferArena;
	DX12_Arena TextureArena;

	TArray<SPendingBufferUpload> PendingBufferUploads;
	TArray<SPendingTextureUpload> PendingTextureUploads;

	// Rasterization
public:
	void EnsureObjectConstantCapacity (uint32 ObjectCount);
	void EnsureMaterialConstantCapacity (uint32 MaterialCount);
	CMaterialLibrary& GetMaterialLibrary ();
	ID3D12PipelineState* GetPipelineStateForMaterial (const SMaterial& Material);
	D3D12_GPU_DESCRIPTOR_HANDLE GetDefaultWhiteTextureGpu () const;
	void CreateShaderResourceView (
		ID3D12Resource* Texture,
		const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc,
		D3D12_CPU_DESCRIPTOR_HANDLE* OutCpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* OutGpuHandle);
	DX12_DescriptorHeap& GetDescriptorHeap ();

	frt::CEvent<> OnShaderDescriptorHeapRebuild;

	DX12_DescriptorHeap ShaderDescriptorHeap;
	bool bVSyncEnabled = true;
	DXGI_RATIONAL DisplayRefreshRate = { 0u, 1u };

private:
	struct SShaderPermutation
	{
		std::string Name;
		std::filesystem::path CompiledPath;
		std::filesystem::path SourcePath;
		TArray<SShaderDefine> Defines;
	};


	void CreateRootSignature ();
	void CreatePipelineState ();
	[[nodiscard]] SShaderPermutation BuildShaderPermutation (
		const std::string& BaseName,
		const SMaterial& Material,
		EShaderStage Stage) const;
	ComPtr<ID3D12PipelineState> BuildPipelineState (
		const SShaderPermutation& VertexShader,
		const SShaderPermutation& PixelShader,
		D3D12_CULL_MODE CullMode,
		D3D12_COMPARISON_FUNC DepthFunc,
		bool bDepthEnable,
		bool bDepthWrite,
		bool bAlphaBlend);
	void CreateDefaultWhiteTexture ();
	void EnsureShaderDescriptorCapacity (uint32 RequiredCount);
	void RebuildShaderDescriptorHeap (uint32 NewCapacity);
	void RebuildShaderDescriptors ();
	void PrepareRasterPass ();
	void TransitionCurrentBackBufferToRenderTarget ();
	void SetupCurrentBackBufferRenderTarget ();
	void BindDefaultRasterState ();

	ComPtr<ID3D12RootSignature> RootSignature;
	ComPtr<ID3D12PipelineState> PipelineState;
	CShaderLibrary ShaderLibrary;
	CMaterialLibrary MaterialLibrary;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> PipelineStateCache;
	STexture DefaultWhiteTexture = {};
	bool bPendingPipelineStateRebuild = false;

	ComPtr<ID3D12Resource> CommonConstantBuffer;
	D3D12_GPU_DESCRIPTOR_HANDLE CommonConstantBufferDescriptor;


	struct SShaderResourceViewRecord
	{
		ID3D12Resource* Resource = nullptr;
		D3D12_SHADER_RESOURCE_VIEW_DESC Desc = {};
		D3D12_GPU_DESCRIPTOR_HANDLE* GpuHandle = nullptr;
	};


	DX12_DescriptorHeap RtvHeap;
	DX12_Arena RtvArena;

	DX12_DescriptorHeap DsvHeap;
	ComPtr<ID3D12Resource> DepthStencilBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilDescriptor;

	TArray<SShaderResourceViewRecord> TrackedSrvs;

	D3D12_DESCRIPTOR_HEAP_TYPE ShaderDescriptorHeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	D3D12_DESCRIPTOR_HEAP_FLAGS ShaderDescriptorHeapFlags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	// Raytracing
public:
	void InitializeRaytracingResources ();
	raytracing::SAccelerationStructureBuffers TopLevelASBuffers;

private:
	ComPtr<ID3D12RootSignature> CreateRayGenSignature ();
	ComPtr<ID3D12RootSignature> CreateMissSignature ();
	ComPtr<ID3D12RootSignature> CreateHitSignature ();

	void CreateRaytracingPipeline ();
	void CreateRaytracingOutputBuffer ();
	void CreateShaderResourceHeap ();
	void CreateShaderBindingTable ();
	D3D12_DISPATCH_RAYS_DESC BuildDispatchRaysDesc ();
	void DispatchRaytracingToCurrentFrameBuffer ();

	ComPtr<IDxcBlob> RayGenLibrary;
	ComPtr<IDxcBlob> MissLibrary;
	ComPtr<IDxcBlob> HitLibrary;

	ComPtr<ID3D12RootSignature> RayGenSignature;
	ComPtr<ID3D12RootSignature> MissSignature;
	ComPtr<ID3D12RootSignature> HitSignature;

	ComPtr<ID3D12StateObject> RtStateObject;
	ComPtr<ID3D12StateObjectProperties> RtStateObjectProperties;

	ComPtr<ID3D12Resource> RtOutputResource;
	ComPtr<ID3D12DescriptorHeap> SrvUavHeap;

	nv_helpers_dx12::ShaderBindingTableGenerator SbtHelper;
	ComPtr<ID3D12Resource> SbtStorage;
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
