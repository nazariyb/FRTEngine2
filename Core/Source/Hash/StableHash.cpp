#include "Hash/StableHash.h"

#include <new>
#include <type_traits>

#include <blake3.h>
#include <xxhash.h>


namespace frt::hash
{
namespace
{
static_assert(sizeof(blake3_hasher) == CContentHasher::StateSize, "CContentHasher::StateSize must match blake3.h");
static_assert(alignof(blake3_hasher) <= CContentHasher::StateAlignment, "CContentHasher::StateAlignment must match blake3.h");
static_assert(std::is_trivially_copyable_v<blake3_hasher>, "a CContentHasher copy is a byte copy of the state");

blake3_hasher* AsHasher (uint8* State) noexcept
{
	return std::launder(reinterpret_cast<blake3_hasher*>(State));
}

const blake3_hasher* AsHasher (const uint8* State) noexcept
{
	return std::launder(reinterpret_cast<const blake3_hasher*>(State));
}
}


uint64 Xxh3_64 (std::span<const uint8> Bytes, uint64 Seed) noexcept
{
	return XXH3_64bits_withSeed(Bytes.data(), Bytes.size(), Seed);
}

uint64 Xxh3_64 (FStringView Text, uint64 Seed) noexcept
{
	return Xxh3_64(Text.AsBytes(), Seed);
}


std::optional<SContentHash> SContentHash::Parse (FStringView Text) noexcept
{
	SContentHash hash;
	if (!Text.DecodeHex(std::span<uint8>(hash.Bytes)))
	{
		return std::nullopt;
	}
	return hash;
}

void SContentHash::AppendText (FString& Out) const noexcept
{
	constexpr char digits[] = "0123456789abcdef";
	char text[TextLength];
	for (uint32 i = 0u; i < Size; ++i)
	{
		text[2u * i] = digits[Bytes[i] >> 4u];
		text[2u * i + 1u] = digits[Bytes[i] & 0x0Fu];
	}
	Out.Append(FStringView(text, TextLength));
}

FString SContentHash::ToString () const noexcept
{
	FString text;
	AppendText(text);
	return text;
}


// The hasher is constructed in place in State: blake3_hasher is a plain C struct, so the storage
// only needs its size and alignment, and copying CContentHasher copies it.
CContentHasher::CContentHasher () noexcept
{
	blake3_hasher_init(new (State) blake3_hasher);
}

CContentHasher CContentHasher::ForDeriveKey (FStringView Context) noexcept
{
	CContentHasher hasher;
	blake3_hasher_init_derive_key_raw(AsHasher(hasher.State), Context.GetData(), Context.Len());
	return hasher;
}

void CContentHasher::Update (std::span<const uint8> Bytes) noexcept
{
	blake3_hasher_update(AsHasher(State), Bytes.data(), Bytes.size());
}

void CContentHasher::Update (FStringView Text) noexcept
{
	Update(Text.AsBytes());
}

SContentHash CContentHasher::Finalize () const noexcept
{
	SContentHash hash;
	FinalizeTo(std::span<uint8>(hash.Bytes));
	return hash;
}

void CContentHasher::FinalizeTo (std::span<uint8> Out) const noexcept
{
	blake3_hasher_finalize(AsHasher(State), Out.data(), Out.size());
}

void CContentHasher::Reset () noexcept
{
	// blake3_hasher_reset keeps the key words, which is what holds the derive_key context's key.
	blake3_hasher_reset(AsHasher(State));
}


SContentHash HashContent (std::span<const uint8> Bytes) noexcept
{
	CContentHasher hasher;
	hasher.Update(Bytes);
	return hasher.Finalize();
}

SContentHash DeriveKey (FStringView Context, std::span<const uint8> Material) noexcept
{
	CContentHasher hasher = CContentHasher::ForDeriveKey(Context);
	hasher.Update(Material);
	return hasher.Finalize();
}

void DeriveKeyBytes (FStringView Context, std::span<const uint8> Material, std::span<uint8> Out) noexcept
{
	CContentHasher hasher = CContentHasher::ForDeriveKey(Context);
	hasher.Update(Material);
	hasher.FinalizeTo(Out);
}
}
