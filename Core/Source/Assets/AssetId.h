#pragma once

#include <compare>
#include <optional>
#include <span>
#include <type_traits>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * Asset identity (design: Local/Design/AssetManager, sections 4.2, 4.3 and 4.5).
 *
 * Three layers, as every studio in the research separates them (Halo's datum index + salt, Destiny's
 * TagHash and TagHash64):
 *	- SAssetId { SGuid, SubId }: persistent, in text files, the registry and build inputs.
 *	- SAssetId64: a 64-bit hash of it for cooked data, saves and network messages.
 *	- HAsset (later): a live slot, never persisted.
 *
 * The GUID is 128 bits, version 4, not Godot's 63 or Hazel's 64: real duplicates come from copied
 * files, not from generation, and 128 bits removes the collision worry across projects and DLC. Not
 * version 7 either: it leaks creation time, has fewer random bits, and its ordering locality buys
 * nothing for a hash table plus a sorted snapshot.
 *
 * Every derivation here is frozen: the values are written into metas, cooked data and saves, and
 * Core-Test pins them. Changing one is a format change with a new context string ("... v2").
 */
namespace frt::assets
{
/**
 * Persistent identity of a main asset: 16 bytes in RFC 9562 order, compared as memcmp.
 *
 * Text is 32 lowercase hex digits in byte order (9fc0d4010bbf28b4594072e72b8655ab): one greppable
 * token. Parse also takes the dashed and braced spellings other tools write, in either case.
 *
 * Generated GUIDs whose text is all decimal digits, or digits and a single 'e', are re-rolled
 * (HasNumericText, p about 1.5e-6), so no YAML reader types an id as a number. All digits is an
 * integer under the YAML 1.2 core schema, and under YAML 1.1 unless it starts with 0 (then octal if
 * every digit is 0-7). Digits around one 'e' ("477e817") are a float under the YAML 1.2 core schema
 * (ruamel.yaml and others; the JSON schema too without a leading 0); YAML 1.1 floats need a '.' and a
 * signed exponent, so PyYAML keeps them strings. A leading or trailing 'e' is a number in no schema,
 * but the rule re-rolls it too: it over-approximates on purpose to stay simple, and it is frozen into
 * MakeAutoGuid's outputs.
 */
struct SGuid
{
	static constexpr uint32 Size = 16u;
	static constexpr uint32 TextLength = 2u * Size;

	uint8 Bytes[Size] = {};

	// UUIDv4 from the OS generator (platform::FillRandomBytes), re-rolled while HasNumericText. For
	// assets a person creates in the editor (Create, Duplicate, Import); metas a tool writes on its own
	// use MakeAutoGuid, which every machine reproduces.
	[[nodiscard]] FRT_CORE_API static SGuid NewRandom () noexcept;

	// 32 hex digits; or 8-4-4-4-12 with dashes; either of those in braces; any case. Nothing else:
	// no spaces, no "urn:uuid:". The all-zero GUID parses (and is not IsValid).
	[[nodiscard]] FRT_CORE_API static std::optional<SGuid> Parse (FStringView Text) noexcept;

	// 32 lowercase hex digits, no dashes.
	FRT_CORE_API void AppendText (FString& Out) const noexcept;
	[[nodiscard]] FRT_CORE_API FString ToString () const noexcept;

	// The text is all decimal digits, or digits and exactly one 'e'.
	[[nodiscard]] FRT_CORE_API bool HasNumericText () const noexcept;

	// Not all zero.
	[[nodiscard]] constexpr bool IsValid () const noexcept
	{
		for (const uint8 byte : Bytes)
		{
			if (byte != 0u)
			{
				return true;
			}
		}
		return false;
	}

	// RFC 9562 version (4 random, 8 derived) and whether the variant bits are RFC 9562's (10xx).
	[[nodiscard]] constexpr uint32 GetVersion () const noexcept { return Bytes[6] >> 4u; }
	[[nodiscard]] constexpr bool HasRfcVariant () const noexcept { return (Bytes[8] & 0xC0u) == 0x80u; }

	constexpr auto operator<=> (const SGuid&) const = default;
};


/**
 * Deterministic UUIDv8 for identities nobody commits: transient assets, per-target variants, the
 * registry's provisional ids. A pure function of its inputs on every machine:
 *
 *   BLAKE3 derive_key("FRT derived GUID v1",
 *       Parent.Bytes || le64(len Generator) || Generator || le64(len Label) || Label)
 *
 * first 16 bytes, version nibble 8 and variant 10. Lengths are byte counts of the UTF-8 text; they
 * keep ("ab", "c") and ("a", "bc") apart. Never re-rolled (the result is never written as text a
 * YAML reader types).
 */
[[nodiscard]] FRT_CORE_API SGuid MakeDerivedGuid (const SGuid& Parent, FStringView Generator, FStringView Label) noexcept;

/**
 * Reproducible identity for metas a tool creates on its own (the meta lifecycle's CreateMeta,
 * `migrate metas`, the losers of a duplicate-GUID dedupe), so two machines given the same commit write
 * the same bytes. Candidate number Attempt is
 *
 *   BLAKE3 derive_key("FRT auto GUID v1",
 *       le64(len Salt) || Salt || le64(len Generator) || Generator || le64(len Input) || Input
 *       || le32(Attempt) if Attempt > 0)
 *
 * first 16 bytes, version nibble 4 and variant 10. MakeAutoGuid returns the first candidate whose text
 * is not numeric and, with a predicate, that IsTaken (a live GUID) does not claim. Salt is the
 * project's ([Project] AssetSalt, else its Name; "FRT.Engine" for the engine); Input is what the
 * caller derives from (for an automatic meta: the case-folded mount path, then the content's BLAKE3).
 */
[[nodiscard]] FRT_CORE_API SGuid MakeAutoGuidCandidate (FStringView Salt, FStringView Generator, std::span<const uint8> Input,
	uint32 Attempt) noexcept;

[[nodiscard]] FRT_CORE_API SGuid MakeAutoGuid (FStringView Salt, FStringView Generator, std::span<const uint8> Input) noexcept;

// IsTaken(const SGuid&) -> bool: true re-rolls (a collision with a live GUID).
template <typename TIsTaken>
	requires std::is_invocable_r_v<bool, TIsTaken&, const SGuid&>
[[nodiscard]] SGuid MakeAutoGuid (FStringView Salt, FStringView Generator, std::span<const uint8> Input, TIsTaken&& IsTaken)
{
	for (uint32 attempt = 0u;; ++attempt)
	{
		const SGuid guid = MakeAutoGuidCandidate(Salt, Generator, Input, attempt);
		if (!guid.HasNumericText() && !IsTaken(guid))
		{
			return guid;
		}
	}
}


// A main asset (SubId 0) or one of its sub-assets: an imported object (MakeSubId of its label), a table
// row, a native object with a stored id.
struct SAssetId
{
	SGuid Guid;
	uint64 SubId = 0u;

	[[nodiscard]] constexpr bool IsValid () const noexcept { return Guid.IsValid(); }
	[[nodiscard]] constexpr bool IsMainAsset () const noexcept { return SubId == 0u; }

	constexpr auto operator<=> (const SAssetId&) const = default;
};

/**
 * The 64-bit form of an SAssetId, for cooked data, saves and network messages:
 *
 *   XXH3_64bits_withSeed(Guid.Bytes || le64(SubId), 24, 0x5445535341545246 ("FRTASSET" read as le64))
 *
 * with 0 reserved (remapped to 1), so a zero-initialized field means "none". The set of ids is closed:
 * the editor registry checks collisions on insert, the cook fails on one and a pak mount refuses one.
 */
struct SAssetId64
{
	uint64 Value = 0u;

	[[nodiscard]] constexpr bool IsValid () const noexcept { return Value != 0u; }

	constexpr auto operator<=> (const SAssetId64&) const = default;
};

inline constexpr uint64 AssetId64Seed = 0x5445535341545246ull;

[[nodiscard]] FRT_CORE_API SAssetId64 MakeAssetId64 (const SAssetId& Id) noexcept;

// The SubId of an imported object or a table row: XXH3_64bits of its label ("mesh:Body", "row:Rifle";
// kind, colon, name, from the importer's vocabulary, never a C++ type name), 0 remapped to 1. A pure
// function of the label, so a reimport or a reordered DCC file keeps every id. Two labels of one
// source with the same id are an import error.
[[nodiscard]] FRT_CORE_API uint64 MakeSubId (FStringView Label) noexcept;
}
