#include "Model.h"

#include <stdlib.h>
#include <filesystem>
#include <utility>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "Mesh.h"
#include "Memory/Memory.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ASSERT(x) frt_assert(x)
#include <stb_image.h>

#include "GameInstance.h"
#include "Render/MaterialLibrary.h"
#include "Render/Renderer.h"


namespace frt::graphics
{
SRenderModel SRenderModel::LoadFromFile (const std::string& Filename, const std::string& TexturePath)
{
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(
		Filename,
		aiProcess_Triangulate |
		aiProcess_OptimizeMeshes |
		aiProcess_FlipWindingOrder |
		aiProcess_FlipUVs |
		aiProcess_PreTransformVertices);
	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		frt_assert(false);
		return {};
	}

	SRenderModel result;

	// Materials
	const std::filesystem::path modelPath = Filename;
	const std::filesystem::path materialDir = modelPath.parent_path();
	const std::string materialBaseName = modelPath.stem().string();

#if !defined(FRT_HEADLESS)
	memory::TRefWeak<CRenderer> renderer = GameInstance::GetInstance().GetRenderer();
	frt_assert(renderer);
	CMaterialLibrary& materialLibrary = renderer->GetMaterialLibrary();
#else
	CMaterialLibrary materialLibrary;
#endif

	result.Materials = TArray<memory::TRefShared<SMaterial>>(scene->mNumMaterials);
	for (int64 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
	{
		const aiMaterial* material = scene->mMaterials[materialIndex];
		frt_assert(material);

		aiString materialAiName;
		std::string materialName;
		if (material->Get(AI_MATKEY_NAME, materialAiName) == AI_SUCCESS)
		{
			materialName = materialAiName.C_Str();
		}

		SMaterial defaultMaterial;
		defaultMaterial.Name = materialName.empty()
			? materialBaseName + "_mat" + std::to_string(materialIndex)
			: materialName;
		defaultMaterial.VertexShaderName = "VertexShader";
		defaultMaterial.PixelShaderName = "PixelShader";

		std::filesystem::path materialPath =
			materialDir / (materialBaseName + "_mat" + std::to_string(materialIndex) + ".frtmat");

		if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
		{
			std::filesystem::path texturePath;
			if (!TexturePath.empty())
			{
				texturePath = TexturePath;
			}
			else
			{
				aiString textureName;
				if (material->GetTexture(aiTextureType_DIFFUSE, 0, &textureName) == AI_SUCCESS)
				{
					texturePath = textureName.C_Str();
				}
			}

			if (!texturePath.empty())
			{
				std::error_code ec;
				const std::filesystem::path relativePath =
					std::filesystem::relative(texturePath, materialPath.parent_path(), ec);
				defaultMaterial.BaseColorTexturePath = ec ? texturePath.string() : relativePath.string();
			}
		}

		memory::TRefShared<SMaterial> materialRef =
			materialLibrary.LoadOrCreateMaterial(materialPath, defaultMaterial);
		result.Materials.Add(materialRef);
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

		dstSection.MaterialIndex = static_cast<uint32>(srcMesh->mMaterialIndex);

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
			vertex.Position.x = -srcMesh->mVertices[vertexIndex].x;
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

		result.VertexBufferGpu.Attach(renderer->CreateBufferAsset(vbDesc));
		renderer->EnqueueBufferUpload(
			result.VertexBufferGpu.Get(),
			vbDesc.Width,
			result.Vertices.GetData(),
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
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

		result.IndexBufferGpu.Attach(renderer->CreateBufferAsset(ibDesc));
		renderer->EnqueueBufferUpload(
			result.IndexBufferGpu.Get(),
			ibDesc.Width,
			result.Indices.GetData(),
			D3D12_RESOURCE_STATE_INDEX_BUFFER);
	}
#endif
	return result;
}

SRenderModel SRenderModel::FromMesh (SMesh&& Mesh, memory::TRefShared<SMaterial> Material)
{
	SRenderModel result;
	result.Vertices = std::move(Mesh.Vertices);
	result.Indices = std::move(Mesh.Indices);
	result.VertexBufferGpu = std::move(Mesh.VertexBufferGpu);
	result.IndexBufferGpu = std::move(Mesh.IndexBufferGpu);

	if (Material)
	{
		result.Materials.Add(Material);
	}
	else
	{
		memory::TRefShared<SMaterial> material = memory::NewShared<SMaterial>();
		material->VertexShaderName = "VertexShader";
		material->PixelShaderName = "PixelShader";
		result.Materials.Add(material);
	}

	SRenderSection& section = result.Sections.Add();
	section.IndexOffset = 0u;
	section.VertexOffset = 0u;
	section.IndexCount = result.Indices.Count();
	section.VertexCount = result.Vertices.Count();
	section.MaterialIndex = 0u;

	// TODO: map Mesh.Texture to a texture slot when materials land.
	return result;
}
}
