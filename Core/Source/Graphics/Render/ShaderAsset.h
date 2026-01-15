#pragma once

#include "Core.h"

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "CoreTypes.h"
#include "Containers/Array.h"


namespace frt::graphics
{
enum class EShaderStage : uint8
{
	Vertex,
	Pixel,
	Compute,
	Geometry,
	Hull,
	Domain
};


struct SShaderAsset
{
	std::string Name;
	std::filesystem::path Path;
	EShaderStage Stage = EShaderStage::Vertex;
	TArray<uint8> Bytecode;

	D3D12_SHADER_BYTECODE GetBytecode () const;
};


class CShaderLibrary
{
public:
	const SShaderAsset* LoadShader (const char* Name, const std::filesystem::path& Path, EShaderStage Stage);
	const SShaderAsset* GetShader (const char* Name) const;

private:
	std::unordered_map<std::string, SShaderAsset> Shaders;
};
}
