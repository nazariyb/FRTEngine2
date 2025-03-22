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
	struct Vertex;
	struct Texture;
}


namespace frt::graphics
{
	struct FRT_CORE_API Model
	{
		TArray<SMesh> meshes;
		TArray<Texture> textures;

		TArray<Vertex> vertices;
		TArray<uint32> indices;

		ID3D12Resource* vertexBuffer = nullptr;
		ID3D12Resource* indexBuffer = nullptr;

		static Model LoadFromFile(const std::string& filename, const std::string& texturePath);
	};
}
