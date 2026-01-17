#pragma once

#include <d3d12.h>

#include "Core.h"
#include "CoreTypes.h"

struct ID3D12Resource;


namespace frt::graphics
{
struct FRT_CORE_API STexture
{
	int32 Width;
	int32 Height;
	uint32* Texels;

	ID3D12Resource* GpuTexture;
	D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor;
};
}
