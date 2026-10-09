#include "Containers/StringView.h"

#include <charconv>
#include <cstring>
#include <ostream>
#include <string>
#include <system_error>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "Containers/String.h"
#include "Unicode/UnicodeTablesInternal.h"


namespace
{
using namespace frt;

constexpr uint32 NotFound = FStringView::NotFound;

// Keys of ill-formed bytes: one per byte, after every code point (IgnoreCase order, path keys).
constexpr uint32 IllFormedKeyBase = 0x110000u;
constexpr char32_t ZeroWidthJoiner = 0x200Du;

unicode::SDecodedCodePoint DecodeAt (FStringView Text, uint32 Offset) noexcept
{
	return unicode::DecodeUtf8(Text.GetData() + Offset, Text.Len() - Offset);
}

// The start of the unit that ends at End (End > 0, a boundary): backwards in the segmentation forward
// decoding produces.
uint32 PreviousUnitStart (FStringView Text, uint32 End) noexcept
{
	return detail::FloorCodePointBoundaryAt(Text.GetData(), Text.Len(), End - 1u);
}

// Two units equal under IgnoreCase: the same simple case folding, or the same ill-formed bytes. An
// ill-formed unit never equals a well-formed one (its keys sort after every code point).
bool SameUnitFolded (const char* A, const unicode::SDecodedCodePoint& UnitA, const char* B, const unicode::SDecodedCodePoint& UnitB) noexcept
{
	if (UnitA.bWellFormed != UnitB.bWellFormed)
	{
		return false;
	}
	if (UnitA.bWellFormed)
	{
		return UnitA.CodePoint == UnitB.CodePoint
			|| unicode::tables::LookupFoldCase(UnitA.CodePoint) == unicode::tables::LookupFoldCase(UnitB.CodePoint);
	}
	return UnitA.Length == UnitB.Length && std::memcmp(A, B, UnitA.Length) == 0;
}

// Haystack bytes matched when Needle's units match Hay's units from Start one by one, or NotFound.
uint32 MatchUnitsFolded (FStringView Hay, uint32 Start, FStringView Needle) noexcept
{
	const char* hay = Hay.GetData();
	const char* needle = Needle.GetData();
	uint32 h = Start;
	uint32 n = 0u;
	while (n < Needle.Len())
	{
		if (h >= Hay.Len())
		{
			return NotFound;
		}

		// ASCII against ASCII is a byte comparison.
		if (ascii::IsAscii(hay[h]) && ascii::IsAscii(needle[n]))
		{
			if (ascii::ToLower(hay[h]) != ascii::ToLower(needle[n]))
			{
				return NotFound;
			}
			++h;
			++n;
			continue;
		}

		const unicode::SDecodedCodePoint hayUnit = DecodeAt(Hay, h);
		const unicode::SDecodedCodePoint needleUnit = DecodeAt(Needle, n);
		if (!SameUnitFolded(hay + h, hayUnit, needle + n, needleUnit))
		{
			return NotFound;
		}
		h += hayUnit.Length;
		n += needleUnit.Length;
	}
	return h - Start;
}

// The next key of the IgnoreCase key sequence (units as TStringReadOps defines them): the folded code
// point of a well-formed unit, one key per byte of an ill-formed one. The bytes after the first of an
// ill-formed unit are continuation bytes, each a unit of its own: stepping one byte gives exactly one
// key per byte.
uint32 NextFoldedKey (FStringView Text, uint32& Position) noexcept
{
	const char byte = Text.GetData()[Position];
	if (ascii::IsAscii(byte))
	{
		++Position;
		return static_cast<unsigned char>(ascii::ToLower(byte));
	}

	const unicode::SDecodedCodePoint unit = DecodeAt(Text, Position);
	if (!unit.bWellFormed)
	{
		++Position;
		return IllFormedKeyBase + static_cast<unsigned char>(byte);
	}
	Position += unit.Length;
	return unicode::tables::LookupFoldCase(unit.CodePoint);
}

// The key of a path unit (SPathHash): NTFS upper-cases each UTF-16 unit with its $UpCase table, BMP only,
// so supplementary code points stay as they are. Ill-formed bytes (WTF-8 surrogates included) as is.
uint32 NextPathKey (FStringView Text, uint32& Position) noexcept
{
	const char byte = Text.GetData()[Position];
	if (ascii::IsAscii(byte))
	{
		++Position;
		return static_cast<unsigned char>(ascii::ToUpper(byte));
	}

	const unicode::SDecodedCodePoint unit = DecodeAt(Text, Position);
	if (!unit.bWellFormed)
	{
		++Position;
		return IllFormedKeyBase + static_cast<unsigned char>(byte);
	}
	Position += unit.Length;
	return unit.CodePoint <= 0xFFFFu ? unicode::tables::LookupMapCase(unit.CodePoint, unicode::ECaseMapping::Upper) : unit.CodePoint;
}

// Hashes a byte stream produced piece by piece, without allocating: 256-byte chunks, each hashed with the
// previous chunk's hash as the seed. A stream of up to 256 bytes hashes exactly like HashBytes of it.
class FHashStream
{
public:
	void Put (char Byte) noexcept
	{
		if (Count == sizeof(Buffer))
		{
			Seed = hash::HashBytes(Buffer, Count, Seed);
			Count = 0u;
		}
		Buffer[Count++] = Byte;
	}

	// A key of NextFoldedKey / NextPathKey: a code point as UTF-8, an ill-formed byte as itself.
	void PutKey (uint32 Key) noexcept
	{
		if (Key >= IllFormedKeyBase)
		{
			Put(static_cast<char>(Key - IllFormedKeyBase));
			return;
		}
		char bytes[4] = {};
		const uint32 count = unicode::EncodeWtf8(static_cast<char32_t>(Key), bytes);
		for (uint32 i = 0u; i < count; ++i)
		{
			Put(bytes[i]);
		}
	}

	[[nodiscard]] uint64 Finish () const noexcept
	{
		return hash::HashBytes(Buffer, Count, Seed);
	}

private:
	char Buffer[256] = {};
	uint32 Count = 0u;
	uint64 Seed = 0u;
};

// IgnoreCase may compare bytes when the needle is ASCII without s S k K: exactly two non-ASCII code points
// fold to ASCII, U+017F LONG S -> s and U+212A KELVIN SIGN -> k, so a match of such a needle is ASCII.
bool CanFoldAsBytes (FStringView Needle, ESearchCase Case) noexcept
{
	if (Case == ESearchCase::IgnoreCaseAscii)
	{
		return true;
	}
	for (const char c : Needle)
	{
		if (!ascii::IsAscii(c) || c == 's' || c == 'S' || c == 'k' || c == 'K')
		{
			return false;
		}
	}
	return true;
}

// Whether the unit at Unit (decoded as Decoded) is one of Set's units (6.4 set rules).
bool SetContainsUnit (FStringView Set, const char* Unit, const unicode::SDecodedCodePoint& Decoded) noexcept
{
	if (Decoded.bWellFormed && Decoded.CodePoint < 0x80u)
	{
		return std::memchr(Set.GetData(), static_cast<int>(Decoded.CodePoint), Set.Len()) != nullptr;
	}

	for (uint32 i = 0u; i < Set.Len();)
	{
		const unicode::SDecodedCodePoint member = DecodeAt(Set, i);
		if (Decoded.bWellFormed
			? member.bWellFormed && member.CodePoint == Decoded.CodePoint
			: !member.bWellFormed && member.Length == Decoded.Length && std::memcmp(Set.GetData() + i, Unit, member.Length) == 0)
		{
			return true;
		}
		i += member.Length;
	}
	return false;
}

bool UnitsMatch (const char* A, const unicode::SDecodedCodePoint& UnitA, const char* B, const unicode::SDecodedCodePoint& UnitB,
	ESearchCase Case) noexcept
{
	switch (Case)
	{
	case ESearchCase::IgnoreCase:
		return SameUnitFolded(A, UnitA, B, UnitB);
	case ESearchCase::IgnoreCaseAscii:
		return UnitA.Length == UnitB.Length && detail::EqualBytesIgnoreCaseAscii(A, B, UnitA.Length);
	default:
		return UnitA.Length == UnitB.Length && std::memcmp(A, B, UnitA.Length) == 0;
	}
}

// A text made of Head followed by Tail: Truncate's rules read a few bytes past a cut, and FormatToBuffer
// has those bytes only in its lookahead. Nothing follows Tail (or what does is never read: the rules read
// at most 4 bytes past the cut, and Tail holds 4 when there are more).
struct SSeamText
{
	const char* Head;
	uint32 HeadLen;
	const char* Tail;
	uint32 TailLen;

	[[nodiscard]] uint32 Len () const noexcept { return HeadLen + TailLen; }
	[[nodiscard]] char At (uint32 Index) const noexcept { return Index < HeadLen ? Head[Index] : Tail[Index - HeadLen]; }

	[[nodiscard]] unicode::SDecodedCodePoint DecodeAt (uint32 Index) const noexcept
	{
		if (Index + 4u <= HeadLen || TailLen == 0u)
		{
			return unicode::DecodeUtf8(Head + Index, HeadLen - Index);
		}
		char window[4] = {};
		const uint32 available = Len() - Index < 4u ? Len() - Index : 4u;
		for (uint32 i = 0u; i < available; ++i)
		{
			window[i] = At(Index + i);
		}
		return unicode::DecodeUtf8(window, available);
	}

	// detail::FloorCodePointBoundaryAt across the seam.
	[[nodiscard]] uint32 FloorBoundary (uint32 Index) const noexcept
	{
		if (Index >= Len())
		{
			return Len();
		}
		if (!unicode::IsContinuationByte(At(Index)))
		{
			return Index;
		}
		const uint32 lowest = Index >= 3u ? Index - 3u : 0u;
		for (uint32 lead = Index; lead-- > lowest;)
		{
			if (!unicode::IsContinuationByte(At(lead)))
			{
				return lead + DecodeAt(lead).Length <= Index ? Index : lead;
			}
		}
		return Index;
	}
};

// 6.5: the longest prefix of at most MaxBytes that ends on a boundary and does not cut a visible sequence.
uint32 TruncateSeamText (const SSeamText& Text, uint32 MaxBytes) noexcept
{
	if (MaxBytes >= Text.Len())
	{
		return Text.Len();
	}

	uint32 cut = Text.FloorBoundary(MaxBytes);

	// Back off while the cut separates a ZWJ from what follows it, or a mark / modifier / selector / tag
	// from what it modifies.
	while (cut > 0u)
	{
		const uint32 previousStart = Text.FloorBoundary(cut - 1u);
		const unicode::SDecodedCodePoint previous = Text.DecodeAt(previousStart);
		bool bJoined = previous.bWellFormed && previous.CodePoint == ZeroWidthJoiner;
		if (!bJoined && cut < Text.Len())
		{
			const unicode::SDecodedCodePoint next = Text.DecodeAt(cut);
			bJoined = next.bWellFormed && unicode::tables::LookupExtendsPrevious(next.CodePoint);
		}
		if (!bJoined)
		{
			break;
		}
		cut = previousStart;
	}

	// Regional indicators pair up into flags from the start of their run: an odd count before the cut
	// means the cut halves a flag.
	uint32 indicators = 0u;
	for (uint32 end = cut; end > 0u;)
	{
		const uint32 start = Text.FloorBoundary(end - 1u);
		const unicode::SDecodedCodePoint unit = Text.DecodeAt(start);
		if (!unit.bWellFormed || !unicode::IsRegionalIndicator(unit.CodePoint))
		{
			break;
		}
		++indicators;
		end = start;
	}
	if (indicators % 2u != 0u)
	{
		cut -= 4u;
	}
	return cut;
}

// The optional '+' of the parsers (from_chars takes none), which must not be followed by a sign.
const char* SkipPlus (const char* First, const char* Last) noexcept
{
	if (First != Last && *First == '+')
	{
		++First;
		if (First != Last && (*First == '+' || *First == '-'))
		{
			return nullptr;
		}
	}
	return First;
}

template <typename T>
bool ParseWithFromChars (FStringView Text, int Base, T& Out, uint32& Consumed) noexcept
{
	const char* begin = Text.GetData();
	const char* end = begin + Text.Len();
	const char* first = SkipPlus(begin, end);
	if (first == nullptr)
	{
		return false;
	}

	T value{};
	std::from_chars_result result{};
	if constexpr (std::is_floating_point_v<T>)
	{
		(void)Base;
		result = std::from_chars(first, end, value, std::chars_format::general);
	}
	else
	{
		result = std::from_chars(first, end, value, Base);
	}

	if (result.ec != std::errc())
	{
		return false;
	}
	Out = value;
	Consumed = static_cast<uint32>(result.ptr - begin);
	return true;
}


// wyhash (final version 4) by Wang Yi, released into the public domain under The Unlicense
// (https://github.com/wangyi-fudan/wyhash): a port of the reference algorithm in this engine's style.
// Multiply-mix of 64-bit words: fast on short keys, which is what the string maps hash.
constexpr uint64 WySecret[4] = { 0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull };

// The 128-bit product of A and B: low half in A, high half in B.
inline void WyMultiply (uint64& A, uint64& B) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
	uint64 high = 0u;
	A = _umul128(A, B, &high);
	B = high;
#else
	const unsigned __int128 product = static_cast<unsigned __int128>(A) * B;
	A = static_cast<uint64>(product);
	B = static_cast<uint64>(product >> 64);
#endif
}

inline uint64 WyMix (uint64 A, uint64 B) noexcept
{
	WyMultiply(A, B);
	return A ^ B;
}

inline uint64 WyRead8 (const uint8* P) noexcept
{
	uint64 value = 0u;
	std::memcpy(&value, P, 8u);
	return value;
}

inline uint64 WyRead4 (const uint8* P) noexcept
{
	uint32 value = 0u;
	std::memcpy(&value, P, 4u);
	return value;
}

// 1-3 bytes: first, middle, last.
inline uint64 WyRead3 (const uint8* P, size_t K) noexcept
{
	return (static_cast<uint64>(P[0]) << 16) | (static_cast<uint64>(P[K >> 1]) << 8) | P[K - 1u];
}
}


namespace frt::hash
{
uint64 HashBytes (const void* Data, size_t Size, uint64 Seed) noexcept
{
	const uint8* p = static_cast<const uint8*>(Data);
	uint64 seed = Seed ^ WyMix(Seed ^ WySecret[0], WySecret[1]);
	uint64 a = 0u;
	uint64 b = 0u;

	if (Size <= 16u)
	{
		if (Size >= 4u)
		{
			a = (WyRead4(p) << 32) | WyRead4(p + ((Size >> 3) << 2));
			b = (WyRead4(p + Size - 4u) << 32) | WyRead4(p + Size - 4u - ((Size >> 3) << 2));
		}
		else if (Size > 0u)
		{
			a = WyRead3(p, Size);
		}
	}
	else
	{
		size_t remaining = Size;
		if (remaining > 48u)
		{
			uint64 seed1 = seed;
			uint64 seed2 = seed;
			do
			{
				seed = WyMix(WyRead8(p) ^ WySecret[1], WyRead8(p + 8) ^ seed);
				seed1 = WyMix(WyRead8(p + 16) ^ WySecret[2], WyRead8(p + 24) ^ seed1);
				seed2 = WyMix(WyRead8(p + 32) ^ WySecret[3], WyRead8(p + 40) ^ seed2);
				p += 48;
				remaining -= 48u;
			}
			while (remaining > 48u);
			seed ^= seed1 ^ seed2;
		}
		while (remaining > 16u)
		{
			seed = WyMix(WyRead8(p) ^ WySecret[1], WyRead8(p + 8) ^ seed);
			remaining -= 16u;
			p += 16;
		}
		// The last 16 bytes, overlapping what was mixed already when fewer remain.
		a = WyRead8(p + remaining - 16u);
		b = WyRead8(p + remaining - 8u);
	}

	a ^= WySecret[1];
	b ^= seed;
	WyMultiply(a, b);
	return WyMix(a ^ WySecret[0] ^ Size, b ^ WySecret[1]);
}
}


namespace frt::string_impl
{
bool EqualsFolded (FStringView A, FStringView B, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	if (Case == ESearchCase::IgnoreCaseAscii)
	{
		return A.Len() == B.Len() && detail::EqualBytesIgnoreCaseAscii(A.GetData(), B.GetData(), A.Len());
	}

	// No length shortcut: folding changes byte lengths (U+212A is 3 bytes, its fold 'k' one).
	uint32 a = 0u;
	uint32 b = 0u;
	while (a < A.Len() && b < B.Len())
	{
		if (NextFoldedKey(A, a) != NextFoldedKey(B, b))
		{
			return false;
		}
	}
	return a == A.Len() && b == B.Len();
}

int32 CompareFolded (FStringView A, FStringView B, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	if (Case == ESearchCase::IgnoreCaseAscii)
	{
		return A.Compare<ESearchCase::IgnoreCaseAscii>(B);
	}

	// Lexicographic over the key sequences; a proper prefix is less.
	uint32 a = 0u;
	uint32 b = 0u;
	while (a < A.Len() && b < B.Len())
	{
		const uint32 keyA = NextFoldedKey(A, a);
		const uint32 keyB = NextFoldedKey(B, b);
		if (keyA != keyB)
		{
			return keyA < keyB ? -1 : 1;
		}
	}
	const bool bEndA = a == A.Len();
	const bool bEndB = b == B.Len();
	return bEndA == bEndB ? 0 : (bEndA ? -1 : 1);
}

uint32 MatchPrefixFolded (FStringView Hay, FStringView Prefix, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	if (Case == ESearchCase::IgnoreCaseAscii)
	{
		return Hay.Len() >= Prefix.Len() && detail::EqualBytesIgnoreCaseAscii(Hay.GetData(), Prefix.GetData(), Prefix.Len())
			? Prefix.Len()
			: NotFound;
	}
	return MatchUnitsFolded(Hay, 0u, Prefix);
}

uint32 MatchSuffixFolded (FStringView Hay, FStringView Suffix, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	if (Case == ESearchCase::IgnoreCaseAscii)
	{
		return Hay.Len() >= Suffix.Len()
			&& detail::EqualBytesIgnoreCaseAscii(Hay.GetData() + (Hay.Len() - Suffix.Len()), Suffix.GetData(), Suffix.Len())
			? Suffix.Len()
			: NotFound;
	}

	// Unit by unit from the ends. Backward steps follow the forward segmentation, so the match starts at
	// one of the haystack's unit starts.
	uint32 h = Hay.Len();
	uint32 n = Suffix.Len();
	while (n > 0u)
	{
		if (h == 0u)
		{
			return NotFound;
		}
		const uint32 needleStart = PreviousUnitStart(Suffix, n);
		const uint32 hayStart = PreviousUnitStart(Hay, h);
		if (!SameUnitFolded(Hay.GetData() + hayStart, DecodeAt(Hay, hayStart), Suffix.GetData() + needleStart, DecodeAt(Suffix, needleStart)))
		{
			return NotFound;
		}
		h = hayStart;
		n = needleStart;
	}
	return Hay.Len() - h;
}

SStringMatch FindFolded (FStringView Hay, FStringView Needle, uint32 Start, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	if (Start > Hay.Len())
	{
		return SStringMatch();
	}
	if (Needle.IsEmpty())
	{
		return SStringMatch{ Start, 0u };
	}

	if (CanFoldAsBytes(Needle, Case))
	{
		for (uint32 i = Start; Hay.Len() - i >= Needle.Len(); ++i)
		{
			if (detail::EqualBytesIgnoreCaseAscii(Hay.GetData() + i, Needle.GetData(), Needle.Len()))
			{
				return SStringMatch{ i, Needle.Len() };
			}
		}
		return SStringMatch();
	}

	// Candidates are the haystack's unit starts, decoded forward from Start.
	for (uint32 i = Start; i < Hay.Len(); i += DecodeAt(Hay, i).Length)
	{
		const uint32 matched = MatchUnitsFolded(Hay, i, Needle);
		if (matched != NotFound)
		{
			return SStringMatch{ i, matched };
		}
	}
	return SStringMatch();
}

SStringMatch FindLastFolded (FStringView Hay, FStringView Needle, uint32 End, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	const uint32 end = End < Hay.Len() ? End : Hay.Len();
	if (Needle.IsEmpty())
	{
		return SStringMatch{ end, 0u };
	}

	if (CanFoldAsBytes(Needle, Case))
	{
		for (uint32 i = end >= Needle.Len() ? end - Needle.Len() + 1u : 0u; i-- > 0u;)
		{
			if (detail::EqualBytesIgnoreCaseAscii(Hay.GetData() + i, Needle.GetData(), Needle.Len()))
			{
				return SStringMatch{ i, Needle.Len() };
			}
		}
		return SStringMatch();
	}

	// Forward over the unit starts, keeping the last match that ends by End: matched lengths vary, and
	// backward candidates would not be the forward segmentation's.
	SStringMatch last;
	for (uint32 i = 0u; i < end; i += DecodeAt(Hay, i).Length)
	{
		const uint32 matched = MatchUnitsFolded(Hay, i, Needle);
		if (matched != NotFound && i + matched <= end)
		{
			last = SStringMatch{ i, matched };
		}
	}
	return last;
}

uint32 FindAny (FStringView Hay, std::span<const FStringView> Needles, uint32 Start, ESearchCase Case, uint32* OutNeedleIndex) noexcept
{
	uint32 best = NotFound;
	uint32 bestNeedle = NotFound;
	for (size_t i = 0u; i < Needles.size(); ++i)
	{
		if (Needles[i].IsEmpty())
		{
			continue;
		}
		const uint32 found = Case == ESearchCase::CaseSensitive
			? Hay.Find(Needles[i], Start)
			: FindFolded(Hay, Needles[i], Start, Case).Index;
		// Strictly earlier only: a tie keeps the lower needle index.
		if (found < best)
		{
			best = found;
			bestNeedle = static_cast<uint32>(i);
		}
	}
	if (OutNeedleIndex != nullptr)
	{
		*OutNeedleIndex = bestNeedle;
	}
	return best;
}

uint32 FindInSet (FStringView Hay, FStringView Set, uint32 Start, bool bNegate) noexcept
{
	const char* data = Hay.GetData();
	if (unicode::IsAscii(Set.GetData(), Set.Len()))
	{
		// An ASCII set never holds a byte >= 0x80, and every ASCII byte starts a unit: a byte scan. A
		// non-ASCII byte found while negated follows set members only, so it starts a unit too.
		for (uint32 i = Start; i < Hay.Len(); ++i)
		{
			const bool bMember = ascii::IsAscii(data[i]) && std::memchr(Set.GetData(), data[i], Set.Len()) != nullptr;
			if (bMember != bNegate)
			{
				return i;
			}
		}
		return NotFound;
	}

	for (uint32 i = Start; i < Hay.Len();)
	{
		const unicode::SDecodedCodePoint unit = DecodeAt(Hay, i);
		if (SetContainsUnit(Set, data + i, unit) != bNegate)
		{
			return i;
		}
		i += unit.Length;
	}
	return NotFound;
}

uint32 FindLastInSet (FStringView Hay, FStringView Set, uint32 End, bool bNegate) noexcept
{
	// The unit End cuts into counts: it starts before End.
	for (uint32 end = End < Hay.Len() ? End : Hay.Len(); end > 0u;)
	{
		const uint32 start = PreviousUnitStart(Hay, end);
		if (SetContainsUnit(Set, Hay.GetData() + start, DecodeAt(Hay, start)) != bNegate)
		{
			return start;
		}
		end = start;
	}
	return NotFound;
}

bool MatchesWildcard (FStringView Text, FStringView Pattern, ESearchCase Case) noexcept
{
	// Greedy with backtracking to the last '*': linear for one star, O(n * m) at worst. Everything moves
	// by whole units.
	const char* text = Text.GetData();
	const char* pattern = Pattern.GetData();
	uint32 t = 0u;
	uint32 p = 0u;
	uint32 starPattern = NotFound;
	uint32 starText = 0u;
	while (t < Text.Len())
	{
		if (p < Pattern.Len())
		{
			if (pattern[p] == '*')
			{
				starPattern = p++;
				starText = t;
				continue;
			}

			const unicode::SDecodedCodePoint textUnit = DecodeAt(Text, t);
			if (pattern[p] == '?')
			{
				++p;
				t += textUnit.Length;
				continue;
			}

			const unicode::SDecodedCodePoint patternUnit = DecodeAt(Pattern, p);
			if (UnitsMatch(text + t, textUnit, pattern + p, patternUnit, Case))
			{
				p += patternUnit.Length;
				t += textUnit.Length;
				continue;
			}
		}

		if (starPattern == NotFound)
		{
			return false;
		}
		// Let the last '*' take one more unit.
		p = starPattern + 1u;
		starText += DecodeAt(Text, starText).Length;
		t = starText;
	}

	while (p < Pattern.Len() && pattern[p] == '*')
	{
		++p;
	}
	return p == Pattern.Len();
}

uint32 TrimStartLength (FStringView Text, FStringView CharSet) noexcept
{
	if (!CharSet.IsEmpty())
	{
		const uint32 first = FindInSet(Text, CharSet, 0u, true);
		return first == NotFound ? Text.Len() : first;
	}

	uint32 i = 0u;
	while (i < Text.Len())
	{
		const unicode::SDecodedCodePoint unit = DecodeAt(Text, i);
		if (!unit.bWellFormed || !unicode::tables::LookupIsWhitespace(unit.CodePoint))
		{
			break;
		}
		i += unit.Length;
	}
	return i;
}

uint32 TrimEndLength (FStringView Text, FStringView CharSet) noexcept
{
	if (!CharSet.IsEmpty())
	{
		const uint32 last = FindLastInSet(Text, CharSet, NotFound, true);
		return last == NotFound ? Text.Len() : Text.Len() - (last + DecodeAt(Text, last).Length);
	}

	uint32 end = Text.Len();
	while (end > 0u)
	{
		const uint32 start = PreviousUnitStart(Text, end);
		const unicode::SDecodedCodePoint unit = DecodeAt(Text, start);
		if (!unit.bWellFormed || !unicode::tables::LookupIsWhitespace(unit.CodePoint))
		{
			break;
		}
		end = start;
	}
	return Text.Len() - end;
}

uint32 TruncateLength (FStringView Text, uint32 MaxBytes) noexcept
{
	return TruncateSeamText(SSeamText{ Text.GetData(), Text.Len(), nullptr, 0u }, MaxBytes);
}

uint32 TruncateFormattedLength (const char* Data, uint32 Written, const char* Lookahead, uint32 LookaheadLen) noexcept
{
	frt_assert(LookaheadLen >= 1u && LookaheadLen <= 4u)
	return TruncateSeamText(SSeamText{ Data, Written, Lookahead, LookaheadLen }, Written);
}

uint64 HashFolded (FStringView Text, ESearchCase Case) noexcept
{
	frt_assert(Case != ESearchCase::CaseSensitive)
	FHashStream stream;
	if (Case == ESearchCase::IgnoreCaseAscii)
	{
		for (const char c : Text)
		{
			stream.Put(ascii::ToLower(c));
		}
		return stream.Finish();
	}

	// The UTF-8 of each well-formed unit's folding, each ill-formed byte as is (never U+FFFD): equal key
	// sequences give equal streams.
	for (uint32 i = 0u; i < Text.Len();)
	{
		stream.PutKey(NextFoldedKey(Text, i));
	}
	return stream.Finish();
}

uint64 HashPath (FStringView Text) noexcept
{
	FHashStream stream;
	for (uint32 i = 0u; i < Text.Len();)
	{
		stream.PutKey(NextPathKey(Text, i));
	}
	return stream.Finish();
}

bool EqualsPath (FStringView A, FStringView B) noexcept
{
	uint32 a = 0u;
	uint32 b = 0u;
	while (a < A.Len() && b < B.Len())
	{
		if (NextPathKey(A, a) != NextPathKey(B, b))
		{
			return false;
		}
	}
	return a == A.Len() && b == B.Len();
}

bool ParseSigned (FStringView Text, uint32 Base, int64& Out, uint32& Consumed) noexcept
{
	frt_assert(Base >= 2u && Base <= 36u)
	return ParseWithFromChars(Text, static_cast<int>(Base), Out, Consumed);
}

bool ParseUnsigned (FStringView Text, uint32 Base, uint64& Out, uint32& Consumed) noexcept
{
	frt_assert(Base >= 2u && Base <= 36u)
	return ParseWithFromChars(Text, static_cast<int>(Base), Out, Consumed);
}

bool ParseFloat (FStringView Text, float& Out, uint32& Consumed) noexcept
{
	return ParseWithFromChars(Text, 10, Out, Consumed);
}

bool ParseDouble (FStringView Text, double& Out, uint32& Consumed) noexcept
{
	return ParseWithFromChars(Text, 10, Out, Consumed);
}

bool ToPath (FStringView Text, bool bStrict, std::filesystem::path& Out) noexcept
{
	if (bStrict && !unicode::IsValidWtf8(Text.GetData(), Text.Len()))
	{
		return false;
	}

	// Our own transcoder, not MultiByteToWideChar: Win32 would turn WTF-8 surrogates into U+FFFD and break
	// the path -> FString -> path round trip.
	const uint64 length = unicode::Utf16LengthOfWtf8(Text.GetData(), Text.Len());
	std::wstring wide(static_cast<size_t>(length), wchar_t());
	unicode::ConvertWtf8ToUtf16(Text.GetData(), Text.Len(), wide.data());
	Out = std::filesystem::path(std::move(wide));
	return true;
}

bool DecodeHex (FStringView Text, std::span<uint8> Out) noexcept
{
	if (Text.Len() != 2u * Out.size())
	{
		return false;
	}

	for (size_t i = 0u; i < Out.size(); ++i)
	{
		const int32 high = ascii::HexValue(Text[static_cast<uint32>(2u * i)]);
		const int32 low = ascii::HexValue(Text[static_cast<uint32>(2u * i + 1u)]);
		if (high < 0 || low < 0)
		{
			return false;
		}
		Out[i] = static_cast<uint8>((high << 4) | low);
	}
	return true;
}
}


namespace frt
{
std::ostream& operator<< (std::ostream& Out, FStringView Text)
{
	return Out.write(Text.GetData(), static_cast<std::streamsize>(Text.Len()));
}

void PrintTo (const FStringView& Text, std::ostream* Out)
{
	*Out << '"' << FString::Escape(Text) << '"';
}

void PrintTo (const FStringLiteral& Text, std::ostream* Out)
{
	PrintTo(FStringView(Text), Out);
}
}
