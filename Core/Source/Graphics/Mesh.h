#pragma once

#include "Core.h"
#include "CoreTypes.h"

namespace frt::graphics
{
	struct FRT_CORE_API Mesh
	{
		uint32 indexOffset;
		uint32 indexCount;
		uint32 vertexOffset;
		uint32 vertexCount;
		uint32 textureIndex;

		Mesh() : indexOffset(0), indexCount(0), vertexOffset(0), vertexCount(0), textureIndex(0) {}
	};
}
