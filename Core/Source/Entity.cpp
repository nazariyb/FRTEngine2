#include "Entity.h"

#include "GameInstance.h"
#include "Timer.h"
#include "Graphics/Render/Renderer.h"

void frt::CEntity::Tick(float DeltaSeconds)
{
	const float timeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	const double scale = std::cos(timeElapsed) / 8.f + .25f;
	Transform.SetScale(1.f);
	Transform.SetTranslation(1.2f, 0.f, 0.f);
	Transform.SetRotation(0.f, math::PI_OVER_FOUR * timeElapsed, -math::PI_OVER_FOUR / 2.f);
}

#if !defined(FRT_HEADLESS)
void frt::CEntity::Present (float DeltaSeconds, ID3D12GraphicsCommandList* CommandList)
{
	{
		D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
		indexBufferView.BufferLocation = Mesh.IndexBufferGpu->GetGPUVirtualAddress();
		indexBufferView.SizeInBytes = Mesh.Indices.Count() * sizeof(uint32);
		indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		CommandList->IASetIndexBuffer(&indexBufferView);
	}
	{
		D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[1] = {};
		vertexBufferViews[0].BufferLocation = Mesh.VertexBufferGpu->GetGPUVirtualAddress();
		vertexBufferViews[0].SizeInBytes = Mesh.Vertices.Count() * sizeof(graphics::SVertex);
		vertexBufferViews[0].StrideInBytes = sizeof(graphics::SVertex);
		CommandList->IASetVertexBuffers(0, 1, vertexBufferViews);
	}

	// for (uint32 meshId = 0; meshId < Model.meshes.GetNum(); ++meshId)
	{
		// graphics::SMesh& mesh = Model.meshes[meshId];
		// graphics::Texture& texture = Model.textures[mesh.textureIndex];

		// CommandList->SetGraphicsRootDescriptorTable(0, mesh.Texture->gpuDescriptor);
		CommandList->DrawIndexedInstanced(Mesh.Indices.Count(), 1, Mesh.IndexOffset, Mesh.VertexOffset, 0);
	}
}
#endif
