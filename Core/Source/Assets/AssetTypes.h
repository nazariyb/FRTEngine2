#pragma once

#include <optional>
#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"


/**
 * The asset types the registry knows before reflection exists (design §5.2 "per-type column tables", §15
 * I16): a name, a short name and the tag keys of each type. One immutable static table, so workers (the
 * importers' Analyze) and the store read it without a lock, and every piece agrees on which keys a type
 * has: an importer emits exactly the keys listed here; a new key is added here, never inside a piece.
 *
 * TypeIds are frozen for M3 (no hierarchy yet, so "derived types included" changes nothing). M5's
 * reflection replaces the table and keeps the interface's shape; ids never reach the disk (snapshots
 * store type names).
 */
namespace frt::assets
{
// One tag key of a type's table: the name tags are queried by ("Width"), its value type and flags.
struct SStaticTagKey
{
	FStringLiteral Name;
	ETagType Type;
	ETagFlags Flags;
};

// "frt.Texture", "Texture", its keys in column order.
struct SAssetTypeInfo
{
	FStringLiteral Name;
	FStringLiteral ShortName;
	std::span<const SStaticTagKey> Keys;
};

// The frozen M3 TypeIds, by name (the table's indices).
namespace asset_types
{
inline constexpr reflect::TypeId Unknown = 0u;          // frt.Unknown: no keys
inline constexpr reflect::TypeId Raw = 1u;              // frt.Raw: no keys
inline constexpr reflect::TypeId Texture = 2u;          // frt.Texture: Width, Height, Channels (Int64), HasAlpha (Bool)
inline constexpr reflect::TypeId Model = 3u;            // frt.Model: MeshCount, MaterialCount, ImageCount, VertexCount,
                                                        // TriangleCount (Int64; counts summed over glTF accessors)
inline constexpr reflect::TypeId Mesh = 4u;             // frt.Mesh: VertexCount, TriangleCount (Int64)
inline constexpr reflect::TypeId Material = 5u;         // frt.Material: no keys
inline constexpr reflect::TypeId InputActionMap = 6u;   // frt.InputActionMap: Actions (Name, MultiValue)
inline constexpr reflect::TypeId InputAction = 7u;      // frt.InputAction: ValueType (Name: Button / Axis)
inline constexpr reflect::TypeId Count = 8u;
}

/**
 * The table. Every member is static, thread-safe and allocation-free.
 */
class FRT_CORE_API CAssetTypeRegistry
{
public:
	CAssetTypeRegistry () = delete;

	// Every type, indexed by TypeId.
	[[nodiscard]] static std::span<const SAssetTypeInfo> GetTypes () noexcept;

	// The type with that id; frt.Unknown for an id outside the table (InvalidTypeId included).
	[[nodiscard]] static const SAssetTypeInfo& Get (reflect::TypeId Type) noexcept;

	// By the exact dotted name ("frt.Texture"), else by a short name that names exactly one type ("Texture");
	// both under the portable fold (SStringEqualIgnoreCase). nullopt when nothing or more than one matches.
	[[nodiscard]] static std::optional<reflect::TypeId> Find (FStringView Name) noexcept;
	// By the exact dotted name only, byte for byte ("frt.Texture"), as snapshots, journals and images store a type:
	// a short name or another case that happens to match is not the stored type. nullopt: this build has no such type.
	[[nodiscard]] static std::optional<reflect::TypeId> FindExact (FStringView Name) noexcept;

	// The index of the key in Get(Type).Keys (under the portable fold), nullopt when the type has no such key.
	[[nodiscard]] static std::optional<uint32> FindKey (reflect::TypeId Type, FStringView KeyName) noexcept;
};
}
