#pragma once

#include <string>

#include "Core.h"
#include "GraphicsCoreTypes.h"
#include "Memory/Memory.h"
#include "Mesh.h"
#include "Texture.h"

struct ID3D12Resource;

namespace frt::graphics
{
	struct Mesh;
	struct Vertex;
	struct Texture;
}


namespace frt::graphics
{
	struct FRT_CORE_API Model
	{
		memory::TMemoryHandleArray<Mesh, memory::DefaultAllocator> meshes;
		memory::TMemoryHandleArray<Texture, memory::DefaultAllocator> textures;

		memory::TMemoryHandleArray<Vertex, memory::DefaultAllocator> vertices;
		memory::TMemoryHandleArray<uint32, memory::DefaultAllocator> indices;

		ID3D12Resource* vertexBuffer = nullptr;
		ID3D12Resource* indexBuffer = nullptr;

		static Model LoadFromFile(const std::string& filename);
	};
}
