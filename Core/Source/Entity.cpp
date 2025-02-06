#include "Entity.h"

#include "GameInstance.h"
#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Graphics/ConstantBuffer.h"
#include "Graphics/Renderer.h"

void frt::CEntity::Tick(float DeltaSeconds)
{

}

void frt::CEntity::Present(float DeltaSeconds, ID3D12GraphicsCommandList* CommandList)
{
	{
		const float TimeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
		const double scale = std::cos(TimeElapsed) / 8.f + .25f;
		Transform.SetScale(.5);
		Transform.SetTranslation(.5f, 0.f, 0.f);
		Transform.SetRotation(0.f, math::PI_OVER_FOUR * TimeElapsed, 0.f);

		const auto [renderWidth, renderHeight] = GameInstance::GetInstance().GetWindow().GetWindowSize();

		const auto& Camera = GameInstance::GetInstance().GetCamera();
		DirectX::XMMATRIX view = Camera.GetViewMatrix();
		DirectX::XMMATRIX projection = Camera.GetProjectionMatrix(90.f, (float)renderWidth / renderHeight, 1.f, 10'000.f);

		DirectX::XMFLOAT4X4 mvp;
		DirectX::XMStoreFloat4x4(&mvp, DirectX::XMLoadFloat4x4(&Transform.GetMatrix()) * view * projection);

		auto& renderer = GameInstance::GetInstance().GetGraphics();
		Model.ConstantBuffer.Upload((math::STransform*)&mvp.m, renderer.GetUploadArena(), renderer.GetCommandList());
	}

	{
		D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
		indexBufferView.BufferLocation = Model.indexBuffer->GetGPUVirtualAddress();
		indexBufferView.SizeInBytes = static_cast<uint32>(Model.indices.GetSize());
		indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		CommandList->IASetIndexBuffer(&indexBufferView);
	}
	{
		D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[1] = {};
		vertexBufferViews[0].BufferLocation = Model.vertexBuffer->GetGPUVirtualAddress();
		vertexBufferViews[0].SizeInBytes = static_cast<uint32>(Model.vertices.GetSize());
		vertexBufferViews[0].StrideInBytes = sizeof(graphics::Vertex);
		CommandList->IASetVertexBuffers(0, 1, vertexBufferViews);
	}

	for (uint32 meshId = 0; meshId < Model.meshes.GetNum(); ++meshId)
	{
		graphics::Mesh& mesh = Model.meshes[meshId];
		graphics::Texture& texture = Model.textures[mesh.textureIndex];

		CommandList->SetGraphicsRootDescriptorTable(0, texture.gpuDescriptor);
		CommandList->DrawIndexedInstanced(mesh.indexCount, 1, mesh.indexOffset, mesh.vertexOffset, 0);
	}
}
