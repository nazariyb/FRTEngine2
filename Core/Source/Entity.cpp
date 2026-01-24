#include "Entity.h"

#include "GameInstance.h"
#include "Timer.h"
#include "Graphics/Render/Renderer.h"

void frt::CEntity::Tick(float DeltaSeconds)
{
	const float timeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	Transform.SetScale(1.f);
	Transform.SetRotation(0.f, math::PI_OVER_FOUR * timeElapsed, 0.f);
}

#if !defined(FRT_HEADLESS)
void frt::CEntity::Present (float DeltaSeconds, ID3D12GraphicsCommandList* CommandList)
{
	if (!RenderModel.Model)
	{
		return;
	}

	const graphics::SRenderModel& model = *RenderModel.Model;
	if (!model.VertexBufferGpu || !model.IndexBufferGpu)
	{
		return;
	}

	{
		D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
		indexBufferView.BufferLocation = model.IndexBufferGpu->GetGPUVirtualAddress();
		indexBufferView.SizeInBytes = model.Indices.Count() * sizeof(uint32);
		indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		CommandList->IASetIndexBuffer(&indexBufferView);
	}
	{
		D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[1] = {};
		vertexBufferViews[0].BufferLocation = model.VertexBufferGpu->GetGPUVirtualAddress();
		vertexBufferViews[0].SizeInBytes = model.Vertices.Count() * sizeof(graphics::SVertex);
		vertexBufferViews[0].StrideInBytes = sizeof(graphics::SVertex);
		CommandList->IASetVertexBuffers(0, 1, vertexBufferViews);
	}

	memory::TRefWeak<graphics::CRenderer> renderer = GameInstance::GetInstance().GetRenderer();
	if (!renderer)
	{
		return;
	}

	for (uint32 sectionIndex = 0; sectionIndex < model.Sections.Count(); ++sectionIndex)
	{
		const graphics::SRenderSection& section = model.Sections[sectionIndex];
		if (section.MaterialIndex < model.Materials.Count())
		{
			memory::TRefShared<graphics::SMaterial> material = model.Materials[section.MaterialIndex];
			D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = renderer->GetDefaultWhiteTextureGpu();
			if (material && material->bHasBaseColorTexture)
			{
				textureHandle = material->BaseColorTexture.GpuDescriptor;
			}

			CommandList->SetGraphicsRootDescriptorTable(
				render::constants::RootParam_BaseColorTexture,
				textureHandle);

			if (material)
			{
				ID3D12PipelineState* pipelineState = renderer->GetPipelineStateForMaterial(*material);
				if (pipelineState)
				{
					CommandList->SetPipelineState(pipelineState);
				}

				graphics::SFrameResources& frameResources = renderer->GetCurrentFrameResource();
				const auto& materialHandles = frameResources.MaterialCB.DescriptorHeapHandleGpu;
				if (material->RuntimeIndex < materialHandles.size())
				{
					CommandList->SetGraphicsRootDescriptorTable(
						render::constants::RootParam_MaterialCbv,
						materialHandles[material->RuntimeIndex]);
				}
			}
		}

		CommandList->DrawIndexedInstanced(
			section.IndexCount,
			1,
			section.IndexOffset,
			section.VertexOffset,
			0);
	}
}
#endif
