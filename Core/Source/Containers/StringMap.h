#pragma once

#include <unordered_map>
#include <unordered_set>

#include "Containers/String.h"


/**
 * Hash maps and sets keyed by FString. The functors are transparent, so Map.find(View),
 * Map.find("literal") and contains() do not allocate; insert with Map.try_emplace(FString(View), ...).
 * Hash values are never persisted (they may change between versions).
 */
namespace frt
{
// Path keys: NTFS compares names by upper-casing each UTF-16 code unit through its $UpCase table (BMP only),
// which Windows fills from its own casing table. Neither Unicode case folding nor Unicode's uppercase:
// U+212A KELVIN vs 'k', U+2126 OHM vs 'ω', U+0131 'ı' vs 'I', U+03C2 'ς' vs 'Σ', U+017F 'ſ' vs 'S',
// Georgian Mkhedruli vs Mtavruli and every supplementary-plane cased letter (Deseret, Osage, Adlam, ...)
// are distinct names there. Key per decoded unit: well-formed cp <= FFFF -> the OS table's upper case
// (as CompareStringOrdinal(..., TRUE)); cp > FFFF -> cp; ill-formed bytes as is (WTF-8 surrogates
// included). A volume keeps the table of the Windows that formatted it: letters an older one did not
// case yet can still differ.
struct SPathHash
{
	using is_transparent = void;
	size_t operator() (FStringView S) const noexcept { return static_cast<size_t>(string_impl::HashPath(S)); }
};

struct SPathEqual
{
	using is_transparent = void;
	bool operator() (FStringView A, FStringView B) const noexcept { return string_impl::EqualsPath(A, B); }
};

// Names, byte-exact.
template <typename TValue>
using TStringMap = std::unordered_map<FString, TValue, SStringHash, SStringEqual>;

// User-visible caseless text (Unicode simple folding). Not for paths: TPathMap.
template <typename TValue>
using TStringMapIgnoreCase = std::unordered_map<FString, TValue, SStringHashIgnoreCase, SStringEqualIgnoreCase>;

// Keys: FString::FromPath(Path.lexically_normal(), EPathStyle::Generic) -- only FromPath produces path keys
// (concatenated pieces do not re-canonicalize surrogate halves).
template <typename TValue>
using TPathMap = std::unordered_map<FString, TValue, SPathHash, SPathEqual>;

using FStringSet = std::unordered_set<FString, SStringHash, SStringEqual>;
using FStringSetIgnoreCase = std::unordered_set<FString, SStringHashIgnoreCase, SStringEqualIgnoreCase>;
}
