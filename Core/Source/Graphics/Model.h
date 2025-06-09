#pragma once

#include <string>

#include "ConstantBuffer.h"
#include "Core.h"
#include "GraphicsCoreTypes.h"
#include "Mesh.h"
#include "Texture.h"
#include "Containers/Array.h"
#include "Math/Transform.h"

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
