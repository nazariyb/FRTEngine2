#include "Model.h"

#include <stdlib.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "Mesh.h"
#include "Texture.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ASSERT(x) frt_assert(x)
#include <stb_image.h>

#include "GameInstance.h"
#include "Renderer.h"


namespace frt::graphics
{
	void Model::CreateRenderData(ID3D12Device* Device, DX12_Arena& BufferArena, DX12_DescriptorHeap& DescriptorHeap)
	{
		ConstantBuffer = SConstantBuffer<math::STransform>(Device, BufferArena, DescriptorHeap);
	}

	Model Model::LoadFromFile(const std::string& filename, const std::string& texturePath)
	{
		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(filename, aiProcess_Triangulate | aiProcess_OptimizeMeshes | aiProcess_FlipWindingOrder);
		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
		{
			frt_assert(false);
			return {};
		}

		Model result;

		// Textures
		auto materialTextureMap = memory::NewArray<uint32>(scene->mNumMaterials);
		uint32 textureNum = 0;
		for (int64 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
		{
			const aiMaterial* material = scene->mMaterials[materialIndex];
			frt_assert(material);

			if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
			{
				materialTextureMap[materialIndex] = textureNum;
				textureNum += 1;
			}
			else
			{
				materialTextureMap[materialIndex] = 0xFFFFFFFF;
			}
		}

		result.textures = memory::NewArray<Texture>(textureNum);

		int32 textureIndex = 0;
		for (int64 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
		{
			const aiMaterial* material = scene->mMaterials[materialIndex];
			frt_assert(material);

			if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
			{
				aiString textureName; // TODO: this should be used
				material->GetTexture(aiTextureType_DIFFUSE, 0, &textureName);

				Texture& texture = result.textures[textureIndex];

				int32 channelNum = 0;
				texture.texels = (uint32*)stbi_load(texturePath.c_str(), &texture.width, &texture.height, &channelNum, 4);

				{
					D3D12_RESOURCE_DESC Desc = {};
					Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					Desc.Width = texture.width;
					Desc.Height = texture.height;
					Desc.DepthOrArraySize = 1;
					Desc.MipLevels = 1;
					Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					Desc.SampleDesc.Count = 1;
					Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
					texture.gpuTexture = GameInstance::GetInstance().GetGraphics().CreateTextureAsset(
						Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, texture.texels);

					D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
					srvDesc.Format = Desc.Format;
					srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srvDesc.Texture2D.MostDetailedMip = 0;
					srvDesc.Texture2D.MipLevels = 1;
					srvDesc.Texture2D.PlaneSlice = 0;

					D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
					GameInstance::GetInstance().GetGraphics().CreateShaderResourceView(
						texture.gpuTexture, srvDesc, &cpuDescriptor, &texture.gpuDescriptor);
				}

				++textureIndex;
			}
		}

		// Meshes
		const int32 meshesNum = (int32)scene->mNumMeshes;
		result.meshes = memory::NewArray<Mesh>(meshesNum);

		int32 indexNum = 0;
		int32 vertexNum = 0;

		for (int32 i = 0; i < meshesNum; ++i)
		{
			const aiMesh* srcMesh = scene->mMeshes[i];
			frt_assert(srcMesh);
			Mesh& dstMesh = result.meshes[i];

			dstMesh.textureIndex = materialTextureMap[srcMesh->mMaterialIndex];

			dstMesh.indexOffset = indexNum;
			dstMesh.vertexOffset = vertexNum;

			dstMesh.indexCount = srcMesh->mNumFaces * 3;
			dstMesh.vertexCount = srcMesh->mNumVertices;

			indexNum += static_cast<int32>(dstMesh.indexCount);
			vertexNum += static_cast<int32>(dstMesh.vertexCount);
		}

		// Vertices & Indices
		result.vertices = memory::NewArray<Vertex>(vertexNum);
		result.indices = memory::NewArray<uint32>(indexNum);

		for (int64 i = 0; i < meshesNum; ++i)
		{
			const aiMesh* srcMesh = scene->mMeshes[i];
			frt_assert(srcMesh);
			Mesh& dstMesh = result.meshes[i];

			for (int64 vertexIndex = 0; vertexIndex < dstMesh.vertexCount; ++vertexIndex)
			{
				Vertex& vertex = result.vertices[dstMesh.vertexOffset + vertexIndex];
				vertex.position[0] = srcMesh->mVertices[vertexIndex].x;
				vertex.position[1] = srcMesh->mVertices[vertexIndex].y;
				vertex.position[2] = srcMesh->mVertices[vertexIndex].z;

				if (srcMesh->mTextureCoords[0])
				{
					vertex.uv[0] = srcMesh->mTextureCoords[0][vertexIndex].x;
					vertex.uv[1] = srcMesh->mTextureCoords[0][vertexIndex].y;
				}
				else
				{
					vertex.uv[0] = 0;
					vertex.uv[1] = 0;
				}
			}

			for (int64 faceIndex = 0; faceIndex < srcMesh->mNumFaces; ++faceIndex)
			{
				uint32* indices = &result.indices[dstMesh.indexOffset + faceIndex * 3];

				indices[0] = srcMesh->mFaces[faceIndex].mIndices[0];
				indices[1] = srcMesh->mFaces[faceIndex].mIndices[1];
				indices[2] = srcMesh->mFaces[faceIndex].mIndices[2];
			}
		}

		Renderer& graphics = GameInstance::GetInstance().GetGraphics();

		{
			D3D12_RESOURCE_DESC vbDesc = {};
			vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			vbDesc.Alignment = 0;
			vbDesc.Width = sizeof(Vertex) * vertexNum;
			vbDesc.Height = 1;
			vbDesc.DepthOrArraySize = 1;
			vbDesc.MipLevels = 1;
			vbDesc.Format = DXGI_FORMAT_UNKNOWN;
			vbDesc.SampleDesc.Count = 1;
			vbDesc.SampleDesc.Quality = 0;
			vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			result.vertexBuffer = graphics.CreateBufferAsset(
				vbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, result.vertices.Get());
		}

		{
			D3D12_RESOURCE_DESC ibDesc = {};
			ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ibDesc.Alignment = 0;
			ibDesc.Width = sizeof(uint32) * indexNum;
			ibDesc.Height = 1;
			ibDesc.DepthOrArraySize = 1;
			ibDesc.MipLevels = 1;
			ibDesc.Format = DXGI_FORMAT_UNKNOWN;
			ibDesc.SampleDesc.Count = 1;
			ibDesc.SampleDesc.Quality = 0;
			ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ibDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			result.indexBuffer = graphics.CreateBufferAsset(
				ibDesc, D3D12_RESOURCE_STATE_INDEX_BUFFER, result.indices.Get());
		}

		result.CreateRenderData(graphics.GetDevice(), graphics.GetBufferArena(), graphics.GetDescriptorHeap());

		return result;
	}

	Model Model::CreateCube()
	{
		Model result;

		const uint32 textureNum = 1;
		result.textures = memory::NewArray<Texture>(textureNum);

		int32 textureIndex = 0;
		for (int64 materialIndex = 0; materialIndex < 1; ++materialIndex)
		{
			Texture& texture = result.textures[textureIndex];

			texture.width = 1024;
			texture.height = 1024;
			texture.texels = (uint32*)malloc(1024ull*1024*sizeof(uint32));
			memset(texture.texels, 0, 1024ull*1024*sizeof(uint32));

			{
				D3D12_RESOURCE_DESC Desc = {};
				Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				Desc.Width = texture.width;
				Desc.Height = texture.height;
				Desc.DepthOrArraySize = 1;
				Desc.MipLevels = 1;
				Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				Desc.SampleDesc.Count = 1;
				Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				texture.gpuTexture = GameInstance::GetInstance().GetGraphics().CreateTextureAsset(
					Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, texture.texels);

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = Desc.Format;
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Texture2D.MostDetailedMip = 0;
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Texture2D.PlaneSlice = 0;

				D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
				GameInstance::GetInstance().GetGraphics().CreateShaderResourceView(
					texture.gpuTexture, srvDesc, &cpuDescriptor, &texture.gpuDescriptor);
			}

			++textureIndex;
		}

		// Meshes
		const int32 meshesNum = 1;
		result.meshes = memory::NewArray<Mesh>(meshesNum);

		int32 indexNum = 0;
		int32 vertexNum = 0;

		for (int32 i = 0; i < meshesNum; ++i)
		{
			Mesh& dstMesh = result.meshes[i];

			dstMesh.textureIndex = 0;

			dstMesh.indexOffset = indexNum;
			dstMesh.vertexOffset = vertexNum;

			dstMesh.indexCount = 3;
			dstMesh.vertexCount = 3;

			indexNum += static_cast<int32>(dstMesh.indexCount);
			vertexNum += static_cast<int32>(dstMesh.vertexCount);
		}

		// Vertices & Indices
		result.vertices = memory::NewArray<Vertex>(vertexNum);
		result.indices = memory::NewArray<uint32>(indexNum);

		for (int64 i = 0; i < meshesNum; ++i)
		{
			Mesh& dstMesh = result.meshes[i];

			Vertex vertices[3] =
			{
				{ {   0.0f,  0.5f, 0.f }, { 0.5f, 1.f } },
				{ {  0.5f, -0.5f, 0.f }, { 1.f, 0.f } },
				{ { -0.5f, -0.5f, 0.f }, { 0.f, 0.f } },
			};
			for (int64 vertexIndex = 0; vertexIndex < dstMesh.vertexCount; ++vertexIndex)
			{
				Vertex& vertex = result.vertices[dstMesh.vertexOffset + vertexIndex];
				vertex.position[0] = vertices[vertexIndex].position[0];
				vertex.position[1] = vertices[vertexIndex].position[1];
				vertex.position[2] = vertices[vertexIndex].position[2];

				vertex.uv[0] = vertices[vertexIndex].uv[0];
				vertex.uv[1] = vertices[vertexIndex].uv[1];
			}

			result.indices[0] = 0;
			result.indices[1] = 1;
			result.indices[2] = 2;
		}

		Renderer& graphics = GameInstance::GetInstance().GetGraphics();

		{
			D3D12_RESOURCE_DESC vbDesc = {};
			vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			vbDesc.Alignment = 0;
			vbDesc.Width = sizeof(Vertex) * vertexNum;
			vbDesc.Height = 1;
			vbDesc.DepthOrArraySize = 1;
			vbDesc.MipLevels = 1;
			vbDesc.Format = DXGI_FORMAT_UNKNOWN;
			vbDesc.SampleDesc.Count = 1;
			vbDesc.SampleDesc.Quality = 0;
			vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			result.vertexBuffer = graphics.CreateBufferAsset(
				vbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, result.vertices.Get());
		}

		{
			D3D12_RESOURCE_DESC ibDesc = {};
			ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ibDesc.Alignment = 0;
			ibDesc.Width = sizeof(uint32) * indexNum;
			ibDesc.Height = 1;
			ibDesc.DepthOrArraySize = 1;
			ibDesc.MipLevels = 1;
			ibDesc.Format = DXGI_FORMAT_UNKNOWN;
			ibDesc.SampleDesc.Count = 1;
			ibDesc.SampleDesc.Quality = 0;
			ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ibDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			result.indexBuffer = graphics.CreateBufferAsset(
				ibDesc, D3D12_RESOURCE_STATE_INDEX_BUFFER, result.indices.Get());
		}

		return result;
	}
}
