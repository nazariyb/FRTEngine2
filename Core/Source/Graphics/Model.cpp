#include "Model.h"

#include <stdlib.h>
#include <atomic>
#include <filesystem>
#include <optional>
#include <utility>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/GltfMaterial.h>
#include <assimp/material.h>

#include <cstdio>

#include "Mesh.h"
#include "Containers/String.h"
#include "Graphics/AssimpWideIO.h"
#include "Memory/Memory.h"

// stb never sees a file name: images are opened by their UTF-16 names (_wfopen_s) and decoded with
// stbi_load_from_file (TextureManager.cpp), so there is no STBI_WINDOWS_UTF8.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ASSERT(x) frt_assert(x)
#include <stb_image.h>

#include "Render/MaterialLibrary.h"


namespace frt::graphics
{
namespace
{
// Assimp reports a URI as written (glTF), so an escaped name ("Brick%20Wall.png") is decoded when
// only the decoded form names a file next to the model - as CAssimpWideIOSystem opens the model's
// buffers. Embedded textures ("*N") have no escapes.
std::filesystem::path ToTexturePath (FStringView Uri, const std::filesystem::path& ModelDir)
{
	std::filesystem::path path = Uri.ToPath();
	std::error_code ec;
	if (const std::optional<FString> decoded = DecodeUriEscapes(Uri); decoded && !std::filesystem::exists(ModelDir / path, ec))
	{
		std::filesystem::path decodedPath = decoded->ToPath();
		if (std::filesystem::exists(ModelDir / decodedPath, ec))
		{
			return decodedPath;
		}
	}
	return path;
}

std::filesystem::path FindTexturePath (const aiMaterial* Material, std::initializer_list<std::pair<aiTextureType, unsigned int>> Candidates, const std::filesystem::path& ModelDir)
{
	for (const auto& [type, index] : Candidates)
	{
		if (Material->GetTextureCount(type) <= index)
		{
			continue;
		}
		aiString name;
		if (Material->GetTexture(type, index, &name) == AI_SUCCESS && name.length > 0)
		{
			return ToTexturePath(FStringView(name.data, name.length), ModelDir);
		}
	}
	return {};
}

// Assimp reports texture URIs as authored (relative to the model). Materials live next to the
// model, so a relative URI already resolves against the material file; absolute ones are made
// relative to keep the asset relocatable. Written with '/', as asset files store paths.
FString MakeMaterialRelative (const std::filesystem::path& TexturePath, const std::filesystem::path& MaterialDir)
{
	if (TexturePath.empty() || !TexturePath.is_absolute())
	{
		return FString::FromPath(TexturePath, EPathStyle::Generic);
	}
	std::error_code ec;
	const std::filesystem::path rel = std::filesystem::relative(TexturePath, MaterialDir, ec);
	return FString::FromPath(ec ? TexturePath : rel, EPathStyle::Generic);
}

SMaterial ImportMaterial (const aiMaterial* Material, FStringView FallbackName, const std::filesystem::path& MaterialDir)
{
	SMaterial m;
	aiString aiName;
	m.Name = Material->Get(AI_MATKEY_NAME, aiName) == AI_SUCCESS && aiName.length > 0 ? FStringView(aiName.data, aiName.length) : FallbackName;
	m.VertexShaderName = "VertexShader";
	m.PixelShaderName = "PixelShader";

	aiColor4D baseColor(1.0f, 1.0f, 1.0f, 1.0f);
	if (Material->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS ||
		Material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS)
	{
		m.DiffuseAlbedo = SColor(baseColor.r, baseColor.g, baseColor.b, baseColor.a);
	}

	// glTF defaults are metallic 1, roughness 1 (the factors multiply the texture). Formats
	// without PBR keys get a dielectric.
	float metallic = 0.0f;
	m.Metallic = Material->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS ? metallic : 0.0f;
	float roughness = 1.0f;
	if (Material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS)
	{
		m.Roughness = roughness;
	}

	aiColor3D emissive(0.0f, 0.0f, 0.0f);
	if (Material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
	{
		m.Emissive = SColor(emissive.r, emissive.g, emissive.b, 1.0f);
		const bool bAnyEmission = emissive.r > 0.0f || emissive.g > 0.0f || emissive.b > 0.0f;
		float strength = 1.0f;
		Material->Get(AI_MATKEY_EMISSIVE_INTENSITY, strength);
		m.EmissiveIntensity = bAnyEmission ? strength : 0.0f;
	}

	aiString alphaMode;
	if (Material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS)
	{
		const FStringView mode(alphaMode.data, alphaMode.length);
		m.AlphaMode = mode == "MASK" ? EAlphaMode::Mask : (mode == "BLEND" ? EAlphaMode::Blend : EAlphaMode::Opaque);
	}
	Material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, m.AlphaCutoff);

	int twoSided = 0;
	if (Material->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS)
	{
		m.bDoubleSided = twoSided != 0;
	}

	float specularFactor = 1.0f;
	if (Material->Get(AI_MATKEY_SPECULAR_FACTOR, specularFactor) == AI_SUCCESS)
	{
		m.SpecularFactor = specularFactor;
	}
	Material->Get(AI_MATKEY_REFRACTI, m.Ior);
	Material->Get(AI_MATKEY_TRANSMISSION_FACTOR, m.Transmission);

	m.BaseColorTexturePath = MakeMaterialRelative(
		FindTexturePath(Material, { { aiTextureType_BASE_COLOR, 0u }, { aiTextureType_DIFFUSE, 0u } }, MaterialDir), MaterialDir);
	m.NormalTexturePath = MakeMaterialRelative(
		FindTexturePath(Material, { { aiTextureType_NORMALS, 0u } }, MaterialDir), MaterialDir);
	// glTF's packed metallic-roughness texture: its own type since assimp 6, UNKNOWN before; it is
	// also reported as METALNESS and DIFFUSE_ROUGHNESS.
	m.MetallicRoughnessTexturePath = MakeMaterialRelative(
		FindTexturePath(Material, { { aiTextureType_GLTF_METALLIC_ROUGHNESS, 0u }, { aiTextureType_UNKNOWN, 0u }, { aiTextureType_METALNESS, 0u }, { aiTextureType_DIFFUSE_ROUGHNESS, 0u } }, MaterialDir), MaterialDir);
	m.EmissiveTexturePath = MakeMaterialRelative(
		FindTexturePath(Material, { { aiTextureType_EMISSIVE, 0u } }, MaterialDir), MaterialDir);
	m.OcclusionTexturePath = MakeMaterialRelative(
		FindTexturePath(Material, { { aiTextureType_LIGHTMAP, 0u }, { aiTextureType_AMBIENT_OCCLUSION, 0u } }, MaterialDir), MaterialDir);

	// An emissive texture with a zero factor would be invisible; exporters that only set the
	// texture mean "emit what the texture says".
	if (!m.EmissiveTexturePath.IsEmpty() && m.EmissiveIntensity <= 0.0f)
	{
		m.Emissive = SColor(1.0f, 1.0f, 1.0f, 1.0f);
		m.EmissiveIntensity = 1.0f;
	}
	return m;
}
}

SRenderModel SRenderModel::LoadFromFile (const std::filesystem::path& File, const std::filesystem::path& TextureOverride, CMaterialLibrary& MaterialLibrary)
{
	// Coordinate conventions. glTF assets face +Z with +Y up, so in the asset's own frame +X is
	// its left: that is exactly the engine's LUF frame, and positions, normals and tangents
	// import unchanged (an X flip here mirrors every model). The render-space conversion
	// (negate world X) happens once, on the instance transform.
	//
	// UVs: assimp stores bottom-left-origin UVs; FlipUVs restores glTF's top-left origin, which
	// is also D3D's, and negates the bitangents to match.
	Assimp::Importer importer;
	// Opens the model and its side files (buffers) by their UTF-16 names, which Assimp's own
	// handler cannot do for every name Windows allows (AssimpWideIO.h). The importer owns it.
	importer.SetIOHandler(new CAssimpWideIOSystem());
	importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
	const FString fileName = FString::FromPath(File);
	const aiScene* scene = importer.ReadFile(
		fileName.CStr(),
		aiProcess_Triangulate |
		aiProcess_SortByPType |
		aiProcess_OptimizeMeshes |
		aiProcess_FlipUVs |
		aiProcess_PreTransformVertices |
		aiProcess_GenSmoothNormals |
		aiProcess_CalcTangentSpace);
	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		std::fprintf(stderr, "[model] failed to import %s: %s\n", fileName.CStr(), importer.GetErrorString());
		frt_assert(false);
		return {};
	}

	SRenderModel result;
	result.SourcePath = File;

	const std::filesystem::path materialDir = File.parent_path();
	const FString materialBaseName = FString::FromPath(File.stem());


	result.Materials = TArray<memory::TRefShared<SMaterial>>(scene->mNumMaterials);
	for (uint32 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
	{
		const aiMaterial* material = scene->mMaterials[materialIndex];
		frt_assert(material);

		const FString materialName = FString::Format("{}_mat{}", materialBaseName, materialIndex);
		SMaterial defaultMaterial = ImportMaterial(material, materialName, materialDir);

		// Optional override used by the demo for models whose glTF lacks a base colour texture.
		if (!TextureOverride.empty() && defaultMaterial.BaseColorTexturePath.IsEmpty())
		{
			std::error_code ec;
			const std::filesystem::path abs = std::filesystem::absolute(TextureOverride, ec);
			defaultMaterial.BaseColorTexturePath = MakeMaterialRelative(abs, std::filesystem::absolute(materialDir, ec));
		}

		const std::filesystem::path materialPath = materialDir / (materialName + ".frtmat.yml").ToPath();
		result.Materials.Add(MaterialLibrary.LoadOrCreateMaterial(materialPath, defaultMaterial));
	}

	// Sections: one per aiMesh, section-local indices into one shared vertex/index buffer.
	uint32 indexNum = 0u;
	uint32 vertexNum = 0u;
	result.Sections = TArray<SRenderSection>(scene->mNumMeshes);
	for (uint32 i = 0; i < scene->mNumMeshes; ++i)
	{
		const aiMesh* srcMesh = scene->mMeshes[i];
		frt_assert(srcMesh);
		if (!(srcMesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) || !srcMesh->mNormals)
		{
			continue;
		}
		SRenderSection& section = result.Sections.Add();
		section.MaterialIndex = srcMesh->mMaterialIndex;
		section.IndexOffset = indexNum;
		section.VertexOffset = vertexNum;
		section.IndexCount = srcMesh->mNumFaces * 3u;
		section.VertexCount = srcMesh->mNumVertices;
		section.SourceMesh = i;
		indexNum += section.IndexCount;
		vertexNum += section.VertexCount;
	}

	result.Vertices.SetSizeUninitialized(vertexNum);
	result.Indices.SetSizeUninitialized(indexNum);

	for (uint32 s = 0; s < result.Sections.Count(); ++s)
	{
		const SRenderSection& section = result.Sections[s];
		const aiMesh* srcMesh = scene->mMeshes[section.SourceMesh];

		for (uint32 v = 0; v < section.VertexCount; ++v)
		{
			SVertex& vertex = result.Vertices[section.VertexOffset + v];
			const aiVector3D& p = srcMesh->mVertices[v];
			const aiVector3D& n = srcMesh->mNormals[v];
			vertex.Position = Vector3f(p.x, p.y, p.z);
			vertex.Normal = Vector3f(n.x, n.y, n.z);

			Vector3f tangent = Vector3f(1.0f, 0.0f, 0.0f);
			Vector3f bitangent = Vector3f(0.0f, 0.0f, 1.0f);
			if (srcMesh->mTangents && srcMesh->mBitangents)
			{
				const aiVector3D& t = srcMesh->mTangents[v];
				const aiVector3D& b = srcMesh->mBitangents[v];
				tangent = Vector3f(t.x, t.y, t.z);
				// After FlipUVs assimp's bitangent points along +v of the top-left UV space,
				// while glTF (OpenGL-convention) normal maps encode +Y toward -v. The shader
				// rebuilds the bitangent as cross(N, T) * sign, so store the glTF one: -b.
				bitangent = Vector3f(-b.x, -b.y, -b.z);
			}
			vertex.Tangent = tangent;
			vertex.Bitangent = bitangent;

			if (srcMesh->mTextureCoords[0])
			{
				vertex.Uv.x = srcMesh->mTextureCoords[0][v].x;
				vertex.Uv.y = srcMesh->mTextureCoords[0][v].y;
			}
			else
			{
				vertex.Uv = Vector2f(0.0f, 0.0f);
			}

			if (srcMesh->mColors[0])
			{
				const aiColor4D& c = srcMesh->mColors[0][v];
				vertex.Color = DirectX::XMFLOAT4(c.r, c.g, c.b, c.a);
			}
			else
			{
				vertex.Color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
			}
		}

		for (uint32 f = 0; f < srcMesh->mNumFaces; ++f)
		{
			const aiFace& face = srcMesh->mFaces[f];
			uint32* indices = &result.Indices[section.IndexOffset + f * 3u];
			if (face.mNumIndices == 3u)
			{
				indices[0] = face.mIndices[0];
				indices[1] = face.mIndices[1];
				indices[2] = face.mIndices[2];
			}
			else
			{
				indices[0] = indices[1] = indices[2] = 0u; // degenerate, invisible
			}
		}
	}

	return result;
}

namespace
{
std::atomic<uint64> NextModelIdentity { 1u };
}

SRenderModel::SIdentity::SIdentity ()
	: Value(NextModelIdentity.fetch_add(1u, std::memory_order_relaxed))
{
}

SRenderModel::SIdentity::SIdentity (const SIdentity&)
	: SIdentity()
{
}

SRenderModel::SIdentity& SRenderModel::SIdentity::operator= (const SIdentity&)
{
	Value = NextModelIdentity.fetch_add(1u, std::memory_order_relaxed);
	return *this;
}

SRenderModel SRenderModel::FromMesh (SMesh&& Mesh, memory::TRefShared<SMaterial> Material)
{
	SRenderModel result;
	result.Vertices = std::move(Mesh.Vertices);
	result.Indices = std::move(Mesh.Indices);

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
