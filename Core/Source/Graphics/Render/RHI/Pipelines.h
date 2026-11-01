#pragma once

#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include "Containers/StringMap.h"
#include "Functional/MulticastDelegate.h"
#include "Graphics/Render/RHI/ShaderCache.h"
#include "Graphics/Render/RHI/ShaderCompiler.h"


namespace frt::assets
{
class CFileWatcherService;
struct SFileChange;
}


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

/**
 * Every compiled program - compute, graphics, ray tracing libraries - by kind and name, with hot
 * reload in dev builds: a program records the files its build read (the source and every include
 * opened), and when the file watcher reports one of them changed - its write time differs from the
 * one the build saw - the program is marked, and RebuildChangedPrograms builds exactly the marked
 * ones again (the renderer calls it once per frame, before the scene is drawn). The watcher watches
 * the shader root, and the directory of any file a build read from outside it.
 */
class FRT_CORE_API CPipelineLibrary
{
public:
	// Programs are compiled through Shaders (the shader cache: DXIL from the DDC when it has it).
	// Watcher: the file watcher hot reload listens to (Assets/FileWatcherService.h), which must
	// outlive Shutdown; null watches nothing, and nothing is rebuilt (Release builds).
	void Init (CDevice& Device, CShaderCache& Shaders, assets::CFileWatcherService* Watcher = nullptr);
	// Releases every program and ends the watcher's subscriptions.
	void Shutdown ();
	CPipelineLibrary () = default;
	// Ends the watcher's subscriptions, should Shutdown not have run.
	~CPipelineLibrary ();
	// Pinned: its watcher subscriptions call back into it.
	CPipelineLibrary (const CPipelineLibrary&) = delete;
	CPipelineLibrary& operator= (const CPipelineLibrary&) = delete;

	ID3D12RootSignature* GetComputeRootSignature () const { return ComputeRootSignature.Get(); }
	ID3D12RootSignature* GetGraphicsRootSignature () const { return GraphicsRootSignature.Get(); }

	// Programs are identified by kind + name. Creating one that already exists returns the
	// existing handle (debug builds assert the description matches), so every instance of a
	// pass - one per render view - shares one compiled program and one hot-reload watch.
	HPipeline CreateCompute (FStringView Name, const SShaderCompileDesc& Shader);
	HPipeline CreateGraphics (FStringView Name, const SGraphicsPipelineDesc& Desc);
	ID3D12PipelineState* Get (HPipeline Handle) const;

	// DXIL libraries for ray tracing state objects. The owner compares GetLibraryVersion against
	// what it built with and rebuilds its state object when it changes.
	HShaderLibrary RegisterLibrary (FStringView Name, const SShaderCompileDesc& Desc);
	const SShaderBinary* GetLibrary (HShaderLibrary Handle) const;
	uint32 GetLibraryVersion (HShaderLibrary Handle) const;

	// Recompiles every program the file watcher reported a changed source or include of. Successful
	// rebuilds replace the old object (old one is released after the GPU is done with it);
	// failures keep the previous object and print the compiler log. True when one was rebuilt.
	bool RebuildChangedPrograms ();
	void SetHotReloadEnabled (bool bEnabled) { bHotReload = bEnabled; }

	// Every compile error since the last successful reload, for the UI.
	const std::vector<FString>& GetErrors () const { return Errors; }
	// Programs rebuilt successfully by hot reload since Init.
	uint64 GetHotReloadCount () const { return HotReloads; }
	// Every file a program's build read, once, with the number of programs that read it; sorted by
	// path. For tools and tests (the editor's smoke test edits one in a copy of the shaders).
	std::vector<std::pair<std::filesystem::path, uint32>> GetWatchedFiles () const;

	CShaderCache& GetShaderCache () { return *Shaders; }

private:
	enum class EKind : uint8 { Compute, Graphics, Library };
	static_assert(static_cast<size_t>(EKind::Library) == 2u, "ProgramIndex holds one map per kind");

	struct SWatchedFile
	{
		std::filesystem::path Path;
		std::filesystem::file_time_type WriteTime;
	};

	struct SProgram
	{
		FString Name;
		EKind Kind = EKind::Compute;
		SShaderCompileDesc Shader;           // compute / library
		SGraphicsPipelineDesc Graphics;       // graphics
		ComPtr<ID3D12PipelineState> Pso;
		SShaderBinary Library;
		uint32 Version = 0u;
		std::vector<SWatchedFile> Watched;
		// The watcher reported a file of Watched changed: RebuildChangedPrograms builds it again.
		bool bChanged = false;
	};

	uint32 FindOrAdd (EKind Kind, FStringView Name, SProgram&& Program);
	bool Build (SProgram& Program);
	void Watch (SProgram& Program, const std::vector<std::filesystem::path>& Dependencies);
	// Subscribes Directory to the watcher, unless a watched directory holds it already.
	void WatchDirectory (const std::filesystem::path& Directory);
	void Unwatch ();
	void OnFilesChanged (std::span<const assets::SFileChange> Changes);

	CDevice* Device = nullptr;
	CShaderCache* Shaders = nullptr;
	ComPtr<ID3D12RootSignature> ComputeRootSignature;
	ComPtr<ID3D12RootSignature> GraphicsRootSignature;
	std::vector<SProgram> Programs;
	TStringMap<uint32> ProgramIndex[3];   // per EKind: name -> Programs index
	std::vector<FString> Errors;
	bool bHotReload = true;
	bool bAnyChanged = false;
	uint64 HotReloads = 0u;
	assets::CFileWatcherService* FileWatcher = nullptr;
	std::vector<std::pair<std::filesystem::path, SDelegateHandle>> WatchedDirectories;
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
