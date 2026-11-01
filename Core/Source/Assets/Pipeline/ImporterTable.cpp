#include "Assets/Pipeline/ImporterTable.h"

#include <algorithm>
#include <iterator>

#include "Assets/AssetTypes.h"
#include "Hash/StableHash.h"


namespace frt::assets
{
namespace
{
// Reaching it during constant evaluation is the compile error: an AnalyzeVersion below that is not 32 hex
// digits, or is all zero.
inline void InvalidAnalyzeVersion () noexcept {}

consteval uint8 HexDigitValue (char C) noexcept
{
	if (C >= '0' && C <= '9')
	{
		return static_cast<uint8>(C - '0');
	}
	if (C >= 'a' && C <= 'f')
	{
		return static_cast<uint8>(C - 'a' + 10);
	}
	InvalidAnalyzeVersion();
	return 0u;
}

// 32 lowercase hex digits, as `FRT.bat assets newguid` prints them.
consteval SGuid VersionGuid (FStringLiteral Text) noexcept
{
	SGuid guid;
	if (Text.Len() != SGuid::TextLength)
	{
		InvalidAnalyzeVersion();
		return guid;
	}
	for (uint32 i = 0u; i < SGuid::Size; ++i)
	{
		guid.Bytes[i] = static_cast<uint8>(HexDigitValue(Text.GetData()[2u * i]) << 4u | HexDigitValue(Text.GetData()[2u * i + 1u]));
	}
	if (!guid.IsValid())
	{
		InvalidAnalyzeVersion();
	}
	return guid;
}


// ---- Settings tables ----

constexpr FStringLiteral ColorSpaceValues[] = { "Srgb", "Linear", "Normal" };
constexpr FStringLiteral MipsValues[] = { "Box", "None" };
constexpr FStringLiteral CompressionValues[] = { "None" };

constexpr SSettingsField TextureFields[] = {
	{ "ColorSpace", ETagType::Enum, "Srgb", ColorSpaceValues, false },
	{ "Mips", ETagType::Enum, "Box", MipsValues, false },
	{ "Compression", ETagType::Enum, "None", CompressionValues, false },
	{ "MaxSize", ETagType::Int64, "0", {}, false },
};

constexpr FStringLiteral HierarchyValues[] = { "Flatten", "Preserve" };
constexpr FStringLiteral TangentsValues[] = { "Generate", "Import" };

constexpr SSettingsField GltfFields[] = {
	{ "Hierarchy", ETagType::Enum, "Flatten", HierarchyValues, false },
	{ "Tangents", ETagType::Enum, "Generate", TangentsValues, false },
	{ "ScaleFactor", ETagType::Double, "1", {}, false },
};

constexpr SSettingsTable TextureSettings { "Texture", TextureFields };
constexpr SSettingsTable GltfSettings { "Model.Gltf", GltfFields };


// ---- Importers ----

constexpr FStringLiteral TextureExtensions[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
constexpr FStringLiteral GltfExtensions[] = { ".gltf", ".glb" };
constexpr FStringLiteral MaterialLegacyExtensions[] = { ".frtmat.yml" };
constexpr FStringLiteral InputLegacyExtensions[] = { ".frtinputmap", ".frtinputaction" };
constexpr FStringLiteral DocumentExtensions[] = { ".frt" };
constexpr FStringLiteral PsdExtensions[] = { ".psd" };
constexpr FStringLiteral KritaExtensions[] = { ".kra", ".ora" };
constexpr FStringLiteral BlenderExtensions[] = { ".blend" };

// Available ones first (§7.11, M3), then the v1 importers that later milestones implement (Document M5,
// Texture.Psd / Texture.Krita / Model.Blender M9c, DataTable.Csv M6). DataTable.Csv claims nothing: CSV is
// a table only where a rule says so (§3.5.3). Document's main type comes from the document's $type (M5).
constexpr SImporterDesc Importers[] = {
	{ "Texture", TextureExtensions, VersionGuid("8615e3393e8f45ea9aa1ba548b0997ef"), asset_types::Texture, &TextureSettings, true },
	{ "Model.Gltf", GltfExtensions, VersionGuid("2e2fa2692148404ebbc0546488623604"), asset_types::Model, &GltfSettings, true },
	{ "Material.Legacy", MaterialLegacyExtensions, VersionGuid("cd98a4dbb01140988fb79078522d0abf"), asset_types::Material, nullptr, true },
	{ "Input.Legacy", InputLegacyExtensions, VersionGuid("35d327fabae74e13bb20019f0b971488"), asset_types::InputActionMap, nullptr, true },
	{ "Raw", {}, VersionGuid("6c57da72c3fa4affbf0b88647adac4ca"), asset_types::Raw, nullptr, true },
	{ "Document", DocumentExtensions, VersionGuid("1037bf1a8a2b4ff190b2eb18c85771aa"), asset_types::Unknown, nullptr, false },
	{ "Texture.Psd", PsdExtensions, VersionGuid("d4a7a5ac819d4b1792a7dae803f84c90"), asset_types::Texture, &TextureSettings, false },
	{ "Texture.Krita", KritaExtensions, VersionGuid("2cd46a83a72b4462ad99763fb8c220dc"), asset_types::Texture, &TextureSettings, false },
	{ "Model.Blender", BlenderExtensions, VersionGuid("9f5bf486b32142f5a4f2dcad1ea48340"), asset_types::Model, &GltfSettings, false },
	{ "DataTable.Csv", {}, VersionGuid("cbff091755524826be72a9e240dcb32c"), asset_types::Unknown, nullptr, false },
};

// Extensions whose main type differs from their importer's MainType.
struct SExtensionType
{
	FStringLiteral Extension;
	reflect::TypeId Type;
};

constexpr SExtensionType ExtensionTypes[] = {
	{ ".frtinputmap", asset_types::InputActionMap },
	{ ".frtinputaction", asset_types::InputAction },
};

// The longest of Extensions that FileName ends with (portable fold), with something before it; or -1.
int32 FindClaimedExtension (std::span<const FStringLiteral> Extensions, FStringView FileName) noexcept
{
	int32 best = -1;
	for (uint32 i = 0u; i < Extensions.size(); ++i)
	{
		FStringView stem = FileName;
		if (stem.RemoveFromEnd<ESearchCase::IgnoreCase>(Extensions[i]) && !stem.IsEmpty() && !stem.EndsWith('/')
			&& (best < 0 || Extensions[i].Len() > Extensions[static_cast<uint32>(best)].Len()))
		{
			best = static_cast<int32>(i);
		}
	}
	return best;
}
}


reflect::TypeId SImporterDesc::GetMainType (FStringView FileName) const noexcept
{
	const int32 extension = FindClaimedExtension(Extensions, FileName);
	if (extension >= 0)
	{
		const SStringEqualIgnoreCase equal;
		for (const SExtensionType& entry : ExtensionTypes)
		{
			if (equal(entry.Extension, Extensions[static_cast<uint32>(extension)]))
			{
				return entry.Type;
			}
		}
	}
	return MainType;
}

std::span<const SImporterDesc> GetImporters () noexcept
{
	return Importers;
}

const SImporterDesc* FindImporter (FStringView Name) noexcept
{
	const SStringEqualIgnoreCase equal;
	for (const SImporterDesc& importer : Importers)
	{
		if (equal(Name, importer.Name))
		{
			return &importer;
		}
	}
	return nullptr;
}

const SImporterDesc* FindImporterForFile (FStringView FileName) noexcept
{
	const SImporterDesc* best = nullptr;
	uint32 bestLength = 0u;
	for (const SImporterDesc& importer : Importers)
	{
		if (!importer.bAvailable)
		{
			continue;
		}
		const int32 extension = FindClaimedExtension(importer.Extensions, FileName);
		if (extension >= 0 && importer.Extensions[static_cast<uint32>(extension)].Len() > bestLength)
		{
			best = &importer;
			bestLength = importer.Extensions[static_cast<uint32>(extension)].Len();
		}
	}
	return best;
}

FStringView GetAssetStem (FStringView LeafName) noexcept
{
	uint32 bestLength = 0u;
	for (const SImporterDesc& importer : Importers)
	{
		const int32 extension = FindClaimedExtension(importer.Extensions, LeafName);
		if (extension >= 0)
		{
			bestLength = (std::max)(bestLength, importer.Extensions[static_cast<uint32>(extension)].Len());
		}
	}
	if (bestLength > 0u)
	{
		return LeafName.LeftChop(bestLength);
	}
	const uint32 dot = LeafName.FindLast('.');
	return dot == FStringView::NotFound || dot == 0u ? LeafName : LeafName.Left(dot);
}

int32 FindSettingsField (const SSettingsTable& Table, FStringView Name) noexcept
{
	const SStringEqualIgnoreCase equal;
	for (uint32 i = 0u; i < Table.Fields.size(); ++i)
	{
		if (equal(Name, Table.Fields[i].Name))
		{
			return static_cast<int32>(i);
		}
	}
	return -1;
}

uint64 MakeAnalyzeStamp (const SGuid& AnalyzeVersion, uint64 AnalyzeHash) noexcept
{
	uint8 bytes[SGuid::Size + 8u] = {};
	for (uint32 i = 0u; i < SGuid::Size; ++i)
	{
		bytes[i] = AnalyzeVersion.Bytes[i];
	}
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[SGuid::Size + i] = static_cast<uint8>(AnalyzeHash >> (8u * i));
	}
	return hash::Xxh3_64(bytes);
}
}
