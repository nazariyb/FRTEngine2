#pragma once

#include <filesystem>
#include <string>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Texture.h"
#include "Graphics/SColor.h"


namespace frt::graphics
{
enum class EMaterialFlags : uint32
{
	None = 0,
	UseBaseColorTexture = 1u << 0u
};
FRT_DECLARE_FLAG_ENUM(EMaterialFlags)

struct FRT_CORE_API SMaterial
{
	static constexpr uint32 InvalidTextureIndex = 0xFFFFFFFFu;

	std::string Name;
	std::string VertexShaderName;
	std::string PixelShaderName;
	std::string BaseColorTexturePath;
	std::filesystem::path SourcePath;
	std::filesystem::file_time_type LastWriteTime;

	SColor DiffuseAlbedo = SColor::White;
	float Metallic = 0.0f;
	float Roughness = 1.0f;
	SColor Emissive = SColor::Black;
	float EmissiveIntensity = 0.0f;

	uint32 RuntimeIndex = 0u;
	SFlags<EMaterialFlags> Flags;

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
