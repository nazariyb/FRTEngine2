#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Graphics/Render/RHI/ShaderCompiler.h"


namespace frt::graphics::rhi
{
class CDevice;

/**
 * Global root signature, shared by every compute, graphics and ray tracing pass:
 *   param 0  b0  root constants (RootConstantCount DWORDs)  - small per-dispatch values
 *   param 1  b1  root CBV, frame constants                   - FrameConstants.hlsli
 *   param 2  b2  root CBV, pass constants                    - per pass, from the upload ring
 *   param 3  t0  root SRV, scene TLAS
 *   s0..s5       static samplers (see Samplers.hlsli)
 * All other resources are reached through ResourceDescriptorHeap[] with indices carried in the
 * constants, so a pass never builds descriptor tables.
 */
enum ERootParameter : uint32
{
	RootParam_Constants = 0,
	RootParam_FrameConstants = 1,
	RootParam_PassConstants = 2,
	RootParam_SceneTlas = 3,
	RootParam_Count
};

inline constexpr uint32 RootConstantCount = 16u;

using HPipeline = uint32;
using HShaderLibrary = uint32;
inline constexpr uint32 InvalidPipelineHandle = 0xFFFFFFFFu;

struct SGraphicsPipelineDesc
{
	SShaderCompileDesc VertexShader;
	SShaderCompileDesc PixelShader;
	std::vector<D3D12_INPUT_ELEMENT_DESC> InputLayout;   // empty = no IA (fullscreen passes)
	DXGI_FORMAT RenderTargetFormats[8] = {};
	uint32 RenderTargetCount = 1u;
	DXGI_FORMAT DepthFormat = DXGI_FORMAT_UNKNOWN;
	bool bDepthTest = false;
	bool bDepthWrite = false;
	D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // reversed Z
	D3D12_CULL_MODE CullMode = D3D12_CULL_MODE_NONE;
	bool bAlphaBlend = false;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE Topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
};

class FRT_CORE_API CPipelineLibrary
{
public:
	void Init (CDevice& Device, CShaderCompiler& Compiler);
	void Shutdown ();

	ID3D12RootSignature* GetComputeRootSignature () const { return ComputeRootSignature.Get(); }
	ID3D12RootSignature* GetGraphicsRootSignature () const { return GraphicsRootSignature.Get(); }

	// Programs are identified by kind + name. Creating one that already exists returns the
	// existing handle (debug builds assert the description matches), so every instance of a
	// pass - one per render view - shares one compiled program and one hot-reload watch.
	HPipeline CreateCompute (std::string_view Name, const SShaderCompileDesc& Shader);
	HPipeline CreateGraphics (std::string_view Name, const SGraphicsPipelineDesc& Desc);
	ID3D12PipelineState* Get (HPipeline Handle) const;

	// DXIL libraries for ray tracing state objects. The owner compares GetLibraryVersion against
	// what it built with and rebuilds its state object when it changes.
	HShaderLibrary RegisterLibrary (std::string_view Name, const SShaderCompileDesc& Desc);
	const SShaderBinary* GetLibrary (HShaderLibrary Handle) const;
	uint32 GetLibraryVersion (HShaderLibrary Handle) const;

	// Recompiles every program whose source or any include changed on disk. Successful
	// rebuilds replace the old object (old one is released after the GPU is done with it);
	// failures keep the previous object and print the compiler log.
	bool PollHotReload ();
	void SetHotReloadEnabled (bool bEnabled) { bHotReload = bEnabled; }

	// Every compile error since the last successful reload, for the UI.
	const std::vector<std::string>& GetErrors () const { return Errors; }

	CShaderCompiler& GetCompiler () { return *Compiler; }

private:
	enum class EKind : uint8 { Compute, Graphics, Library };

	struct SWatchedFile
	{
		std::filesystem::path Path;
		std::filesystem::file_time_type WriteTime;
	};

	struct SProgram
	{
		std::string Name;
		EKind Kind = EKind::Compute;
		SShaderCompileDesc Shader;           // compute / library
		SGraphicsPipelineDesc Graphics;       // graphics
		ComPtr<ID3D12PipelineState> Pso;
		SShaderBinary Library;
		uint32 Version = 0u;
		std::vector<SWatchedFile> Watched;
	};

	uint32 FindOrAdd (EKind Kind, std::string_view Name, SProgram&& Program);
	bool Build (SProgram& Program);
	void Watch (SProgram& Program, const std::vector<std::filesystem::path>& Dependencies);
	bool IsStale (const SProgram& Program) const;

	CDevice* Device = nullptr;
	CShaderCompiler* Compiler = nullptr;
	ComPtr<ID3D12RootSignature> ComputeRootSignature;
	ComPtr<ID3D12RootSignature> GraphicsRootSignature;
	std::vector<SProgram> Programs;
	std::unordered_map<std::string, uint32> ProgramIndex;   // "<kind>:<name>" -> Programs index
	std::vector<std::string> Errors;
	bool bHotReload = true;
	uint32 PollCounter = 0u;
};

// Binds the global root signature + bindless heap for compute work on a command list.
FRT_CORE_API void BindComputeRoot (
	ID3D12GraphicsCommandList* CommandList,
	CPipelineLibrary& Pipelines,
	CDevice& Device,
	D3D12_GPU_VIRTUAL_ADDRESS FrameConstants,
	D3D12_GPU_VIRTUAL_ADDRESS SceneTlas);

FRT_CORE_API void BindGraphicsRoot (
	ID3D12GraphicsCommandList* CommandList,
	CPipelineLibrary& Pipelines,
	CDevice& Device,
	D3D12_GPU_VIRTUAL_ADDRESS FrameConstants,
	D3D12_GPU_VIRTUAL_ADDRESS SceneTlas);
}
