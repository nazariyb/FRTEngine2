#pragma once

#include <filesystem>

#include <d3d12.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Containers/String.h"
#include "Graphics/SColor.h"


namespace frt::graphics
{
enum class EMaterialFlags : uint32
{
	None = 0,
	UseBaseColorTexture = 1u << 0u
};
FRT_DECLARE_FLAG_ENUM(EMaterialFlags)

enum class EAlphaMode : uint8
{
	Opaque = 0,
	Mask,      // alpha test against AlphaCutoff
	Blend,     // stochastic transparency in the path tracer, alpha blending in raster preview
};

/**
 * glTF 2.0 metallic-roughness material (plus the engine's raster state).
 *
 * Pure asset data: the GPU side (texture uploads, the material table entry) is owned by the
 * renderer's GPU scene, which watches Version to know when to re-upload. Texture paths are
 * stored as authored (relative to the material file); Resolve*TexturePath gives the absolute
 * path the loader should open.
 */
struct FRT_CORE_API SMaterial
{
	static constexpr uint32 InvalidTextureIndex = 0xFFFFFFFFu;

	FString Name;
	FString VertexShaderName;
	FString PixelShaderName;
	std::filesystem::path SourcePath;
	std::filesystem::file_time_type LastWriteTime;

	// Base colour (linear factor; the texture is sRGB-encoded).
	SColor DiffuseAlbedo = SColor::White;
	FString BaseColorTexturePath;

	// Metallic-roughness (texture: G = roughness, B = metallic, multiplied by the factors).
	float Metallic = 0.0f;
	float Roughness = 1.0f;
	FString MetallicRoughnessTexturePath;

	// Tangent-space normal map.
	FString NormalTexturePath;
	float NormalScale = 1.0f;

	// Ambient occlusion (used by the raster preview only; the path tracer computes visibility).
	FString OcclusionTexturePath;
	float OcclusionStrength = 1.0f;

	// Emission: radiance = Emissive.rgb * EmissiveIntensity * texture (sRGB).
	SColor Emissive = SColor::Black;
	float EmissiveIntensity = 0.0f;
	FString EmissiveTexturePath;

	// Transparency.
	EAlphaMode AlphaMode = EAlphaMode::Opaque;
	float AlphaCutoff = 0.5f;
	bool bDoubleSided = false;

	// KHR_materials_specular / ior / transmission (transmission reserved for the next BSDF).
	float SpecularFactor = 1.0f;
	float Ior = 1.5f;
	float Transmission = 0.0f;

	uint32 RuntimeIndex = 0u;
	SFlags<EMaterialFlags> Flags;

	// Incremented on every (re)load so the GPU scene can tell when to refresh this material.
	uint32 Version = 1u;

	D3D12_CULL_MODE CullMode = D3D12_CULL_MODE_NONE;
	D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	bool bDepthEnable = true;
	bool bDepthWrite = true;
	bool bAlphaBlend = false;

	std::filesystem::path ResolveTexturePath (FStringView RelativePath) const;
	bool IsEmissive () const;
};
}


// Names as material files store them (the `mode` of the `alpha` group).
FRT_DECLARE_ENUM_REFLECTION(
	frt::graphics::EAlphaMode,
	FRT_ENUM_ENTRY_NAME(frt::graphics::EAlphaMode, Opaque, "opaque"),
	FRT_ENUM_ENTRY_NAME(frt::graphics::EAlphaMode, Mask, "mask"),
	FRT_ENUM_ENTRY_NAME(frt::graphics::EAlphaMode, Blend, "blend"));
