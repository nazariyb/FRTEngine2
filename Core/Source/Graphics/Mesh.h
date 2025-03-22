#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Texture.h"
#include "Containers/Array.h"


namespace frt::graphics
{
	struct FRT_CORE_API SMesh
	{
		uint32 indexOffset;
		uint32 indexCount;
		uint32 vertexOffset;
		uint32 vertexCount;
		uint32 textureIndex;

		TArray<Vertex> Vertices;
		TArray<uint32> Indices;

		ComPtr<ID3D12Resource> VertexBufferGpu = nullptr;
		ComPtr<ID3D12Resource> IndexBufferGpu = nullptr;

		// temp?
		memory::TRefShared<Texture> Texture;

		SMesh() : indexOffset(0), indexCount(0), vertexOffset(0), vertexCount(0), textureIndex(0) {}
	};
}
