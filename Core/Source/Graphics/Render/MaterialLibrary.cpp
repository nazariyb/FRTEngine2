#include "MaterialLibrary.h"

#include "Material.h"

#include <d3d12.h>
#include <fstream>

#include <stb_image.h>

#include "Graphics/Render/Renderer.h"
#include "Memory/Memory.h"


namespace frt::graphics
{
static constexpr int32 MaterialFileVersion = 1;

static std::string TrimCopy (const std::string& Input)
{
	size_t start = 0;
	while (start < Input.size() && std::isspace(static_cast<unsigned char>(Input[start])))
	{
		++start;
	}

	size_t end = Input.size();
	while (end > start && std::isspace(static_cast<unsigned char>(Input[end - 1u])))
	{
		--end;
	}

	return Input.substr(start, end - start);
}

static std::string StripQuotes (const std::string& Input)
{
	if (Input.size() < 2u)
	{
		return Input;
	}

	const char first = Input.front();
	const char last = Input.back();
	if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
	{
		return Input.substr(1u, Input.size() - 2u);
	}

	return Input;
}

static bool ParseInt32 (const std::string& Input, int32* OutValue)
{
	const char* cstr = Input.c_str();
	char* end = nullptr;
	const long value = std::strtol(cstr, &end, 10);
	if (end == cstr)
	{
		return false;
	}

	*OutValue = static_cast<int32>(value);
	return true;
}

static std::string ToLowerCopy (const std::string& Input)
{
	std::string result = Input;
	for (char& ch : result)
	{
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return result;
}

static bool ParseBool (const std::string& Input, bool* OutValue)
{
	const std::string normalized = ToLowerCopy(Input);
	if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
	{
		*OutValue = true;
		return true;
	}

	if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
	{
		*OutValue = false;
		return true;
	}

	return false;
}

static bool ParseCullMode (const std::string& Input, D3D12_CULL_MODE* OutMode)
{
	const std::string normalized = ToLowerCopy(Input);
	if (normalized == "none")
	{
		*OutMode = D3D12_CULL_MODE_NONE;
		return true;
	}

	if (normalized == "front")
	{
		*OutMode = D3D12_CULL_MODE_FRONT;
		return true;
	}

	if (normalized == "back")
	{
		*OutMode = D3D12_CULL_MODE_BACK;
		return true;
	}

	return false;
}

static bool ParseDepthFunc (const std::string& Input, D3D12_COMPARISON_FUNC* OutFunc)
{
	const std::string normalized = ToLowerCopy(Input);
	if (normalized == "never")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_NEVER;
		return true;
	}
	if (normalized == "less")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_LESS;
		return true;
	}
	if (normalized == "equal")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_EQUAL;
		return true;
	}
	if (normalized == "less_equal" || normalized == "lequal")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		return true;
	}
	if (normalized == "greater")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_GREATER;
		return true;
	}
	if (normalized == "not_equal" || normalized == "notequal")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
		return true;
	}
	if (normalized == "greater_equal" || normalized == "gequal")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		return true;
	}
	if (normalized == "always")
	{
		*OutFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		return true;
	}

	return false;
}

static const char* CullModeToString (D3D12_CULL_MODE Mode)
{
	switch (Mode)
	{
		case D3D12_CULL_MODE_NONE: return "none";
		case D3D12_CULL_MODE_FRONT: return "front";
		case D3D12_CULL_MODE_BACK: return "back";
		default: return "back";
	}
}

static const char* DepthFuncToString (D3D12_COMPARISON_FUNC Func)
{
	switch (Func)
	{
		case D3D12_COMPARISON_FUNC_NEVER: return "never";
		case D3D12_COMPARISON_FUNC_LESS: return "less";
		case D3D12_COMPARISON_FUNC_EQUAL: return "equal";
		case D3D12_COMPARISON_FUNC_LESS_EQUAL: return "less_equal";
		case D3D12_COMPARISON_FUNC_GREATER: return "greater";
		case D3D12_COMPARISON_FUNC_NOT_EQUAL: return "not_equal";
		case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return "greater_equal";
		case D3D12_COMPARISON_FUNC_ALWAYS: return "always";
		default: return "less";
	}
}

static int32 GetMaterialFileVersion (const std::filesystem::path& MaterialPath)
{
	std::ifstream stream(MaterialPath);
	if (!stream.is_open())
	{
		return 0;
	}

	std::string line;
	while (std::getline(stream, line))
	{
		std::string trimmed = TrimCopy(line);
		if (trimmed.empty())
		{
			continue;
		}

		if (trimmed.rfind("#", 0) == 0u || trimmed.rfind("//", 0) == 0u)
		{
			continue;
		}

		size_t split = trimmed.find(':');
		if (split == std::string::npos)
		{
			split = trimmed.find('=');
		}

		if (split == std::string::npos)
		{
			continue;
		}

		std::string key = TrimCopy(trimmed.substr(0, split));
		std::string value = TrimCopy(trimmed.substr(split + 1));
		value = StripQuotes(value);

		if (key == "version")
		{
			int32 version = 0;
			if (ParseInt32(value, &version))
			{
				return version;
			}
		}

		break;
	}

	return 0;
}

void CMaterialLibrary::SetRenderer (CRenderer* InRenderer)
{
	frt_assert(InRenderer);
	Renderer = InRenderer;
}

memory::TRefShared<SMaterial> CMaterialLibrary::LoadOrCreateMaterial (
	const std::filesystem::path& MaterialPath,
	const SMaterial& DefaultMaterial)
{
	const std::string key = MakeKey(MaterialPath);
	auto it = Materials.find(key);
	if (it != Materials.end())
	{
		return it->second.Material;
	}

	SMaterialRecord record;
	record.Material = LoadMaterialFromFile(MaterialPath, DefaultMaterial, true);
	Materials.emplace(key, record);
	return record.Material;
}

bool CMaterialLibrary::ReloadModifiedMaterials ()
{
	bool anyReloaded = false;
	for (auto& entry : Materials)
	{
		if (!entry.second.Material)
		{
			continue;
		}

		SMaterial& material = *entry.second.Material;
		if (material.SourcePath.empty())
		{
			continue;
		}

		std::error_code ec;
		const std::filesystem::file_time_type newWriteTime =
			std::filesystem::last_write_time(material.SourcePath, ec);
		if (ec || newWriteTime == material.LastWriteTime)
		{
			continue;
		}

		const int32 fileVersion = GetMaterialFileVersion(material.SourcePath);
		if (fileVersion < MaterialFileVersion)
		{
			SaveMaterialFile(material.SourcePath, material);
			material.LastWriteTime = std::filesystem::last_write_time(material.SourcePath, ec);
			anyReloaded = true;
			continue;
		}

		if (ParseMaterialFile(material.SourcePath, material))
		{
			material.LastWriteTime = newWriteTime;
			EnsureBaseColorTexture(material);
			anyReloaded = true;
		}
	}

	return anyReloaded;
}

memory::TRefShared<SMaterial> CMaterialLibrary::LoadMaterialFromFile (
	const std::filesystem::path& MaterialPath,
	const SMaterial& DefaultMaterial,
	bool bCreateIfMissing)
{
	SMaterial material = DefaultMaterial;
	material.SourcePath = MaterialPath;

	std::error_code ec;
	const bool exists = std::filesystem::exists(MaterialPath, ec);
	if (!ec && exists)
	{
		const int32 fileVersion = GetMaterialFileVersion(MaterialPath);
		if (fileVersion < MaterialFileVersion)
		{
			SaveMaterialFile(MaterialPath, material);
		}
		else
		{
			ParseMaterialFile(MaterialPath, material);
		}
	}
	else if (bCreateIfMissing)
	{
		SaveMaterialFile(MaterialPath, material);
	}

	if (material.Name.empty())
	{
		material.Name = MaterialPath.stem().string();
	}

	material.LastWriteTime = std::filesystem::last_write_time(MaterialPath, ec);
	EnsureBaseColorTexture(material);
	return memory::NewShared<SMaterial>(std::move(material));
}

bool CMaterialLibrary::ParseMaterialFile (const std::filesystem::path& MaterialPath, SMaterial& Material) const
{
	std::ifstream stream(MaterialPath);
	if (!stream.is_open())
	{
		return false;
	}

	std::string line;
	while (std::getline(stream, line))
	{
		std::string trimmed = TrimCopy(line);
		if (trimmed.empty())
		{
			continue;
		}

		if (trimmed.rfind("#", 0) == 0u || trimmed.rfind("//", 0) == 0u)
		{
			continue;
		}

		size_t split = trimmed.find(':');
		if (split == std::string::npos)
		{
			split = trimmed.find('=');
		}

		if (split == std::string::npos)
		{
			continue;
		}

		std::string key = TrimCopy(trimmed.substr(0, split));
		std::string value = TrimCopy(trimmed.substr(split + 1));
		value = StripQuotes(value);

		if (key == "name")
		{
			Material.Name = value;
		}
		else if (key == "version")
		{
			continue;
		}
		else if (key == "vertex_shader")
		{
			Material.VertexShaderName = value;
		}
		else if (key == "pixel_shader")
		{
			Material.PixelShaderName = value;
		}
		else if (key == "base_color_texture")
		{
			Material.BaseColorTexturePath = value;
		}
		else if (key == "cull")
		{
			(void)ParseCullMode(value, &Material.CullMode);
		}
		else if (key == "depth_enable")
		{
			(void)ParseBool(value, &Material.bDepthEnable);
		}
		else if (key == "depth_write")
		{
			(void)ParseBool(value, &Material.bDepthWrite);
		}
		else if (key == "depth_func")
		{
			(void)ParseDepthFunc(value, &Material.DepthFunc);
		}
		else if (key == "alpha_blend")
		{
			(void)ParseBool(value, &Material.bAlphaBlend);
		}
	}

	return true;
}

bool CMaterialLibrary::SaveMaterialFile (const std::filesystem::path& MaterialPath, const SMaterial& Material) const
{
	std::error_code ec;
	std::filesystem::create_directories(MaterialPath.parent_path(), ec);

	std::ofstream stream(MaterialPath);
	if (!stream.is_open())
	{
		return false;
	}

	stream << "version: " << MaterialFileVersion << "\n";
	stream << "# FRT material\n";
	stream << "name: " << Material.Name << "\n";
	stream << "vertex_shader: " << Material.VertexShaderName << "\n";
	stream << "pixel_shader: " << Material.PixelShaderName << "\n";
	stream << "base_color_texture: " << Material.BaseColorTexturePath << "\n";
	stream << "cull: " << CullModeToString(Material.CullMode) << "\n";
	stream << "depth_enable: " << (Material.bDepthEnable ? "true" : "false") << "\n";
	stream << "depth_write: " << (Material.bDepthWrite ? "true" : "false") << "\n";
	stream << "depth_func: " << DepthFuncToString(Material.DepthFunc) << "\n";
	stream << "alpha_blend: " << (Material.bAlphaBlend ? "true" : "false") << "\n";
	return true;
}

void CMaterialLibrary::EnsureBaseColorTexture (SMaterial& Material) const
{
	if (!Renderer)
	{
		return;
	}

	if (Material.BaseColorTexturePath.empty())
	{
		Material.bHasBaseColorTexture = false;
		Material.LoadedBaseColorTexturePath.clear();
		return;
	}

	std::filesystem::path texturePath = Material.BaseColorTexturePath;
	if (texturePath.is_relative() && !Material.SourcePath.empty())
	{
		texturePath = Material.SourcePath.parent_path() / texturePath;
	}
	texturePath = texturePath.lexically_normal();

	if (Material.bHasBaseColorTexture && Material.LoadedBaseColorTexturePath == texturePath)
	{
		return;
	}

	int32 channelNum = 0;
	int32 width = 0;
	int32 height = 0;
	uint32* texels = (uint32*)stbi_load(texturePath.string().c_str(), &width, &height, &channelNum, 4);
	if (!texels)
	{
		return;
	}

	STexture newTexture = {};
	newTexture.Width = width;
	newTexture.Height = height;
	newTexture.Texels = texels;

#if !defined(FRT_HEADLESS)
	{
		D3D12_RESOURCE_DESC Desc = {};
		Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		Desc.Width = newTexture.Width;
		Desc.Height = newTexture.Height;
		Desc.DepthOrArraySize = 1;
		Desc.MipLevels = 1;
		Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		Desc.SampleDesc.Count = 1;
		Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		newTexture.GpuTexture = Renderer->CreateTextureAsset(
			Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, newTexture.Texels);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = Desc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.PlaneSlice = 0;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
		Renderer->CreateShaderResourceView(newTexture.GpuTexture, srvDesc, &cpuDescriptor, &newTexture.GpuDescriptor);
	}
#endif

	Material.BaseColorTexture = newTexture;
	Material.bHasBaseColorTexture = true;
	Material.LoadedBaseColorTexturePath = texturePath;
}

std::string CMaterialLibrary::MakeKey (const std::filesystem::path& MaterialPath)
{
	return MaterialPath.lexically_normal().string();
}
}
