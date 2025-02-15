#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Texture.h"

namespace frt::graphics
{
	struct FRT_CORE_API SMesh
	{
		uint32 indexOffset;
		uint32 indexCount;
		uint32 vertexOffset;
		uint32 vertexCount;
		uint32 textureIndex;

		memory::TMemoryHandleArray<Vertex, memory::DefaultAllocator> Vertices;
		memory::TMemoryHandleArray<uint32, memory::DefaultAllocator> Indices;

		ComPtr<ID3D12Resource> VertexBufferGpu = nullptr;
		ComPtr<ID3D12Resource> IndexBufferGpu = nullptr;

		// temp?
		memory::TMemoryHandle<Texture> Texture;

		SMesh() : indexOffset(0), indexCount(0), vertexOffset(0), vertexCount(0), textureIndex(0) {}
	};
}
