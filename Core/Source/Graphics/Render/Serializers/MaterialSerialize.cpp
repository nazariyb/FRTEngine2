#include "Graphics/Render/Serializers/MaterialSerialize.h"

#include "Enum.h"
#include "Containers/String.h"
#include "Graphics/Render/Material.h"
#include "Graphics/SColor.h"


namespace frt
{
// SColor lives in the root frt namespace, so its Serialize free functions go here for ADL.

bool Serialize (assets::IReadArchive& Ar, SColor& C)
{
	Ar.Read("r", C.R);
	Ar.Read("g", C.G);
	Ar.Read("b", C.B);
	Ar.Read("a", C.A);
	return true;
}

void Serialize (assets::IWriteArchive& Ar, const SColor& C)
{
	Ar.Write("r", C.R);
	Ar.Write("g", C.G);
	Ar.Write("b", C.B);
	Ar.Write("a", C.A);
}
}


namespace frt::graphics
{
// ---------------- enum ↔ string ----------------

static bool ParseCullMode (FStringView Input, D3D12_CULL_MODE& Out)
{
	const auto matches = [Input] (FStringView Name) { return Input.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
	if (matches("none"))  { Out = D3D12_CULL_MODE_NONE;  return true; }
	if (matches("front")) { Out = D3D12_CULL_MODE_FRONT; return true; }
	if (matches("back"))  { Out = D3D12_CULL_MODE_BACK;  return true; }
	return false;
}

static FStringLiteral CullModeToString (D3D12_CULL_MODE Mode)
{
	switch (Mode)
	{
		case D3D12_CULL_MODE_NONE:  return "none";
		case D3D12_CULL_MODE_FRONT: return "front";
		case D3D12_CULL_MODE_BACK:  return "back";
		default:                    return "none";
	}
}

static bool ParseDepthFunc (FStringView Input, D3D12_COMPARISON_FUNC& Out)
{
	const auto matches = [Input] (FStringView Name) { return Input.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
	if (matches("never"))         { Out = D3D12_COMPARISON_FUNC_NEVER;         return true; }
	if (matches("less"))          { Out = D3D12_COMPARISON_FUNC_LESS;          return true; }
	if (matches("equal"))         { Out = D3D12_COMPARISON_FUNC_EQUAL;         return true; }
	if (matches("less_equal"))    { Out = D3D12_COMPARISON_FUNC_LESS_EQUAL;    return true; }
	if (matches("greater"))       { Out = D3D12_COMPARISON_FUNC_GREATER;       return true; }
	if (matches("greater_equal")) { Out = D3D12_COMPARISON_FUNC_GREATER_EQUAL; return true; }
	if (matches("always"))        { Out = D3D12_COMPARISON_FUNC_ALWAYS;        return true; }
	return false;
}

static FStringLiteral DepthFuncToString (D3D12_COMPARISON_FUNC Func)
{
	switch (Func)
	{
		case D3D12_COMPARISON_FUNC_NEVER:         return "never";
		case D3D12_COMPARISON_FUNC_LESS:          return "less";
		case D3D12_COMPARISON_FUNC_EQUAL:         return "equal";
		case D3D12_COMPARISON_FUNC_LESS_EQUAL:    return "less_equal";
		case D3D12_COMPARISON_FUNC_GREATER:       return "greater";
		case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return "greater_equal";
		case D3D12_COMPARISON_FUNC_ALWAYS:        return "always";
		default:                                  return "less";
	}
}

// ---------------- SMaterial ----------------

bool Serialize (assets::IReadArchive& Ar, SMaterial& M)
{
	int32 version = 0;
	Ar.Read("version", version);
	// v2 is a strict subset of v3: every key it has means the same thing, so it reads as-is and
	// the keys it lacks keep the importer's values.
	if (version != MaterialAssetVersion && version != 2)
	{
		return false;
	}

	Ar.Read("name", M.Name);

	if (Ar.BeginGroup("shaders"))
	{
		Ar.Read("vertex", M.VertexShaderName);
		Ar.Read("pixel", M.PixelShaderName);
		Ar.EndGroup();
	}

	if (Ar.BeginGroup("albedo"))
	{
		assets::Read(Ar, "color", M.DiffuseAlbedo);
		Ar.Read("texture", M.BaseColorTexturePath);
		Ar.EndGroup();
	}

	Ar.Read("metallic", M.Metallic);
	Ar.Read("roughness", M.Roughness);

	if (Ar.BeginGroup("emissive"))
	{
		assets::Read(Ar, "color", M.Emissive);
		Ar.Read("intensity", M.EmissiveIntensity);
		Ar.Read("texture", M.EmissiveTexturePath);
		Ar.EndGroup();
	}

	Ar.Read("metallicRoughnessTexture", M.MetallicRoughnessTexturePath);

	if (Ar.BeginGroup("normal"))
	{
		Ar.Read("texture", M.NormalTexturePath);
		Ar.Read("scale", M.NormalScale);
		Ar.EndGroup();
	}

	if (Ar.BeginGroup("occlusion"))
	{
		Ar.Read("texture", M.OcclusionTexturePath);
		Ar.Read("strength", M.OcclusionStrength);
		Ar.EndGroup();
	}

	if (Ar.BeginGroup("alpha"))
	{
		// EAlphaMode's reflected names are the file's (Material.h); case is ignored, as for the others.
		FString mode;
		if (Ar.Read("mode", mode))
		{
			(void)enum_::TryParse(mode, &M.AlphaMode);
		}
		Ar.Read("cutoff", M.AlphaCutoff);
		Ar.EndGroup();
	}

	Ar.Read("doubleSided", M.bDoubleSided);

	if (Ar.BeginGroup("specular"))
	{
		Ar.Read("factor", M.SpecularFactor);
		Ar.Read("ior", M.Ior);
		Ar.Read("transmission", M.Transmission);
		Ar.EndGroup();
	}

	if (Ar.BeginGroup("depth"))
	{
		Ar.Read("enable", M.bDepthEnable);
		Ar.Read("write", M.bDepthWrite);

		FString func;
		if (Ar.Read("func", func))
		{
			(void)ParseDepthFunc(func, M.DepthFunc);
		}
		Ar.EndGroup();
	}

	FString cull;
	if (Ar.Read("cull", cull))
	{
		(void)ParseCullMode(cull, M.CullMode);
	}

	if (Ar.BeginGroup("blend"))
	{
		Ar.Read("enable", M.bAlphaBlend);
		Ar.EndGroup();
	}

	return true;
}

void Serialize (assets::IWriteArchive& Ar, const SMaterial& M)
{
	Ar.Write("version", MaterialAssetVersion);
	Ar.Write("name", M.Name);

	Ar.BeginGroup("shaders");
		Ar.Write("vertex", M.VertexShaderName);
		Ar.Write("pixel", M.PixelShaderName);
	Ar.EndGroup();

	Ar.BeginGroup("albedo");
		assets::WriteFlow(Ar, "color", M.DiffuseAlbedo);
		Ar.Write("texture", M.BaseColorTexturePath);
	Ar.EndGroup();

	Ar.Write("metallic", M.Metallic);
	Ar.Write("roughness", M.Roughness);

	Ar.BeginGroup("emissive");
		assets::WriteFlow(Ar, "color", M.Emissive);
		Ar.Write("intensity", M.EmissiveIntensity);
		Ar.Write("texture", M.EmissiveTexturePath);
	Ar.EndGroup();

	Ar.Write("metallicRoughnessTexture", M.MetallicRoughnessTexturePath);

	Ar.BeginGroup("normal");
		Ar.Write("texture", M.NormalTexturePath);
		Ar.Write("scale", M.NormalScale);
	Ar.EndGroup();

	Ar.BeginGroup("occlusion");
		Ar.Write("texture", M.OcclusionTexturePath);
		Ar.Write("strength", M.OcclusionStrength);
	Ar.EndGroup();

	Ar.BeginGroup("alpha");
		Ar.Write("mode", enum_::ToString(M.AlphaMode));
		Ar.Write("cutoff", M.AlphaCutoff);
	Ar.EndGroup();

	Ar.Write("doubleSided", M.bDoubleSided);

	Ar.BeginGroup("specular");
		Ar.Write("factor", M.SpecularFactor);
		Ar.Write("ior", M.Ior);
		Ar.Write("transmission", M.Transmission);
	Ar.EndGroup();

	Ar.BeginGroup("depth");
		Ar.Write("enable", M.bDepthEnable);
		Ar.Write("write", M.bDepthWrite);
		Ar.Write("func", DepthFuncToString(M.DepthFunc));
	Ar.EndGroup();

	Ar.Write("cull", CullModeToString(M.CullMode));

	Ar.BeginGroup("blend");
		Ar.Write("enable", M.bAlphaBlend);
	Ar.EndGroup();
}
}
