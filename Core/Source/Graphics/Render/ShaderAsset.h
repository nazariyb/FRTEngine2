#pragma once

#include "Core.h"

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <string_view>
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

struct SShaderDefine
{
	std::string Name;
	std::string Value;
};


struct SShaderAsset
{
	std::string Name;
	std::filesystem::path Path;
	std::filesystem::path SourcePath;
	std::filesystem::path IncludeDir;
	std::string EntryPoint;
	std::string TargetProfile;
	EShaderStage Stage = EShaderStage::Vertex;
	TArray<SShaderDefine> Defines;
	TArray<uint8> Bytecode;
	std::filesystem::file_time_type LastWriteTime;

	D3D12_SHADER_BYTECODE GetBytecode () const;
	bool ReloadIfChanged ();
};


class CShaderLibrary
{
public:
	const SShaderAsset* LoadShader (
		std::string_view Name,
		const std::filesystem::path& CompiledPath,
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& IncludeDir,
		std::string_view EntryPoint,
		std::string_view TargetProfile,
		EShaderStage Stage,
		const TArray<SShaderDefine>& Defines);
	const SShaderAsset* GetShader (std::string_view Name) const;
	bool ReloadModifiedShaders ();

private:
	struct SStringHash
	{
		using is_transparent = void;

		size_t operator() (std::string_view Value) const noexcept;
		size_t operator() (const std::string& Value) const noexcept;
	};

	struct SStringEqual
	{
		using is_transparent = void;

		bool operator() (std::string_view Lhs, std::string_view Rhs) const noexcept;
		bool operator() (const std::string& Lhs, const std::string& Rhs) const noexcept;
		bool operator() (const std::string& Lhs, std::string_view Rhs) const noexcept;
		bool operator() (std::string_view Lhs, const std::string& Rhs) const noexcept;
	};

	std::unordered_map<std::string, SShaderAsset, SStringHash, SStringEqual> Shaders;
};
}
