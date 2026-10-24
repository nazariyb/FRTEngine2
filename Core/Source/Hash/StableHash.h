#pragma once

#include <compare>
#include <optional>
#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * Hashes that may be written to disk: their output is part of a file format and never changes.
 *
 * hash::HashBytes (wyhash, Containers/StringView.h) stays the in-memory hash of maps and strings; it
 * may change between versions and must never be persisted. Everything here is frozen instead:
 *
 *	- XXH3 (64-bit), xxHash's output frozen since 0.8.0: fast 64-bit ids that live in cooked data,
 *	  saves and network messages (SAssetId64, sub-asset and row ids; Assets/AssetId.h) and trailing
 *	  checksums. A 64-bit value is an id inside a closed set whose collisions are checked, never a
 *	  content address (Bitsquid and Naughty Dog treat 64-bit id collisions as build errors).
 *	- BLAKE3, truncated to 160 bits (SContentHash): content addresses and cache keys, where a
 *	  collision would silently serve wrong bytes, so the hash must be cryptographic. 160 bits is Git's
 *	  object-id width; BLAKE3's output is an XOF, so the first 20 bytes are exactly a 20-byte BLAKE3
 *	  and any prefix is as strong as its length (BLAKE3 paper, section 2.6).
 *	- BLAKE3's derive_key mode for derivations that must never collide with plain content hashes of
 *	  the same bytes: the context string ("FRT DDC static v1") domain-separates each use, so a GUID
 *	  derived from some bytes never equals a cache key or a content hash of the same bytes (BLAKE3
 *	  paper, section 6.2: the context is hard-coded, globally unique and application-specific).
 *
 * The libraries are vendored (ThirdParty/xxHash, ThirdParty/BLAKE3) and seen only by StableHash.cpp:
 * the state of CContentHasher is opaque storage here. Every function is thread-safe; a CContentHasher
 * is a value used by one thread at a time. Nothing here allocates.
 */
namespace frt::hash
{
// Text goes into the functions taking a byte span by its UTF-8 bytes: Text.AsBytes().

// ---------------- XXH3 ----------------

// XXH3_64bits_withSeed: identical on every platform and xxHash version since 0.8.0.
[[nodiscard]] FRT_CORE_API uint64 Xxh3_64 (std::span<const uint8> Bytes, uint64 Seed = 0) noexcept;
// The text's UTF-8 bytes; no terminator, no normalization.
[[nodiscard]] FRT_CORE_API uint64 Xxh3_64 (FStringView Text, uint64 Seed = 0) noexcept;


// ---------------- BLAKE3-160 ----------------

// The first 20 bytes of a BLAKE3 output (hash or derive_key mode). Compares as memcmp. Text: 40
// lowercase hex digits in byte order, one greppable token like a Git object id.
struct SContentHash
{
	static constexpr uint32 Size = 20u;
	static constexpr uint32 TextLength = 2u * Size;

	uint8 Bytes[Size] = {};

	// Exactly 40 hex digits of either case; anything else is nullopt.
	[[nodiscard]] FRT_CORE_API static std::optional<SContentHash> Parse (FStringView Text) noexcept;
	// 40 lowercase hex digits.
	FRT_CORE_API void AppendText (FString& Out) const noexcept;
	[[nodiscard]] FRT_CORE_API FString ToString () const noexcept;

	// Not all zero. No hash function produces all zero in practice, so that marks "no hash yet" - as
	// it marks "none" for every id type (SGuid, SAssetId, SFileIdentity).
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

	[[nodiscard]] std::span<const uint8, Size> AsBytes () const noexcept { return std::span<const uint8, Size>(Bytes); }

	constexpr auto operator<=> (const SContentHash&) const = default;
};


/**
 * Incremental BLAKE3, for content that arrives in pieces (a file read in chunks, a key assembled from
 * fields): Update in any split gives the same output as one call over the concatenation.
 *
 * The default constructor hashes; ForDeriveKey derives a key from the bytes under a context string.
 * Finalize does not consume the state: more Updates may follow, and a copy of the hasher is an
 * independent fork of it. blake3_dispatch.c uses the widest SIMD the CPU has (SSE2 to AVX-512); every
 * path gives the same bytes.
 */
class FRT_CORE_API CContentHasher
{
public:
	CContentHasher () noexcept;

	// derive_key mode. Context is a hard-coded constant naming one use and its version, e.g.
	// "FRT DDC static v1"; never data.
	[[nodiscard]] static CContentHasher ForDeriveKey (FStringView Context) noexcept;

	void Update (std::span<const uint8> Bytes) noexcept;
	void Update (FStringView Text) noexcept;

	// BLAKE3-160 of everything so far.
	[[nodiscard]] SContentHash Finalize () const noexcept;
	// The extendable output: Out.size() bytes of it, any length (32 is BLAKE3-256).
	void FinalizeTo (std::span<uint8> Out) const noexcept;

	// Back to the empty input, in the mode and with the context it was made with.
	void Reset () noexcept;

	// blake3_hasher's size and alignment (StableHash.cpp checks them against blake3.h).
	static constexpr size_t StateSize = 1912u;
	static constexpr size_t StateAlignment = 8u;

private:
	alignas(StateAlignment) uint8 State[StateSize];
};


// BLAKE3-160 of the bytes, in one call.
[[nodiscard]] FRT_CORE_API SContentHash HashContent (std::span<const uint8> Bytes) noexcept;

// derive_key mode: BLAKE3-160 of Material under Context (a hard-coded constant, see ForDeriveKey).
[[nodiscard]] FRT_CORE_API SContentHash DeriveKey (FStringView Context, std::span<const uint8> Material) noexcept;
// The same derivation, Out.size() bytes of output (16 for a GUID).
FRT_CORE_API void DeriveKeyBytes (FStringView Context, std::span<const uint8> Material, std::span<uint8> Out) noexcept;
}
