#include "Graphics/Render/Serializers/MaterialSerializers.h"

#include <optional>

#include "Assets/TextAssetIO.h"
#include "Assets/Yaml/YamlArchive.h"
#include "Containers/String.h"
#include "Graphics/Render/Serializers/MaterialSerialize.h"
#include "Platform/Platform.h"


namespace frt::graphics
{
// ---------------- CMaterialYamlSerializer ----------------

bool CMaterialYamlSerializer::CanRead (const std::filesystem::path& Path) const
{
	// v2 reads as a subset of v3 (see Serialize(IReadArchive&, SMaterial&)).
	const int32 version = assets::text::GetTextAssetVersion(Path);
	return version == MaterialAssetVersion || version == 2;
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

static bool ParseLegacyCullMode (FStringView Input, D3D12_CULL_MODE& Out)
{
	const auto matches = [Input] (FStringView Name) { return Input.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
	if (matches("none"))  { Out = D3D12_CULL_MODE_NONE;  return true; }
	if (matches("front")) { Out = D3D12_CULL_MODE_FRONT; return true; }
	if (matches("back"))  { Out = D3D12_CULL_MODE_BACK;  return true; }
	return false;
}

static bool ParseLegacyDepthFunc (FStringView Input, D3D12_COMPARISON_FUNC& Out)
{
	const auto matches = [Input] (FStringView Name) { return Input.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
	if (matches("never"))                              { Out = D3D12_COMPARISON_FUNC_NEVER;         return true; }
	if (matches("less"))                               { Out = D3D12_COMPARISON_FUNC_LESS;          return true; }
	if (matches("equal"))                              { Out = D3D12_COMPARISON_FUNC_EQUAL;         return true; }
	if (matches("less_equal") || matches("lequal"))    { Out = D3D12_COMPARISON_FUNC_LESS_EQUAL;    return true; }
	if (matches("greater"))                            { Out = D3D12_COMPARISON_FUNC_GREATER;       return true; }
	if (matches("not_equal") || matches("notequal"))   { Out = D3D12_COMPARISON_FUNC_NOT_EQUAL;     return true; }
	if (matches("greater_equal") || matches("gequal")) { Out = D3D12_COMPARISON_FUNC_GREATER_EQUAL; return true; }
	if (matches("always"))                             { Out = D3D12_COMPARISON_FUNC_ALWAYS;        return true; }
	return false;
}

// A value that is no number (strict: "0.5x" is none) leaves the field as it was.
static void ParseLegacyFloat (FStringView Input, float& Out)
{
	if (const std::optional<float> value = Input.ToFloat())
	{
		Out = *value;
	}
}

bool CMaterialLegacyTextSerializer::CanRead (const std::filesystem::path& Path) const
{
	return assets::text::GetTextAssetVersion(Path) == 1;
}

bool CMaterialLegacyTextSerializer::Read (const std::filesystem::path& Path, SMaterial& Out) const
{
	// Decoded as CanRead's GetTextAssetVersion decodes it. Read as bytes, a UTF-16 file that passed
	// that check would match no key, and CMaterialLibrary's upgrade would write the defaults over it.
	const std::optional<FString> text = platform::ReadFileText(Path);
	if (!text)
	{
		return false;
	}

	for (const FStringView line : text->Lines())
	{
		FStringView key;
		FStringView value;
		if (!assets::text::TryParseKeyValue(line, &key, &value))
		{
			continue;
		}

		if (key == "version" || key.IsEmpty()) continue;

		// CanRead's probe skips a comment after the version, so the values skip theirs too: one read as
		// no number would be lost, since CMaterialLibrary upgrades the file with what was read. Names
		// and paths are taken as written, as they always were.
		const FStringView setting = assets::text::StripTrailingComment(value);

		if (key == "name")               Out.Name = value;
		else if (key == "vertex_shader") Out.VertexShaderName = value;
		else if (key == "pixel_shader")  Out.PixelShaderName = value;
		else if (key == "color")         Out.DiffuseAlbedo.FillFromString(setting);
		else if (key == "metallic")      ParseLegacyFloat(setting, Out.Metallic);
		else if (key == "roughness")     ParseLegacyFloat(setting, Out.Roughness);
		else if (key == "emissive")      Out.Emissive.FillFromString(setting);
		else if (key == "emissive_intensity") ParseLegacyFloat(setting, Out.EmissiveIntensity);
		else if (key == "base_color_texture") Out.BaseColorTexturePath = value;
		else if (key == "cull")          (void)ParseLegacyCullMode(setting, Out.CullMode);
		else if (key == "depth_enable")  Out.bDepthEnable = setting.TryParseBool().value_or(Out.bDepthEnable);
		else if (key == "depth_write")   Out.bDepthWrite = setting.TryParseBool().value_or(Out.bDepthWrite);
		else if (key == "depth_func")    (void)ParseLegacyDepthFunc(setting, Out.DepthFunc);
		else if (key == "alpha_blend")   Out.bAlphaBlend = setting.TryParseBool().value_or(Out.bAlphaBlend);
	}

	return true;
}

bool CMaterialLegacyTextSerializer::Write (const std::filesystem::path& /*Path*/, const SMaterial& /*In*/) const
{
	// Legacy v1 is read-only. New saves always go through the default write format (YAML).
	return false;
}
}
