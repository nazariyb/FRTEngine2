#include "Graphics/Render/Serializers/MaterialSerialize.h"

#include "Assets/TextAssetIO.h"
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

static bool ParseCullMode (const std::string& Input, D3D12_CULL_MODE& Out)
{
	const std::string s = assets::text::ToLowerCopy(Input);
	if (s == "none")  { Out = D3D12_CULL_MODE_NONE;  return true; }
	if (s == "front") { Out = D3D12_CULL_MODE_FRONT; return true; }
	if (s == "back")  { Out = D3D12_CULL_MODE_BACK;  return true; }
	return false;
}

static const char* CullModeToString (D3D12_CULL_MODE Mode)
{
	switch (Mode)
	{
		case D3D12_CULL_MODE_NONE:  return "none";
		case D3D12_CULL_MODE_FRONT: return "front";
		case D3D12_CULL_MODE_BACK:  return "back";
		default:                    return "none";
	}
}

static bool ParseDepthFunc (const std::string& Input, D3D12_COMPARISON_FUNC& Out)
{
	const std::string s = assets::text::ToLowerCopy(Input);
	if (s == "never")         { Out = D3D12_COMPARISON_FUNC_NEVER;         return true; }
	if (s == "less")          { Out = D3D12_COMPARISON_FUNC_LESS;          return true; }
	if (s == "equal")         { Out = D3D12_COMPARISON_FUNC_EQUAL;         return true; }
	if (s == "less_equal")    { Out = D3D12_COMPARISON_FUNC_LESS_EQUAL;    return true; }
	if (s == "greater")       { Out = D3D12_COMPARISON_FUNC_GREATER;       return true; }
	if (s == "greater_equal") { Out = D3D12_COMPARISON_FUNC_GREATER_EQUAL; return true; }
	if (s == "always")        { Out = D3D12_COMPARISON_FUNC_ALWAYS;        return true; }
	return false;
}

static const char* DepthFuncToString (D3D12_COMPARISON_FUNC Func)
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
	if (version != MaterialAssetVersion)
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
		Ar.EndGroup();
	}

	if (Ar.BeginGroup("depth"))
	{
		Ar.Read("enable", M.bDepthEnable);
		Ar.Read("write", M.bDepthWrite);

		std::string func;
		if (Ar.Read("func", func))
		{
			(void)ParseDepthFunc(func, M.DepthFunc);
		}
		Ar.EndGroup();
	}

	std::string cull;
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
	Ar.EndGroup();

	Ar.BeginGroup("depth");
		Ar.Write("enable", M.bDepthEnable);
		Ar.Write("write", M.bDepthWrite);
		Ar.Write("func", std::string(DepthFuncToString(M.DepthFunc)));
	Ar.EndGroup();

	Ar.Write("cull", std::string(CullModeToString(M.CullMode)));

	Ar.BeginGroup("blend");
		Ar.Write("enable", M.bAlphaBlend);
	Ar.EndGroup();
}
}
