#pragma once

#include <filesystem>
#include <string>

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

	STexture BaseColorTexture = {};
	bool bHasBaseColorTexture = false;
	std::filesystem::path LoadedBaseColorTexturePath;
};
}
