#pragma once

#include <filesystem>
#include <string>
#include <DirectXMath.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Texture.h"


namespace frt::graphics
{
struct FRT_CORE_API SMaterial
{
	static constexpr uint32 InvalidTextureIndex = 0xFFFFFFFFu;

	std::string Name;
	std::string VertexShaderName;
	std::string PixelShaderName;
	std::string BaseColorTexturePath;
	std::filesystem::path SourcePath;
	std::filesystem::file_time_type LastWriteTime;

	DirectX::XMFLOAT4 DiffuseAlbedo = { 1.f, 1.f, 1.f, 1.f };
	uint32 RuntimeIndex = 0u;

	D3D12_CULL_MODE CullMode = D3D12_CULL_MODE_NONE;
	D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	bool bDepthEnable = true;
	bool bDepthWrite = true;
	bool bAlphaBlend = false;

	STexture BaseColorTexture = {};
	bool bHasBaseColorTexture = false;
	std::filesystem::path LoadedBaseColorTexturePath;
};
}
