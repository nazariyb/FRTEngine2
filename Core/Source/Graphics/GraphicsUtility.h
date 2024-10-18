#pragma once
#include <d3d12.h>

namespace frt::graphics
{
	D3D12_SHADER_BYTECODE Dx12LoadShader(const char* Filename);
}
