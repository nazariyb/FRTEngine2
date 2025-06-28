#pragma once

#include <string>

#include "Core.h"
#include "Mesh.h"
#include "Containers/Array.h"
#include "Render/ConstantBuffer.h"
#include "Render/GraphicsCoreTypes.h"
#include "Render/Texture.h"

struct ID3D12Resource;


namespace frt::graphics
{
struct SMesh;
struct SVertex;
struct STexture;
}


namespace frt::graphics
{
struct FRT_CORE_API SModel
{
	TArray<SMesh> Meshes;
	TArray<STexture> Textures;

	TArray<SVertex> Vertices;
	TArray<uint32> Indices;

	ID3D12Resource* VertexBuffer = nullptr;
	ID3D12Resource* IndexBuffer = nullptr;

	static SModel LoadFromFile (const std::string& Filename, const std::string& TexturePath);
};
}
