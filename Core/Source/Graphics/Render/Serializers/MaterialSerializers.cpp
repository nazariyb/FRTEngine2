#include "Graphics/Render/Serializers/MaterialSerializers.h"

#include <fstream>

#include "Assets/TextAssetIO.h"
#include "Assets/Yaml/YamlArchive.h"
#include "Graphics/Render/Serializers/MaterialSerialize.h"


namespace frt::graphics
{
// ---------------- CMaterialYamlSerializer ----------------

bool CMaterialYamlSerializer::CanRead (const std::filesystem::path& Path) const
{
	return assets::text::GetTextAssetVersion(Path) == MaterialAssetVersion;
}

bool CMaterialYamlSerializer::Read (const std::filesystem::path& Path, SMaterial& Out) const
{
	assets::yaml::CYamlReadArchive archive;
	if (!archive.LoadFromFile(Path))
	{
		return false;
	}
	return Serialize(archive, Out);
}

bool CMaterialYamlSerializer::Write (const std::filesystem::path& Path, const SMaterial& In) const
{
	assets::yaml::CYamlWriteArchive archive;
	Serialize(archive, In);
	return archive.SaveToFile(Path);
}


// ---------------- CMaterialLegacyTextSerializer ----------------

static bool ParseLegacyCullMode (const std::string& Input, D3D12_CULL_MODE& Out)
{
	const std::string s = assets::text::ToLowerCopy(Input);
	if (s == "none")  { Out = D3D12_CULL_MODE_NONE;  return true; }
	if (s == "front") { Out = D3D12_CULL_MODE_FRONT; return true; }
	if (s == "back")  { Out = D3D12_CULL_MODE_BACK;  return true; }
	return false;
}

static bool ParseLegacyDepthFunc (const std::string& Input, D3D12_COMPARISON_FUNC& Out)
{
	const std::string s = assets::text::ToLowerCopy(Input);
	if (s == "never")                          { Out = D3D12_COMPARISON_FUNC_NEVER;         return true; }
	if (s == "less")                           { Out = D3D12_COMPARISON_FUNC_LESS;          return true; }
	if (s == "equal")                          { Out = D3D12_COMPARISON_FUNC_EQUAL;         return true; }
	if (s == "less_equal" || s == "lequal")    { Out = D3D12_COMPARISON_FUNC_LESS_EQUAL;    return true; }
	if (s == "greater")                        { Out = D3D12_COMPARISON_FUNC_GREATER;       return true; }
	if (s == "not_equal" || s == "notequal")   { Out = D3D12_COMPARISON_FUNC_NOT_EQUAL;     return true; }
	if (s == "greater_equal" || s == "gequal") { Out = D3D12_COMPARISON_FUNC_GREATER_EQUAL; return true; }
	if (s == "always")                         { Out = D3D12_COMPARISON_FUNC_ALWAYS;        return true; }
	return false;
}

bool CMaterialLegacyTextSerializer::CanRead (const std::filesystem::path& Path) const
{
	return assets::text::GetTextAssetVersion(Path) == 1;
}

bool CMaterialLegacyTextSerializer::Read (const std::filesystem::path& Path, SMaterial& Out) const
{
	std::ifstream stream(Path);
	if (!stream.is_open())
	{
		return false;
	}

	std::string line;
	while (std::getline(stream, line))
	{
		std::string key;
		std::string value;
		if (!assets::text::TryParseKeyValue(line, &key, &value))
		{
			continue;
		}

		if (key == "version" || key.empty()) continue;

		if (key == "name")               Out.Name = value;
		else if (key == "vertex_shader") Out.VertexShaderName = value;
		else if (key == "pixel_shader")  Out.PixelShaderName = value;
		else if (key == "color")         Out.DiffuseAlbedo.FillFromString(value);
		else if (key == "metallic")      Out.Metallic = std::stof(value);
		else if (key == "roughness")     Out.Roughness = std::stof(value);
		else if (key == "emissive")      Out.Emissive.FillFromString(value);
		else if (key == "emissive_intensity") Out.EmissiveIntensity = std::stof(value);
		else if (key == "base_color_texture") Out.BaseColorTexturePath = value;
		else if (key == "cull")          (void)ParseLegacyCullMode(value, Out.CullMode);
		else if (key == "depth_enable")  (void)assets::text::ParseBool(value, &Out.bDepthEnable);
		else if (key == "depth_write")   (void)assets::text::ParseBool(value, &Out.bDepthWrite);
		else if (key == "depth_func")    (void)ParseLegacyDepthFunc(value, Out.DepthFunc);
		else if (key == "alpha_blend")   (void)assets::text::ParseBool(value, &Out.bAlphaBlend);
	}

	return true;
}

bool CMaterialLegacyTextSerializer::Write (const std::filesystem::path& /*Path*/, const SMaterial& /*In*/) const
{
	// Legacy v1 is read-only. New saves always go through the default write format (YAML).
	return false;
}
}
