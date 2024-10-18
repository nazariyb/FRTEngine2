#pragma once

#include "Core.h"
#include "CoreTypes.h"

struct ID3D12Resource;

namespace frt::graphics
{
	struct FRT_CORE_API Texture
	{
		int32 width;
		int32 height;
		uint32* texels;

		ID3D12Resource* gpuTexture;
		D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor;
	};
}
