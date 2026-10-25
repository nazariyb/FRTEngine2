#include "Assets/AssetTypes.h"

#include <iterator>


namespace frt::assets
{
namespace
{
constexpr SStaticTagKey TextureKeys[] = {
	{ "Width", ETagType::Int64, ETagFlags::None },
	{ "Height", ETagType::Int64, ETagFlags::None },
	{ "Channels", ETagType::Int64, ETagFlags::None },
	{ "HasAlpha", ETagType::Bool, ETagFlags::None },
};

constexpr SStaticTagKey ModelKeys[] = {
	{ "MeshCount", ETagType::Int64, ETagFlags::None },
	{ "MaterialCount", ETagType::Int64, ETagFlags::None },
	{ "ImageCount", ETagType::Int64, ETagFlags::None },
	{ "VertexCount", ETagType::Int64, ETagFlags::None },
	{ "TriangleCount", ETagType::Int64, ETagFlags::None },
};

constexpr SStaticTagKey MeshKeys[] = {
	{ "VertexCount", ETagType::Int64, ETagFlags::None },
	{ "TriangleCount", ETagType::Int64, ETagFlags::None },
};

constexpr SStaticTagKey InputActionMapKeys[] = {
	{ "Actions", ETagType::Name, ETagFlags::MultiValue },
};

constexpr SStaticTagKey InputActionKeys[] = {
	{ "ValueType", ETagType::Name, ETagFlags::None },
};

// Indexed by TypeId: the order is the frozen numbering of asset_types.
constexpr SAssetTypeInfo Types[] = {
	{ "frt.Unknown", "Unknown", {} },
	{ "frt.Raw", "Raw", {} },
	{ "frt.Texture", "Texture", TextureKeys },
	{ "frt.Model", "Model", ModelKeys },
	{ "frt.Mesh", "Mesh", MeshKeys },
	{ "frt.Material", "Material", {} },
	{ "frt.InputActionMap", "InputActionMap", InputActionMapKeys },
	{ "frt.InputAction", "InputAction", InputActionKeys },
};

static_assert(std::size(Types) == asset_types::Count);
}


std::span<const SAssetTypeInfo> CAssetTypeRegistry::GetTypes () noexcept
{
	return Types;
}

const SAssetTypeInfo& CAssetTypeRegistry::Get (reflect::TypeId Type) noexcept
{
	return Type < std::size(Types) ? Types[Type] : Types[asset_types::Unknown];
}

std::optional<reflect::TypeId> CAssetTypeRegistry::Find (FStringView Name) noexcept
{
	const SStringEqualIgnoreCase equal;
	for (uint32 i = 0u; i < std::size(Types); ++i)
	{
		if (equal(Name, Types[i].Name))
		{
			return static_cast<reflect::TypeId>(i);
		}
	}

	std::optional<reflect::TypeId> found;
	for (uint32 i = 0u; i < std::size(Types); ++i)
	{
		if (equal(Name, Types[i].ShortName))
		{
			if (found)
			{
				return std::nullopt;
			}
			found = static_cast<reflect::TypeId>(i);
		}
	}
	return found;
}

std::optional<uint32> CAssetTypeRegistry::FindKey (reflect::TypeId Type, FStringView KeyName) noexcept
{
	const SStringEqualIgnoreCase equal;
	const std::span<const SStaticTagKey> keys = Get(Type).Keys;
	for (uint32 i = 0u; i < keys.size(); ++i)
	{
		if (equal(KeyName, keys[i].Name))
		{
			return i;
		}
	}
	return std::nullopt;
}
}
