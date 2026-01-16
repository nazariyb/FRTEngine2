#include "Entity.h"

#include "GameInstance.h"
#include "Timer.h"
#include "Graphics/Render/Renderer.h"

void frt::CEntity::Tick(float DeltaSeconds)
{
	const float timeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	const double scale = std::cos(timeElapsed) / 8.f + .25f;
	Transform.SetScale(1.f);
	// Transform.SetTranslation(1.2f, 0.f, 0.f);
	Transform.SetRotation(0.f, math::PI_OVER_FOUR * timeElapsed, -math::PI_OVER_FOUR / 2.f);
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

	for (uint32 sectionIndex = 0; sectionIndex < model.Sections.Count(); ++sectionIndex)
	{
		const graphics::SRenderSection& section = model.Sections[sectionIndex];

		if (section.MaterialIndex < model.Textures.Count())
		{
			CommandList->SetGraphicsRootDescriptorTable(0, model.Textures[section.MaterialIndex].GpuDescriptor);
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
