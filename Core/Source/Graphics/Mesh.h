#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt::graphics
{
struct FRT_CORE_API SMesh
{
	uint32 IndexOffset;
	uint32 IndexCount;
	uint32 VertexOffset;
	uint32 VertexCount;
	uint32 TextureIndex;

	TArray<SVertex> Vertices;
	TArray<uint32> Indices;

	SMesh ()
		: IndexOffset(0u)
		, IndexCount(0u)
		, VertexOffset(0u)
		, VertexCount(0u)
		, TextureIndex(0u) {}
};
}
