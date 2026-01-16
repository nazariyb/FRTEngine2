#include "Model.h"

#include <stdlib.h>
#include <utility>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "Mesh.h"
#include "Render/Texture.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ASSERT(x) frt_assert(x)
#include <stb_image.h>

#include "GameInstance.h"
#include "Render/Renderer.h"


namespace frt::graphics
{
SRenderModel SRenderModel::LoadFromFile (const std::string& Filename, const std::string& TexturePath)
{
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(
		Filename, aiProcess_Triangulate | aiProcess_OptimizeMeshes | aiProcess_FlipWindingOrder | aiProcess_FlipUVs);
	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		frt_assert(false);
		return {};
	}

	SRenderModel result;

	// Textures
	auto materialTextureMap = TArray<uint32>(scene->mNumMaterials);
	uint32 textureNum = 0;
	for (int64 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
	{
		const aiMaterial* material = scene->mMaterials[materialIndex];
		frt_assert(material);

		if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
		{
			materialTextureMap.Add(textureNum);
			textureNum += 1;
		}
		else
		{
			materialTextureMap.Add(0xFFFFFFFF);
		}
	}

	result.Textures = TArray<STexture>(textureNum);

	int32 textureIndex = 0;
	for (int64 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
	{
		const aiMaterial* material = scene->mMaterials[materialIndex];
		frt_assert(material);

		if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
		{
			aiString textureName; // TODO: this should be used
			material->GetTexture(aiTextureType_DIFFUSE, 0, &textureName);

			STexture& texture = result.Textures.Add();

			int32 channelNum = 0;
			texture.Texels = (uint32*)stbi_load(TexturePath.c_str(), &texture.Width, &texture.Height, &channelNum, 4);

#if !defined(FRT_HEADLESS)
			{
				D3D12_RESOURCE_DESC Desc = {};
				Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				Desc.Width = texture.Width;
				Desc.Height = texture.Height;
				Desc.DepthOrArraySize = 1;
				Desc.MipLevels = 1;
				Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				Desc.SampleDesc.Count = 1;
				Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				texture.GpuTexture = GameInstance::GetInstance().GetGraphics()->CreateTextureAsset(
					Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, texture.Texels);

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = Desc.Format;
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Texture2D.MostDetailedMip = 0;
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Texture2D.PlaneSlice = 0;

				D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
				GameInstance::GetInstance().GetGraphics()->CreateShaderResourceView(
					texture.GpuTexture, srvDesc, &cpuDescriptor, &texture.GpuDescriptor);
			}
#endif
			++textureIndex;
		}
	}

	// Sections
	const int32 meshesNum = (int32)scene->mNumMeshes;
	result.Sections = TArray<SRenderSection>(meshesNum);

	int32 indexNum = 0;
	int32 vertexNum = 0;

	for (int32 i = 0; i < meshesNum; ++i)
	{
		const aiMesh* srcMesh = scene->mMeshes[i];
		frt_assert(srcMesh);
		SRenderSection& dstSection = result.Sections.Add();

		dstSection.MaterialIndex = materialTextureMap[srcMesh->mMaterialIndex];

		dstSection.IndexOffset = indexNum;
		dstSection.VertexOffset = vertexNum;

		dstSection.IndexCount = srcMesh->mNumFaces * 3;
		dstSection.VertexCount = srcMesh->mNumVertices;

		indexNum += static_cast<int32>(dstSection.IndexCount);
		vertexNum += static_cast<int32>(dstSection.VertexCount);
	}

	// Vertices & Indices
	result.Vertices = TArray<SVertex>();
	result.Vertices.SetSizeUninitialized(vertexNum);
	result.Indices = TArray<uint32>();
	result.Indices.SetSizeUninitialized(indexNum);

	for (int64 i = 0; i < meshesNum; ++i)
	{
		const aiMesh* srcMesh = scene->mMeshes[i];
		frt_assert(srcMesh);
		SRenderSection& dstSection = result.Sections[i];

		for (int64 vertexIndex = 0; vertexIndex < dstSection.VertexCount; ++vertexIndex)
		{
			SVertex& vertex = result.Vertices[dstSection.VertexOffset + vertexIndex];
			vertex.Position.x = srcMesh->mVertices[vertexIndex].x;
			vertex.Position.y = srcMesh->mVertices[vertexIndex].y;
			vertex.Position.z = srcMesh->mVertices[vertexIndex].z;

			if (srcMesh->mTextureCoords[0])
			{
				vertex.Uv.x = srcMesh->mTextureCoords[0][vertexIndex].x;
				vertex.Uv.y = srcMesh->mTextureCoords[0][vertexIndex].y;
			}
			else
			{
				vertex.Uv.x = 0;
				vertex.Uv.y = 0;
			}
		}

		for (int64 faceIndex = 0; faceIndex < srcMesh->mNumFaces; ++faceIndex)
		{
			uint32* indices = &result.Indices[dstSection.IndexOffset + faceIndex * 3];

			indices[0] = srcMesh->mFaces[faceIndex].mIndices[0];
			indices[1] = srcMesh->mFaces[faceIndex].mIndices[1];
			indices[2] = srcMesh->mFaces[faceIndex].mIndices[2];
		}
	}

#if !defined(FRT_HEADLESS)
	memory::TRefWeak<CRenderer> graphics = GameInstance::GetInstance().GetGraphics();

	{
		D3D12_RESOURCE_DESC vbDesc = {};
		vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		vbDesc.Alignment = 0;
		vbDesc.Width = sizeof(SVertex) * vertexNum;
		vbDesc.Height = 1;
		vbDesc.DepthOrArraySize = 1;
		vbDesc.MipLevels = 1;
		vbDesc.Format = DXGI_FORMAT_UNKNOWN;
		vbDesc.SampleDesc.Count = 1;
		vbDesc.SampleDesc.Quality = 0;
		vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		result.VertexBufferGpu.Attach(graphics->CreateBufferAsset(
			vbDesc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, result.Vertices.GetData()));
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

		result.IndexBufferGpu.Attach(graphics->CreateBufferAsset(
			ibDesc, D3D12_RESOURCE_STATE_INDEX_BUFFER, result.Indices.GetData()));
	}
#endif
	return result;
}

SRenderModel SRenderModel::FromMesh (SMesh&& Mesh)
{
	SRenderModel result;
	result.Vertices = std::move(Mesh.Vertices);
	result.Indices = std::move(Mesh.Indices);
	result.VertexBufferGpu = std::move(Mesh.VertexBufferGpu);
	result.IndexBufferGpu = std::move(Mesh.IndexBufferGpu);

	SRenderSection& section = result.Sections.Add();
	section.IndexOffset = 0u;
	section.VertexOffset = 0u;
	section.IndexCount = result.Indices.Count();
	section.VertexCount = result.Vertices.Count();
	section.MaterialIndex = 0xFFFFFFFF;

	// TODO: map Mesh.Texture to a texture slot when materials land.
	return result;
}
}
