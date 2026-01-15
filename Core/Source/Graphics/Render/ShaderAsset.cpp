#include "ShaderAsset.h"

#include <cstdio>
#include <limits>
#include <utility>

#include "Asserts.h"


namespace frt::graphics
{
static TArray<uint8> ReadFileBytes (const std::filesystem::path& Path)
{
	frt_assert(!Path.empty());

	const std::string pathString = Path.string();
	FILE* file = nullptr;
	const int32 openResult = fopen_s(&file, pathString.c_str(), "rb");
	if (openResult != 0 || !file)
	{
		frt_assert(false);
		return {};
	}

	(void)fseek(file, 0, SEEK_END);
	const int64 fileSize = static_cast<int64>(ftell(file));
	(void)fseek(file, 0, SEEK_SET);

	frt_assert(fileSize >= 0);
	fileSize <= std::numeric_limits<int64>::max();

	TArray<uint8> bytes;
	if (fileSize > 0)
	{
		const uint32 byteCount = static_cast<uint32>(fileSize);
		bytes.SetSizeUninitialized(byteCount);
		const uint32 bytesRead = static_cast<uint32>(
			fread(bytes.GetData(), 1, static_cast<size_t>(byteCount), file));
		frt_assert(bytesRead == byteCount);
	}

	(void)fclose(file);
	return bytes;
}


D3D12_SHADER_BYTECODE SShaderAsset::GetBytecode () const
{
	D3D12_SHADER_BYTECODE bytecode = {};
	bytecode.pShaderBytecode = Bytecode.IsEmpty() ? nullptr : Bytecode.GetData();
	bytecode.BytecodeLength = static_cast<SIZE_T>(Bytecode.GetSize());
	return bytecode;
}


const SShaderAsset* CShaderLibrary::LoadShader (const char* Name, const std::filesystem::path& Path, EShaderStage Stage)
{
	frt_assert(Name);
	frt_assert(!Path.empty());

	auto it = Shaders.find(Name);
	if (it != Shaders.end())
	{
		frt_assert(it->second.Path == Path);
		frt_assert(it->second.Stage == Stage);
		return &it->second;
	}

	SShaderAsset asset;
	asset.Name = Name;
	asset.Path = Path;
	asset.Stage = Stage;
	asset.Bytecode = ReadFileBytes(Path);
	frt_assert(!asset.Bytecode.IsEmpty());

	auto [insertedIt, inserted] = Shaders.emplace(asset.Name, std::move(asset));
	frt_assert(inserted);
	return &insertedIt->second;
}

const SShaderAsset* CShaderLibrary::GetShader (const char* Name) const
{
	frt_assert(Name);

	auto it = Shaders.find(Name);
	if (it == Shaders.end())
	{
		return nullptr;
	}

	return &it->second;
}
}
