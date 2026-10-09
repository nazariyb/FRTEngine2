#pragma once

#include "Graphics/Render/RHI/Device.h"
#include "Graphics/Render/RHI/Pipelines.h"
#include "Shared/FrameData.h"


namespace frt::graphics
{
/**
 * Everything a render pass needs to record GPU work for the current frame.
 *
 * Passes are plain classes that own their pipelines and textures and implement
 * Initialize / Resize / Execute. They record into CommandList through this context:
 *   - BindCompute() / BindGraphics() set the global root signature + bindless heap (the
 *     renderer calls BindCompute once per frame; call it again after anything that changed
 *     the root signature or descriptor heaps, e.g. ImGui);
 *   - PushPassConstants(T) uploads a pass constant struct to the frame upload ring and binds
 *     it at b2; SetRootConstants writes b0;
 *   - Barriers collects transitions; Dispatch flushes it before dispatching.
 * Resource states are tracked on the SGpuTexture / SGpuBuffer objects themselves.
 */
class FRT_CORE_API CRenderContext
{
public:
	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	ID3D12GraphicsCommandList4* CommandList = nullptr;
	const gpu::SFrameConstants* Frame = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS FrameConstantsVa = 0u;
	D3D12_GPU_VIRTUAL_ADDRESS SceneTlasVa = 0u;
	uint64 FrameNumber = 0u;
	rhi::CBarrierBatch Barriers;

	void BindCompute ()
	{
		rhi::BindComputeRoot(CommandList, *Pipelines, *Device, FrameConstantsVa, SceneTlasVa);
	}

	void BindGraphics ()
	{
		rhi::BindGraphicsRoot(CommandList, *Pipelines, *Device, FrameConstantsVa, SceneTlasVa);
	}

	template <typename T>
	D3D12_GPU_VIRTUAL_ADDRESS UploadConstants (const T& Constants)
	{
		return Device->GetUploadRing().Push(Constants, 256u).Gpu;
	}

	template <typename T>
	void PushPassConstants (const T& Constants)
	{
		CommandList->SetComputeRootConstantBufferView(rhi::RootParam_PassConstants, UploadConstants(Constants));
	}

	template <typename T>
	void PushGraphicsPassConstants (const T& Constants)
	{
		CommandList->SetGraphicsRootConstantBufferView(rhi::RootParam_PassConstants, UploadConstants(Constants));
	}

	void SetRootConstants (const uint32* Values, uint32 Count, uint32 Offset = 0u)
	{
		CommandList->SetComputeRoot32BitConstants(rhi::RootParam_Constants, Count, Values, Offset);
	}

	void FlushBarriers ()
	{
		Barriers.Flush(CommandList);
	}

	// Binds Pipeline, flushes barriers and dispatches. GroupsX/Y/Z are thread-group counts.
	void Dispatch (rhi::HPipeline Pipeline, uint32 GroupsX, uint32 GroupsY, uint32 GroupsZ = 1u)
	{
		ID3D12PipelineState* pso = Pipelines->Get(Pipeline);
		if (!pso || GroupsX == 0u || GroupsY == 0u || GroupsZ == 0u)
		{
			return;
		}
		FlushBarriers();
		CommandList->SetPipelineState(pso);
		CommandList->Dispatch(GroupsX, GroupsY, GroupsZ);
	}

	// Dispatch covering Width x Height pixels with 8x8 groups (the convention for image passes).
	void Dispatch2D (rhi::HPipeline Pipeline, uint32 Width, uint32 Height, uint32 GroupSize = 8u)
	{
		Dispatch(Pipeline, rhi::DivideRoundUp(Width, GroupSize), rhi::DivideRoundUp(Height, GroupSize), 1u);
	}
};

// Shader source path helper: "PathTracing/PathTracer.hlsl" -> <repo>/Core/Content/Shaders/...
FRT_CORE_API rhi::SShaderCompileDesc MakeShaderDesc (
	FStringView RelativePath,
	FStringView EntryPoint,
	FStringView Profile,
	std::initializer_list<rhi::SShaderDefine> Defines = {});
}
