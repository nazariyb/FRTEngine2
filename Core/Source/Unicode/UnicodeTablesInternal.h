#pragma once

// Core-private: never include this outside Core/Source. The arrays it reads are not exported;
// other modules go through the exported unicode::detail::GetCharRecord instead.
//
// Core's per-code-point loops (IgnoreCase comparison, trimming, case conversion, truncation) include
// it so the lookup inlines into them instead of costing a call per code point.

#include "Unicode/Unicode.h"
#include "Unicode/UnicodeTables.generated.h"


namespace frt::unicode::tables
{
// The record of any code point: Index1 -> Index2 -> Leaf -> Records (UnicodeTables.generated.h).
// Above U+10FFFF there is no trie entry, and the answer is the Cn record.
[[nodiscard]] inline const detail::SCharRecord& LookupRecord (char32_t C) noexcept
{
	if (C > MaxCodePoint)
	{
		return Records[UnassignedRecord];
	}

	constexpr uint32 Index2Bits = Shift1 - Shift2;
	const uint32 index2Block = Index1[C >> Shift1];
	const uint32 leafBlock = Index2[(index2Block << Index2Bits) | ((C >> Shift2) & ((1u << Index2Bits) - 1u))];
	return Records[Leaf[(leafBlock << Shift2) | (C & ((1u << Shift2) - 1u))]];
}

// unicode::MapCase / FoldCase / GetGeneralCategory / IsWhitespace / ExtendsPrevious over the inline
// lookup: the same answers, for the same hot loops. Named apart from those on purpose: ECaseMapping
// would otherwise drag unicode::MapCase into every unqualified call by argument-dependent lookup.
[[nodiscard]] inline char32_t LookupMapCase (char32_t C, ECaseMapping Mapping) noexcept
{
	if (C < 0x80u)
	{
		return detail::MapAsciiCase(C, Mapping);
	}
	return detail::ApplyCaseDelta(C, detail::GetCaseDelta(LookupRecord(C), Mapping));
}

[[nodiscard]] inline char32_t LookupFoldCase (char32_t C) noexcept
{
	return LookupMapCase(C, ECaseMapping::Fold);
}

[[nodiscard]] inline EGeneralCategory LookupGeneralCategory (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return detail::GetAsciiCategory(static_cast<char>(C));
	}
	return LookupRecord(C).Category;
}

[[nodiscard]] inline bool LookupIsWhitespace (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return ascii::IsWhitespace(static_cast<char>(C));
	}
	return (LookupRecord(C).Flags & detail::WhiteSpace) != 0;
}

[[nodiscard]] inline bool LookupExtendsPrevious (char32_t C) noexcept
{
	if (C < 0x80u)
	{
		return false;
	}
	return detail::IsExtendingNonMark(C) || detail::IsMark(LookupRecord(C).Category);
}
}
