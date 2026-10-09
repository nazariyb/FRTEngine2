#pragma once

#include <compare>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Containers/StringFwd.h"
#include "Core.h"
#include "CoreTypes.h"
#include "Math/MathUtility.h"
#include "Unicode/Unicode.h"


/**
 * Borrowed UTF-8 text, and the read-only API every string type shares.
 *
 *	- FStringView: pointer + length, not NUL-terminated, constexpr. Stores nullptr for every empty
 *	  view, so an empty view made by a game module never points into its image after it unloads.
 *	- FStringLiteral: an immutable view of a string literal. Static and NUL-terminated by type, so it
 *	  has CStr(); constructible only in constant evaluation.
 *	- detail::TStringReadOps: searching, comparison, splitting, parsing and conversion, written once
 *	  for FStringView, FStringLiteral and FString (Containers/String.h).
 *
 * Bytes are UTF-8 by contract, not by invariant: nothing validates on construction, every decoder is
 * total (an ill-formed sequence is one U+FFFD per maximal subpart). Lengths and positions are bytes;
 * positions that would split a code point assert in Debug/Development (FRT_STRING_ASSERT_BOUNDARY).
 * Every function that looks for or compares a text or a character takes ESearchCase as a template
 * parameter (the list, and the exact-by-design exceptions, are at ESearchCase in StringFwd.h);
 * CaseSensitive paths are inline, the folded ones are exported from Core (string_impl).
 */

// Every substring/insert/remove position (Debug and Development): a position inside a UTF-8 sequence
// would split a code point. Constant evaluation skips it - the inputs there are literals.
#define FRT_STRING_ASSERT_BOUNDARY(Pos) \
	if (!std::is_constant_evaluated()) { frt_assert(this->IsCodePointBoundary(static_cast<uint32>(Pos))) }


namespace frt
{
struct SStringSplit;
struct SStringMatch;
class FCodePointRange;
class FStringSplitRange;
class FStringLineRange;


/**
 * The out-of-line algorithms behind the read-only API (StringView.cpp). Case arguments of the
 * *Folded functions are IgnoreCase or IgnoreCaseAscii: the CaseSensitive paths are inline.
 */
namespace string_impl
{
[[nodiscard]] FRT_CORE_API bool EqualsFolded (FStringView A, FStringView B, ESearchCase Case) noexcept;
[[nodiscard]] FRT_CORE_API int32 CompareFolded (FStringView A, FStringView B, ESearchCase Case) noexcept;
// Matched haystack bytes when Hay starts / ends with the affix (folded), else NotFound.
[[nodiscard]] FRT_CORE_API uint32 MatchPrefixFolded (FStringView Hay, FStringView Prefix, ESearchCase Case) noexcept;
[[nodiscard]] FRT_CORE_API uint32 MatchSuffixFolded (FStringView Hay, FStringView Suffix, ESearchCase Case) noexcept;
[[nodiscard]] FRT_CORE_API SStringMatch FindFolded (FStringView Hay, FStringView Needle, uint32 Start, ESearchCase Case) noexcept;
[[nodiscard]] FRT_CORE_API SStringMatch FindLastFolded (FStringView Hay, FStringView Needle, uint32 End, ESearchCase Case) noexcept;
// Any case. Not found: NotFound, and *OutNeedleIndex (when given) is NotFound too.
[[nodiscard]] FRT_CORE_API uint32 FindAny (FStringView Hay, std::span<const FStringView> Needles, uint32 Start, ESearchCase Case, uint32* OutNeedleIndex) noexcept;
[[nodiscard]] FRT_CORE_API uint32 FindInSet (FStringView Hay, FStringView Set, uint32 Start, bool bNegate) noexcept;
[[nodiscard]] FRT_CORE_API uint32 FindLastInSet (FStringView Hay, FStringView Set, uint32 End, bool bNegate) noexcept;
// Any case.
[[nodiscard]] FRT_CORE_API bool MatchesWildcard (FStringView Text, FStringView Pattern, ESearchCase Case) noexcept;
// Bytes to remove from the start / the end: whole units of CharSet, or of White_Space when CharSet is empty.
[[nodiscard]] FRT_CORE_API uint32 TrimStartLength (FStringView Text, FStringView CharSet) noexcept;
[[nodiscard]] FRT_CORE_API uint32 TrimEndLength (FStringView Text, FStringView CharSet) noexcept;
// FStringView::Truncate's label-safe cut: the length to keep.
[[nodiscard]] FRT_CORE_API uint32 TruncateLength (FStringView Text, uint32 MaxBytes) noexcept;
[[nodiscard]] FRT_CORE_API uint64 HashFolded (FStringView Text, ESearchCase Case) noexcept;
[[nodiscard]] FRT_CORE_API uint64 HashPath (FStringView Text) noexcept;
[[nodiscard]] FRT_CORE_API bool EqualsPath (FStringView A, FStringView B) noexcept;
// Parsing: Consumed = bytes used. ASCII digits only.
FRT_CORE_API bool ParseSigned (FStringView Text, uint32 Base, int64& Out, uint32& Consumed) noexcept;
FRT_CORE_API bool ParseUnsigned (FStringView Text, uint32 Base, uint64& Out, uint32& Consumed) noexcept;
FRT_CORE_API bool ParseFloat (FStringView Text, float& Out, uint32& Consumed) noexcept;
FRT_CORE_API bool ParseDouble (FStringView Text, double& Out, uint32& Consumed) noexcept;
FRT_CORE_API bool ToPath (FStringView Text, bool bStrict, std::filesystem::path& Out) noexcept;
FRT_CORE_API bool DecodeHex (FStringView Text, std::span<uint8> Out) noexcept;
}


namespace hash
{
// 64-bit multiply-mix hash of the wyhash family (StringView.cpp). Not stable across versions: never
// write a hash to disk.
[[nodiscard]] FRT_CORE_API uint64 HashBytes (const void* Data, size_t Size, uint64 Seed = 0) noexcept;
}


namespace detail
{
// Longest numeric prefix of Text as T (from_chars rules plus an optional '+'), range-checked.
template <concepts::StringNumber T>
std::optional<T> ParseNumber (FStringView Text, uint32 Base, uint32& OutConsumed) noexcept;

// Called only from a failing constant evaluation: its being non-constexpr is the error message.
inline void StringLiteralIsNotNulTerminated () noexcept
{}
}
}


namespace frt::detail
{
/**
 * The read-only API of FStringView, FStringLiteral and FString, written once. TDerived provides
 * GetData() (never null) and Len().
 *
 * Results are byte offsets, or NotFound. Case-sensitive paths are constexpr and run on
 * std::string_view's algorithms; IgnoreCase / IgnoreCaseAscii searches and everything that needs the
 * Unicode tables call string_impl.
 *
 * Case semantics: CaseSensitive compares bytes (unsigned, which is code-point order for well-formed
 * text). IgnoreCaseAscii lowers A-Z and compares bytes. IgnoreCase compares *units* (a well-formed
 * code point, or one ill-formed maximal subpart) by their simple case folding; an ill-formed unit
 * only ever equals the same bytes. A match can therefore be longer or shorter than the needle
 * (U+212A KELVIN SIGN, 3 bytes, matches "k"): whatever continues after a match uses the length
 * FindMatch reports, never Needle.Len().
 */
template <typename TDerived>
class TStringReadOps
{
public:
	using IndexStrategy = math::SIndexStrategy;

	static constexpr uint32 NotFound = 0xFFFFFFFFu;
	static constexpr uint32 MaxLength = 0x7FFFFFFFu;
	// Default End of Slice.
	static constexpr int64 SliceEnd = (std::numeric_limits<int64>::max)();

	// ---- View / element access ----
	[[nodiscard]] constexpr FStringView View () const noexcept FRT_LIFETIMEBOUND;

	// SIndexStrategy::IsValid<TIndex>(Index, Len()).
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default>
	[[nodiscard]] constexpr bool IsIndexValid (int64 Index) const noexcept;

	// A byte; asserts IsIndexValid. Get<IS_Circular>(-1) is the last byte.
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default>
	[[nodiscard]] constexpr char Get (int64 Index) const noexcept;

	// Bytes; assert non-empty.
	[[nodiscard]] constexpr char First () const noexcept;
	[[nodiscard]] constexpr char Last () const noexcept;

	// ---- Comparison ----
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool Equals (FStringView Other) const noexcept;

	// -1, 0 or 1. IgnoreCase: a strict weak order consistent with Equals<IgnoreCase>.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr int32 Compare (FStringView Other) const noexcept;

	// ---- Search. Results are byte offsets or NotFound. ----
	// First occurrence starting at >= Start. Empty Needle -> Start if Start <= Len(), else NotFound.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr uint32 Find (FStringView Needle, uint32 Start = 0) const noexcept;

	// Like Find, plus the matched haystack length, which differs from Needle.Len() under IgnoreCase.
	// Use it whenever the code continues after the match: S.Mid(M.Index + M.Len).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr SStringMatch FindMatch (FStringView Needle, uint32 Start = 0) const noexcept;

	// Ch: an ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr uint32 Find (char Ch, uint32 Start = 0) const noexcept;

	// Asserts IsScalarValue; encodes, then the view overload.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] uint32 Find (char32_t CodePoint, uint32 Start = 0) const noexcept;

	// Last occurrence lying entirely inside [0, End) (End clamped to Len()). "X".FindLast("X") == 0.
	// Empty Needle -> min(End, Len()).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr uint32 FindLast (FStringView Needle, uint32 End = NotFound) const noexcept;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr SStringMatch FindLastMatch (FStringView Needle, uint32 End = NotFound) const noexcept;

	// Ch: an ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr uint32 FindLast (char Ch, uint32 End = NotFound) const noexcept;

	// Asserts IsScalarValue.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] uint32 FindLast (char32_t CodePoint, uint32 End = NotFound) const noexcept;

	// Code-point sets (case-sensitive): CharSet is decoded into units (ASCII-only sets use a byte scan).
	// An ill-formed unit matches only the same ill-formed bytes in the set.
	[[nodiscard]] uint32 FindFirstOf (FStringView CharSet, uint32 Start = 0) const noexcept;
	[[nodiscard]] uint32 FindFirstNotOf (FStringView CharSet, uint32 Start = 0) const noexcept;
	// The last unit starting before End.
	[[nodiscard]] uint32 FindLastOf (FStringView CharSet, uint32 End = NotFound) const noexcept;
	[[nodiscard]] uint32 FindLastNotOf (FStringView CharSet, uint32 End = NotFound) const noexcept;

	// Pred(char32_t) -> bool over decoded units (U+FFFD for an ill-formed unit; walk CodePoints() and
	// use FCodePointIterator::IsWellFormed when the difference matters).
	template <typename TPredicate>
	[[nodiscard]] uint32 FindIf (TPredicate&& Pred, uint32 Start = 0) const;

	template <typename TPredicate>
	[[nodiscard]] uint32 FindLastIf (TPredicate&& Pred, uint32 End = NotFound) const;

	// Earliest match of any non-empty needle; ties -> lowest needle index; OutNeedleIndex gets that index
	// (NotFound when nothing matched).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] uint32 FindAny (std::span<const FStringView> Needles, uint32 Start = 0, uint32* OutNeedleIndex = nullptr) const noexcept;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] uint32 FindAny (std::initializer_list<FStringView> Needles, uint32 Start = 0, uint32* OutNeedleIndex = nullptr) const noexcept;

	// "" -> true.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool Contains (FStringView Needle) const noexcept;

	// An ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool Contains (char Ch) const noexcept;

	// A scalar value (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] bool Contains (char32_t CodePoint) const noexcept;

	// "" -> true.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool StartsWith (FStringView Prefix) const noexcept;

	// An ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool StartsWith (char Ch) const noexcept;

	// A scalar value (asserted). Every ASCII-character overload has a char32_t one, so a code point never
	// reaches a char parameter as its low byte (U'ї' as 'W', which the ASCII assert cannot tell from a real
	// 'W'); the lazy Split's and FString::ReplaceCharInline's are deleted instead.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] bool StartsWith (char32_t CodePoint) const noexcept;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool EndsWith (FStringView Suffix) const noexcept;

	// An ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr bool EndsWith (char Ch) const noexcept;

	// A scalar value (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] bool EndsWith (char32_t CodePoint) const noexcept;

	// Non-overlapping, left to right; empty Needle -> 0.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] uint32 CountOccurrences (FStringView Needle) const noexcept;

	// '*' = any run of units (including none), '?' = exactly one unit; no escapes.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] bool MatchesWildcard (FStringView Pattern) const noexcept;

	// ---- Structure / Unicode ----
	[[nodiscard]] bool IsAscii () const noexcept;
	// Strict: WTF-8 surrogate forms are invalid.
	[[nodiscard]] bool IsValidUtf8 () const noexcept;
	// Canonical WTF-8 (lone surrogates allowed, split pairs not).
	[[nodiscard]] bool IsValidWtf8 () const noexcept;
	[[nodiscard]] uint32 CountCodePoints () const noexcept;
	[[nodiscard]] constexpr FCodePointRange CodePoints () const noexcept FRT_LIFETIMEBOUND;
	// No unit spans Index. Index > Len() -> false.
	[[nodiscard]] constexpr bool IsCodePointBoundary (uint32 Index) const noexcept;
	// Index clamped to Len(); the start / the end of the unit spanning it.
	[[nodiscard]] constexpr uint32 FloorCodePointBoundary (uint32 Index) const noexcept;
	[[nodiscard]] constexpr uint32 CeilCodePointBoundary (uint32 Index) const noexcept;
	// Optional sign, ASCII digits with at most one '.', >= 1 digit; no exponent, no spaces.
	// IsNumeric("١") == false.
	[[nodiscard]] bool IsNumeric () const noexcept;
	// ASCII C identifier.
	[[nodiscard]] constexpr bool IsIdentifier () const noexcept;

	// ---- Split (results view this text) ----
	// Not found: {Left = whole, Right = empty at end, bFound = false}. Separator chars: ASCII (asserted);
	// code points: scalar values (asserted); separator views: non-empty (asserted; Release treats an empty
	// one as not found). The matched length is cut out (IgnoreCase: 'k' also splits at U+212A KELVIN SIGN).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr SStringSplit SplitFirst (char Separator) const noexcept FRT_LIFETIMEBOUND;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] SStringSplit SplitFirst (char32_t Separator) const noexcept FRT_LIFETIMEBOUND;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr SStringSplit SplitFirst (FStringView Separator) const noexcept FRT_LIFETIMEBOUND;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr SStringSplit SplitLast (char Separator) const noexcept FRT_LIFETIMEBOUND;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] SStringSplit SplitLast (char32_t Separator) const noexcept FRT_LIFETIMEBOUND;

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr SStringSplit SplitLast (FStringView Separator) const noexcept FRT_LIFETIMEBOUND;

	// Lazy token ranges; tokens = delimiters + 1 (empty text -> one empty token unless SkipEmpty).
	// The range stores the delimiter view: it must outlive the iteration too.
	// Delimiter: an ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr FStringSplitRange Split (char Delimiter, EStringSplit Options = EStringSplit::None) const noexcept FRT_LIFETIMEBOUND;
	// A code point: Split("ї"). The range keeps a view of its delimiter, and a code point argument has no
	// text that outlives the call.
	template <ESearchCase TCase = ESearchCase::CaseSensitive, typename T> requires std::same_as<T, char32_t>
	FStringSplitRange Split (T, EStringSplit = EStringSplit::None) const = delete;
	// Delimiter: non-empty (asserted; Release treats an empty one as never found: one token).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] constexpr FStringSplitRange Split (FStringView Delimiter FRT_LIFETIMEBOUND, EStringSplit Options = EStringSplit::None) const noexcept FRT_LIFETIMEBOUND;
	// Any unit of DelimiterSet separates (set rules of FindFirstOf: exact units, no case).
	[[nodiscard]] FStringSplitRange SplitAny (FStringView DelimiterSet FRT_LIFETIMEBOUND, EStringSplit Options = EStringSplit::None) const noexcept FRT_LIFETIMEBOUND;
	// Runs of White_Space; never yields empty tokens.
	[[nodiscard]] FStringSplitRange SplitWhitespace () const noexcept FRT_LIFETIMEBOUND;
	// Terminators "\r\n", "\n", "\r"; no empty line after a final terminator; "" -> no lines; "\n" -> one empty line.
	[[nodiscard]] constexpr FStringLineRange Lines (EStringSplit Options = EStringSplit::None) const noexcept FRT_LIFETIMEBOUND;

	// ---- Parsing: the whole text must be consumed; ASCII digits only (from_chars); optional leading '+';
	//      '-' only for signed and floating types; no whitespace; no "0x" (use Base 16); floats: '.' decimal
	//      point, exponent allowed, "inf"/"nan" accepted (callers that cannot take them check std::isfinite);
	//      Base 2..36 (integers only, asserted). Overflow -> nullopt. ----
	template <concepts::StringNumber T>
	[[nodiscard]] std::optional<T> TryParse (uint32 Base = 10) const noexcept;

	[[nodiscard]] std::optional<int32> ToInt32 (uint32 Base = 10) const noexcept;
	[[nodiscard]] std::optional<int64> ToInt64 (uint32 Base = 10) const noexcept;
	[[nodiscard]] std::optional<uint32> ToUInt32 (uint32 Base = 10) const noexcept;
	[[nodiscard]] std::optional<uint64> ToUInt64 (uint32 Base = 10) const noexcept;
	[[nodiscard]] std::optional<float> ToFloat () const noexcept;
	[[nodiscard]] std::optional<double> ToDouble () const noexcept;

	// true/1/yes/on, false/0/no/off, IgnoreCaseAscii. With a default, like every parse here:
	// TryParseBool().value_or(Default). Not named ToBool: `if (S.ToBool())` would read as "the text says
	// true", yet be true for "false" as well. The numeric To* are optionals too, and `if (S.ToInt32())`
	// asks whether the text is a number, not whether the number is non-zero.
	[[nodiscard]] std::optional<bool> TryParseBool () const noexcept;
	// Len() == 2 * Out.size(), ASCII hex digits of either case.
	[[nodiscard]] bool DecodeHex (std::span<uint8> Out) const noexcept;

	// ---- Conversion out ----
	// WTF-8 -> UTF-16 path; ill-formed -> U+FFFD; separators untouched.
	[[nodiscard]] std::filesystem::path ToPath () const noexcept;
	// nullopt unless canonical WTF-8: for text from hand-edited files that names a path.
	[[nodiscard]] std::optional<std::filesystem::path> TryToPath () const noexcept;
	// Third-party boundaries only (yaml-cpp).
	[[nodiscard]] std::string ToStdString () const;
	// For a third-party API that takes std::string_view. Explicit: engine text never turns into one
	// silently, so each such call is visible.
	constexpr explicit operator std::string_view () const noexcept;
	// Label-safe Truncate(BufferSize - 1) (FStringView::Truncate), then '\0'. Returns the bytes written
	// (no NUL). BufferSize == 0 writes nothing.
	uint32 CopyTo (char* Buffer, uint32 BufferSize) const noexcept;

	// ---- Hash (never persisted; values may change between versions) ----
	// CaseSensitive: hash::HashBytes(GetData(), Len()), the same for every string type; IgnoreCase /
	// IgnoreCaseAscii: consistent with Equals<TCase>.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] uint64 GetHash () const noexcept;

protected:
	// Haystack bytes matched by the affix (they differ from its length under IgnoreCase), or NotFound.
	template <ESearchCase TCase>
	[[nodiscard]] constexpr uint32 MatchPrefix (FStringView Prefix) const noexcept;

	template <ESearchCase TCase>
	[[nodiscard]] constexpr uint32 MatchSuffix (FStringView Suffix) const noexcept;

	// [OutBegin, OutEnd) of Mid(Start, Count) / Slice(Begin, End) of a text of InLen bytes, clamped.
	static constexpr void ClampMid (uint32 InLen, uint32 Start, uint32 Count, uint32& OutBegin, uint32& OutEnd) noexcept;
	static constexpr void ClampSlice (uint32 InLen, int64 Begin, int64 End, uint32& OutBegin, uint32& OutEnd) noexcept;

private:
	[[nodiscard]] constexpr const TDerived& Derived () const noexcept;
	[[nodiscard]] constexpr std::string_view AsStd () const noexcept;
};
}


namespace frt
{
/**
 * Non-owning UTF-8 text: pointer + length, not NUL-terminated (pass it to C APIs as (pointer, length),
 * or through FString / CopyTo). Never null: GetData() of an empty view is "", taken in the calling
 * module.
 *
 * Substrings clamp their arguments instead of asserting on range, and assert that every position they
 * cut at is a code-point boundary.
 */
class FStringView : public detail::TStringReadOps<FStringView>
{
public:
	using value_type = char;
	using size_type = uint32;
	using difference_type = int64;
	using iterator = const char*;
	using const_iterator = const char*;
	using reference = const char&;
	using const_reference = const char&;

	// Empty (Data == nullptr).
	constexpr FStringView () noexcept = default;

	// Implicit. A null pointer value -> empty. "" -> stores nullptr.
	constexpr FStringView (const char* NullTerminated FRT_LIFETIMEBOUND) noexcept
	{
		if (NullTerminated != nullptr)
		{
			const size_t length = std::char_traits<char>::length(NullTerminated);
			frt_assert(length <= MaxLength)
			if (length != 0u)
			{
				Data = NullTerminated;
				Size = static_cast<uint32>(length);
			}
		}
	}

	// The literal nullptr: write {} instead.
	FStringView (std::nullptr_t) = delete;

	// Exactly InLen bytes. InLen <= MaxLength (asserted). InData may be null only if InLen == 0; (p, 0) stores
	// nullptr. (nullptr, 0) is ambiguous by design: pass a typed pointer.
	constexpr FStringView (const char* InData FRT_LIFETIMEBOUND, size_t InLen) noexcept
	{
		frt_assert(InLen <= MaxLength)
		frt_assert(InData != nullptr || InLen == 0u)
		if (InLen != 0u)
		{
			Data = InData;
			Size = static_cast<uint32>(InLen);
		}
	}

	// Implicit (u8"" literals).
	FStringView (const char8_t* NullTerminated FRT_LIFETIMEBOUND) noexcept
		: FStringView(reinterpret_cast<const char*>(NullTerminated))
	{}

	FStringView (const char8_t* InData FRT_LIFETIMEBOUND, size_t InLen) noexcept
		: FStringView(reinterpret_cast<const char*>(InData), InLen)
	{}

	// From a contiguous char range: implicit for string-like types (std::string, std::string_view, FString),
	// explicit for raw byte containers (std::vector<char>, std::array<char, N>, std::span<const char>): a
	// char[64]-turned-std::array would otherwise become a 64-byte view including the trailing NULs.
	template <typename TRange> requires detail::ContiguousCharRange<TRange>
	explicit(!detail::StringLikeType<std::remove_cvref_t<TRange>>)
	constexpr FStringView (TRange&& Range FRT_LIFETIMEBOUND) noexcept
		: FStringView(static_cast<const char*>(std::data(Range)), static_cast<size_t>(std::size(Range)))
	{}

	// A view must not be assigned an owning temporary: `Desc.Name = FString::FromPath(P);` would dangle at the
	// `;`. Lvalues, literals, std::string_view and other borrowed ranges still assign; passing temporaries as
	// FStringView *parameters* is unaffected. (Aggregate initialization is not covered: never initialize an
	// FStringView member from a temporary.)
	// (ContiguousCharRange goes first: it rejects FStringView itself before borrowed_range would instantiate
	// enable_borrowed_range<FStringView> ahead of its specialization below.)
	template <typename TRange>
		requires (detail::ContiguousCharRange<TRange> && !std::is_lvalue_reference_v<TRange>
			&& !std::ranges::borrowed_range<TRange>)
	FStringView& operator= (TRange&&) = delete;

	// Never null; NOT NUL-terminated.
	[[nodiscard]] constexpr const char* GetData () const noexcept { return Data ? Data : ""; }
	[[nodiscard]] constexpr uint32 Len () const noexcept { return Size; }
	[[nodiscard]] constexpr bool IsEmpty () const noexcept { return Size == 0u; }

	// A byte; asserts Index < Len().
	[[nodiscard]] constexpr char operator[] (uint32 Index) const noexcept
	{
		frt_assert(Index < Size)
		return Data[Index];
	}

	// ---- Substrings: clamping (never asserting on range), boundary-asserted positions ----
	[[nodiscard]] constexpr FStringView Left (uint32 Count) const noexcept
	{
		const uint32 length = Count < Size ? Count : Size;
		FRT_STRING_ASSERT_BOUNDARY(length);
		return FStringView(Data, length);
	}

	// All but the last Count.
	[[nodiscard]] constexpr FStringView LeftChop (uint32 Count) const noexcept
	{
		const uint32 length = Size - (Count < Size ? Count : Size);
		FRT_STRING_ASSERT_BOUNDARY(length);
		return FStringView(Data, length);
	}

	[[nodiscard]] constexpr FStringView Right (uint32 Count) const noexcept
	{
		const uint32 start = Size - (Count < Size ? Count : Size);
		FRT_STRING_ASSERT_BOUNDARY(start);
		return FStringView(Data + start, Size - start);
	}

	// All but the first Count.
	[[nodiscard]] constexpr FStringView RightChop (uint32 Count) const noexcept
	{
		const uint32 start = Count < Size ? Count : Size;
		FRT_STRING_ASSERT_BOUNDARY(start);
		return FStringView(Data + start, Size - start);
	}

	[[nodiscard]] constexpr FStringView Mid (uint32 Start, uint32 Count = MaxLength) const noexcept
	{
		uint32 begin = 0u;
		uint32 end = 0u;
		ClampMid(Size, Start, Count, begin, end);
		FRT_STRING_ASSERT_BOUNDARY(begin);
		FRT_STRING_ASSERT_BOUNDARY(end);
		return FStringView(Data + begin, end - begin);
	}

	// Python slicing: negative = from the end; both clamped to [0, Len()]; End <= Begin -> empty.
	[[nodiscard]] constexpr FStringView Slice (int64 Begin, int64 End = SliceEnd) const noexcept
	{
		uint32 begin = 0u;
		uint32 end = 0u;
		ClampSlice(Size, Begin, End, begin, end);
		if (end <= begin)
		{
			return FStringView();
		}
		FRT_STRING_ASSERT_BOUNDARY(begin);
		FRT_STRING_ASSERT_BOUNDARY(end);
		return FStringView(Data + begin, end - begin);
	}

	// Longest prefix of at most MaxBytes that ends on a boundary and does not cut a visible sequence: a
	// base and its combining marks, a ZWJ emoji sequence, a variation selector, an emoji modifier or a tag
	// stay with what they modify, and a flag (two regional indicators) is kept whole or dropped whole. No
	// grapheme tables: the general category is enough for labels and fixed buffers.
	[[nodiscard]] FStringView Truncate (uint32 MaxBytes) const noexcept
	{
		return FStringView(Data, string_impl::TruncateLength(*this, MaxBytes));
	}

	// Unicode White_Space (ASCII fast path).
	[[nodiscard]] FStringView TrimStart () const noexcept
	{
		// Most text starts with a visible ASCII character: answered without a call.
		if (Size == 0u || (ascii::IsAscii(Data[0]) && !ascii::IsWhitespace(Data[0])))
		{
			return *this;
		}
		const uint32 count = string_impl::TrimStartLength(*this, FStringView());
		return FStringView(Data + count, Size - count);
	}

	[[nodiscard]] FStringView TrimEnd () const noexcept
	{
		if (Size == 0u || (ascii::IsAscii(Data[Size - 1u]) && !ascii::IsWhitespace(Data[Size - 1u])))
		{
			return *this;
		}
		return FStringView(Data, Size - string_impl::TrimEndLength(*this, FStringView()));
	}

	[[nodiscard]] FStringView TrimStartAndEnd () const noexcept
	{
		return TrimStart().TrimEnd();
	}

	// Any unit of CharSet (set rules of FindFirstOf). An empty CharSet trims nothing.
	[[nodiscard]] FStringView TrimStart (FStringView CharSet) const noexcept
	{
		if (CharSet.IsEmpty())
		{
			return *this;
		}
		const uint32 count = string_impl::TrimStartLength(*this, CharSet);
		return FStringView(Data + count, Size - count);
	}

	[[nodiscard]] FStringView TrimEnd (FStringView CharSet) const noexcept
	{
		if (CharSet.IsEmpty())
		{
			return *this;
		}
		return FStringView(Data, Size - string_impl::TrimEndLength(*this, CharSet));
	}

	[[nodiscard]] FStringView TrimStartAndEnd (FStringView CharSet) const noexcept
	{
		return TrimStart(CharSet).TrimEnd(CharSet);
	}

	// One pair: first and last byte equal and in QuoteChars, Len() >= 2. QuoteChars: ASCII only (asserted).
	[[nodiscard]] constexpr FStringView TrimQuotes (FStringView QuoteChars = "\"") const noexcept
	{
#if FRT_ASSERTIONS_ENABLED
		for (uint32 i = 0u; i < QuoteChars.Len(); ++i)
		{
			frt_assert(ascii::IsAscii(QuoteChars[i]))
		}
#endif
		if (Size >= 2u && Data[0] == Data[Size - 1u]
			&& std::char_traits<char>::find(QuoteChars.GetData(), QuoteChars.Len(), Data[0]) != nullptr)
		{
			return FStringView(Data + 1, Size - 2u);
		}
		return *this;
	}

	// ---- In-place (this view changes) ----
	constexpr FStringView& LeftInline (uint32 Count) noexcept { return *this = Left(Count); }
	constexpr FStringView& LeftChopInline (uint32 Count) noexcept { return *this = LeftChop(Count); }
	constexpr FStringView& RightInline (uint32 Count) noexcept { return *this = Right(Count); }
	constexpr FStringView& RightChopInline (uint32 Count) noexcept { return *this = RightChop(Count); }
	constexpr FStringView& MidInline (uint32 Start, uint32 Count = MaxLength) noexcept { return *this = Mid(Start, Count); }
	constexpr FStringView& SliceInline (int64 Begin, int64 End = SliceEnd) noexcept { return *this = Slice(Begin, End); }
	FStringView& TruncateInline (uint32 MaxBytes) noexcept { return *this = Truncate(MaxBytes); }
	FStringView& TrimStartInline () noexcept { return *this = TrimStart(); }
	FStringView& TrimEndInline () noexcept { return *this = TrimEnd(); }
	FStringView& TrimStartAndEndInline () noexcept { return *this = TrimStartAndEnd(); }
	FStringView& TrimStartInline (FStringView CharSet) noexcept { return *this = TrimStart(CharSet); }
	FStringView& TrimEndInline (FStringView CharSet) noexcept { return *this = TrimEnd(CharSet); }
	FStringView& TrimStartAndEndInline (FStringView CharSet) noexcept { return *this = TrimStartAndEnd(CharSet); }

	// Removes the affix when the view has it (the matched bytes: they can differ from its length under
	// IgnoreCase).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	constexpr bool RemoveFromStart (FStringView Prefix) noexcept
	{
		const uint32 matched = MatchPrefix<TCase>(Prefix);
		if (matched == NotFound)
		{
			return false;
		}
		*this = FStringView(Data + matched, Size - matched);
		return true;
	}

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	constexpr bool RemoveFromEnd (FStringView Suffix) noexcept
	{
		const uint32 matched = MatchSuffix<TCase>(Suffix);
		if (matched == NotFound)
		{
			return false;
		}
		*this = FStringView(Data, Size - matched);
		return true;
	}

	// ---- Cursor helpers for hand-written parsers ----
	// Text before the first Delimiter; the view advances past the matched delimiter (or becomes empty when
	// there is none). Delimiter: an ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	constexpr FStringView ConsumeToken (char Delimiter) noexcept
	{
		frt_assert(ascii::IsAscii(Delimiter))
		if constexpr (TCase == ESearchCase::IgnoreCase)
		{
			// The one-unit view: 'k' also matches U+212A KELVIN SIGN, which is longer than one byte.
			return ConsumeToken<TCase>(FStringView(&Delimiter, 1u));
		}
		else
		{
			return ConsumeTokenAt(Find<TCase>(Delimiter), 1u);
		}
	}

	// A scalar value (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	FStringView ConsumeToken (char32_t Delimiter) noexcept
	{
		frt_assert(unicode::IsScalarValue(Delimiter))
		char buffer[4] = {};
		const uint32 count = unicode::EncodeUtf8(Delimiter, buffer);
		return ConsumeToken<TCase>(FStringView(buffer, count));
	}

	// Delimiter: non-empty (asserted). Release treats an empty one as not found: the rest is the token, so
	// a `while (!Cursor.IsEmpty())` loop over a delimiter from a config value still ends.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	constexpr FStringView ConsumeToken (FStringView Delimiter) noexcept
	{
		frt_assert(!Delimiter.IsEmpty())
		if (Delimiter.IsEmpty())
		{
			return ConsumeTokenAt(NotFound, 0u);
		}
		const auto match = FindMatch<TCase>(Delimiter);   // SStringMatch: complete only further down
		return ConsumeTokenAt(match.Index, match.Len);
	}

	// An ASCII character (asserted). StartsWith(Ch) -> removed (the matched bytes), true.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	constexpr bool ConsumeChar (char Ch) noexcept
	{
		frt_assert(ascii::IsAscii(Ch))
		return RemoveFromStart<TCase>(FStringView(&Ch, 1u));
	}

	// A scalar value (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	bool ConsumeChar (char32_t CodePoint) noexcept
	{
		frt_assert(unicode::IsScalarValue(CodePoint))
		char buffer[4] = {};
		const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
		return RemoveFromStart<TCase>(FStringView(buffer, count));
	}

	// Longest numeric prefix (from_chars rules + optional '+', ASCII digits); advances on success; unchanged
	// + nullopt otherwise.
	template <concepts::StringNumber T>
	std::optional<T> ConsumeNumber (uint32 Base = 10) noexcept
	{
		uint32 consumed = 0u;
		const std::optional<T> value = detail::ParseNumber<T>(*this, Base, consumed);
		if (value)
		{
			*this = FStringView(Data + consumed, Size - consumed);
		}
		return value;
	}

	// ---- STL (data()/begin() == GetData(): never null) ----
	[[nodiscard]] constexpr const char* data () const noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] constexpr size_type size () const noexcept { return Size; }
	[[nodiscard]] constexpr size_type length () const noexcept { return Size; }
	[[nodiscard]] constexpr bool empty () const noexcept { return Size == 0u; }
	[[nodiscard]] constexpr const_iterator begin () const noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] constexpr const_iterator end () const noexcept FRT_LIFETIMEBOUND { return GetData() + Size; }

private:
	constexpr FStringView ConsumeTokenAt (uint32 Position, uint32 DelimiterLen) noexcept
	{
		if (Position == NotFound)
		{
			const FStringView token = *this;
			*this = FStringView();
			return token;
		}
		const FStringView token(Data, Position);
		*this = FStringView(Data + Position + DelimiterLen, Size - Position - DelimiterLen);
		return token;
	}

	const char* Data = nullptr;   // nullptr whenever Size == 0
	uint32 Size = 0u;
};

// ABI: inline code of the string types is compiled into game modules. A change of their layout or inline
// semantics bumps FRT_GAME_API_VERSION (Engine/GameModule.h), so a stale game DLL fails to load.
static_assert(sizeof(FStringView) == 16);
}

// A view, as std::string_view: ranges may keep iterators into it after it is gone. MSVC's <string_view>
// and <iterator> bring these traits and the borrowed_range concept above (its <xutility> defines them),
// so the string headers do without <ranges>, a far larger header that game modules (no PCH) would parse.
template <>
inline constexpr bool std::ranges::enable_borrowed_range<frt::FStringView> = true;
template <>
inline constexpr bool std::ranges::enable_view<frt::FStringView> = true;


namespace frt
{
/**
 * The type for names that must outlive their user and be readable as C strings: profiler scopes, view
 * and component names, enum reflection names, archive keys, combo tables.
 *
 * Immutable by type: it holds its own pointer and length and is not an FStringView (a base would let
 * `static_cast<FStringView&>(Lit) = RuntimeView` or a `Parse(FStringView& Cursor)` mutate it and break
 * CStr()). Converts to FStringView and to FString implicitly; substrings go through View().
 *
 * A non-empty literal of a game module dangles once that module unloads (like any const char* into
 * it); empty ones do not (they store nullptr).
 */
class FStringLiteral : public detail::TStringReadOps<FStringLiteral>
{
public:
	using value_type = char;
	using size_type = uint32;
	using const_iterator = const char*;

	// Empty.
	constexpr FStringLiteral () noexcept = default;

	// Only constant arrays: a string literal or a static constexpr char array. Copy-initialization from a
	// literal is an immediate invocation, so `FStringLiteral X = "a";`, arrays of them and FStringLiteral
	// parameters called with literals all work; a runtime const char* does not compile. Length = N - 1;
	// Literal[N - 1] must be '\0' (otherwise the constant evaluation fails). N == 1 stores nullptr.
	//
	// MSVC 14.38 rejects (C7595) some immediate invocations that are constant expressions:
	//	- a forwarded parameter inside a template that forwards arguments (std::pair's constructors): write
	//	  FStringLiteral(#Value) at the macro site, not inside the forwarding call;
	//	- a conditional operator choosing between two constructions, `b ? FStringLiteral("x") : FStringLiteral("yy")`;
	//	- braced lists of literals that become an initializer_list, `std::vector<FStringLiteral>{ "a", "b" }`,
	//	  `for (FStringLiteral L : { FStringLiteral("a"), FStringLiteral("b") })`.
	// Bind the literals to constexpr constants first and use those (`constexpr FStringLiteral X = "x";
	// return b ? X : Y;`); arrays, std::array, parameters, returns and members take literals directly.
	template <size_t N>
	consteval FStringLiteral (const char (&Literal)[N]) noexcept
		: Data(N > 1u ? Literal : nullptr)
		, Size(static_cast<uint32>(N - 1u))
	{
		static_assert(N - 1u <= MaxLength);
		if (Literal[N - 1u] != '\0')
		{
			detail::StringLiteralIsNotNulTerminated();
		}
	}

	[[nodiscard]] constexpr const char* GetData () const noexcept { return Data ? Data : ""; }
	// NUL-terminated, static.
	[[nodiscard]] constexpr const char* CStr () const noexcept { return GetData(); }
	[[nodiscard]] constexpr uint32 Len () const noexcept { return Size; }
	[[nodiscard]] constexpr bool IsEmpty () const noexcept { return Size == 0u; }

	// A byte; asserts Index < Len().
	[[nodiscard]] constexpr char operator[] (uint32 Index) const noexcept
	{
		frt_assert(Index < Size)
		return Data[Index];
	}

	// Implicit: a literal is usable wherever a view is.
	constexpr operator FStringView () const noexcept { return FStringView(Data, Size); }

	// ---- STL (read-only) ----
	[[nodiscard]] constexpr const char* data () const noexcept { return GetData(); }
	[[nodiscard]] constexpr size_type size () const noexcept { return Size; }
	[[nodiscard]] constexpr bool empty () const noexcept { return Size == 0u; }
	[[nodiscard]] constexpr const_iterator begin () const noexcept { return GetData(); }
	[[nodiscard]] constexpr const_iterator end () const noexcept { return GetData() + Size; }

private:
	const char* Data = nullptr;   // nullptr whenever Size == 0
	uint32 Size = 0u;
};

// ABI rule: see FStringView.
static_assert(sizeof(FStringLiteral) == 16);

inline constexpr FStringLiteral Utf8Bom = "\xEF\xBB\xBF";
// The INI trim set (ascii::IsWhitespace, Lua's %s): unlike Trim*(), keeps NBSP and U+3000.
inline constexpr FStringLiteral AsciiWhitespace = " \t\r\n\f\v";
}

template <>
inline constexpr bool std::ranges::enable_borrowed_range<frt::FStringLiteral> = true;


// Declared with the types, never in a separate header: an explicit specialization seen after an implicit
// instantiation in another TU is an ODR violation, and the library's range formatter would otherwise
// format a view as ['a', 'b']. Width and precision follow std::format for text: display width, code points.
template <>
struct std::formatter<frt::FStringView, char> : std::formatter<std::string_view, char>
{
	template <typename TContext>
	auto format (frt::FStringView Value, TContext& Context) const
	{
		return std::formatter<std::string_view, char>::format(std::string_view(Value.GetData(), Value.Len()), Context);
	}
};

template <>
struct std::formatter<frt::FStringLiteral, char> : std::formatter<frt::FStringView, char>
{
	template <typename TContext>
	auto format (frt::FStringLiteral Value, TContext& Context) const
	{
		return std::formatter<frt::FStringView, char>::format(frt::FStringView(Value), Context);
	}
};

// The same value as FString's and as GetHash() for the same bytes, so maps can mix the three.
template <>
struct std::hash<frt::FStringView>
{
	size_t operator() (frt::FStringView S) const noexcept { return static_cast<size_t>(S.GetHash()); }
};

template <>
struct std::hash<frt::FStringLiteral>
{
	size_t operator() (frt::FStringLiteral S) const noexcept { return static_cast<size_t>(S.GetHash()); }
};


namespace frt
{
// Usage: if (const SStringSplit kv = Line.SplitFirst('=')) { Key = kv.Left.TrimEnd(); ... }
struct SStringSplit
{
	FStringView Left;
	FStringView Right;
	bool bFound = false;

	constexpr explicit operator bool () const noexcept { return bFound; }
};

struct SStringMatch
{
	uint32 Index = FStringView::NotFound;   // haystack offset of the match
	uint32 Len = 0u;                        // haystack bytes matched (IgnoreCase: may differ from the needle's length)

	constexpr explicit operator bool () const noexcept { return Index != FStringView::NotFound; }
};


// Forward iterator over the units of a byte range: every well-formed code point, and every ill-formed
// maximal subpart as U+FFFD. Every GetByteIndex() is a code-point boundary and every boundary is one.
class FCodePointIterator
{
public:
	using iterator_concept = std::forward_iterator_tag;
	using iterator_category = std::input_iterator_tag;   // operator* returns a prvalue: not a Cpp17ForwardIterator
	using value_type = char32_t;
	using difference_type = int64;
	using reference = char32_t;
	using pointer = void;

	constexpr FCodePointIterator () noexcept = default;

	// Decoded; U+FFFD for an ill-formed unit.
	[[nodiscard]] constexpr char32_t operator* () const noexcept { return Current.CodePoint; }

	constexpr FCodePointIterator& operator++ () noexcept
	{
		Index += Current.Length;
		Decode();
		return *this;
	}

	constexpr FCodePointIterator operator++ (int) noexcept
	{
		const FCodePointIterator previous = *this;
		++*this;
		return previous;
	}

	// Offset from the range start (always a boundary).
	[[nodiscard]] constexpr uint32 GetByteIndex () const noexcept { return Index; }
	// Bytes of the current unit.
	[[nodiscard]] constexpr uint32 GetByteLength () const noexcept { return Current.Length; }
	[[nodiscard]] constexpr bool IsWellFormed () const noexcept { return Current.bWellFormed; }

	[[nodiscard]] friend constexpr bool operator== (const FCodePointIterator& A, const FCodePointIterator& B) noexcept
	{
		return A.Index == B.Index;
	}

private:
	friend class FCodePointRange;

	constexpr FCodePointIterator (const char* InData, uint32 InLen, uint32 InIndex) noexcept
		: Data(InData)
		, Size(InLen)
		, Index(InIndex)
	{
		Decode();
	}

	constexpr void Decode () noexcept
	{
		if (Index < Size)
		{
			Current = unicode::DecodeUtf8(Data + Index, Size - Index);
		}
	}

	const char* Data = nullptr;
	uint32 Size = 0u;
	uint32 Index = 0u;
	unicode::SDecodedCodePoint Current;
};

static_assert(std::forward_iterator<FCodePointIterator>);


class FCodePointRange
{
public:
	constexpr FCodePointRange () noexcept = default;

	constexpr FCodePointRange (const char* InData, uint32 InLen) noexcept
		: Data(InData)
		, Size(InLen)
	{}

	[[nodiscard]] constexpr FCodePointIterator begin () const noexcept { return FCodePointIterator(Data, Size, 0u); }
	[[nodiscard]] constexpr FCodePointIterator end () const noexcept { return FCodePointIterator(Data, Size, Size); }

private:
	const char* Data = nullptr;
	uint32 Size = 0u;
};


namespace detail
{
enum class EStringTokenMode : uint8
{
	Char,         // Split(char)
	View,         // Split(FStringView)
	AnySet,       // SplitAny
	Whitespace,   // SplitWhitespace
	Lines,        // Lines
};

/**
 * The state of one position in a token range: the current token and where the next search starts.
 * Iterators own a copy, so they stay valid when the range object that made them is gone (the source
 * text and the delimiter must not be).
 */
struct SStringTokenizer
{
	constexpr SStringTokenizer () noexcept = default;

	constexpr SStringTokenizer (FStringView InSource, FStringView InDelimiter, char InDelimiterChar,
		EStringTokenMode InMode, EStringSplit InOptions, ESearchCase InCase) noexcept
		: Source(InSource)
		, Delimiter(InDelimiter)
		, DelimiterChar(InDelimiterChar)
		, Mode(InMode)
		, Options(InOptions)
		, Case(InCase)
	{}

	[[nodiscard]] constexpr FStringView GetToken () const noexcept
	{
		return FStringView(Source.GetData() + TokenStart, TokenLen);
	}

	// Moves to the next token, or sets bDone.
	constexpr void Advance () noexcept
	{
		if (Mode == EStringTokenMode::Whitespace)
		{
			AdvanceWhitespace();
			return;
		}

		for (;;)
		{
			uint32 rawStart = NextStart;
			uint32 rawLen = 0u;
			if (Mode == EStringTokenMode::Lines)
			{
				// "" has no lines, and a final terminator does not start an empty one.
				if (NextStart >= Source.Len())
				{
					bDone = true;
					return;
				}
				const uint32 terminator = FindLineTerminator(NextStart);
				rawLen = terminator - rawStart;
				NextStart = terminator;
				if (terminator < Source.Len())
				{
					const bool bCrLf = Source[terminator] == '\r' && terminator + 1u < Source.Len() && Source[terminator + 1u] == '\n';
					NextStart += bCrLf ? 2u : 1u;
				}
			}
			else
			{
				// tokens = delimiters + 1: the token after the last delimiter exists even when it is empty.
				if (bLast)
				{
					bDone = true;
					return;
				}
				uint32 delimiterLen = 0u;
				const uint32 position = FindDelimiter(NextStart, delimiterLen);
				if (position == FStringView::NotFound)
				{
					rawLen = Source.Len() - rawStart;
					NextStart = Source.Len();
					bLast = true;
				}
				else
				{
					rawLen = position - rawStart;
					NextStart = position + delimiterLen;
				}
			}

			TokenStart = rawStart;
			TokenLen = rawLen;
			if (HasFlag(Options, EStringSplit::Trim))
			{
				const FStringView trimmed = GetToken().TrimStartAndEnd();
				TokenLen = trimmed.Len();
				if (!trimmed.IsEmpty())
				{
					TokenStart = static_cast<uint32>(trimmed.GetData() - Source.GetData());
				}
			}

			if (TokenLen != 0u || !HasFlag(Options, EStringSplit::SkipEmpty))
			{
				return;
			}
		}
	}

	[[nodiscard]] friend constexpr bool operator== (const SStringTokenizer& A, const SStringTokenizer& B) noexcept
	{
		// Positions only: the source and the delimiter are the same for iterators of one range. NextStart
		// tells tokens apart even when they are empty, bLast the final empty token from the one before it.
		return A.bDone == B.bDone
			&& (A.bDone || (A.TokenStart == B.TokenStart && A.NextStart == B.NextStart && A.bLast == B.bLast));
	}

	FStringView Source;
	FStringView Delimiter;      // View and AnySet modes
	uint32 TokenStart = 0u;
	uint32 TokenLen = 0u;
	uint32 NextStart = 0u;      // where the search for the next token starts
	char DelimiterChar = '\0';  // Char mode
	EStringTokenMode Mode = EStringTokenMode::Char;
	EStringSplit Options = EStringSplit::None;
	ESearchCase Case = ESearchCase::CaseSensitive;   // Char and View modes
	bool bLast = false;         // the current token is the last one (split modes)
	bool bDone = false;         // past the last token

private:
	// Position of the next delimiter at or after From (and its length), or NotFound.
	constexpr uint32 FindDelimiter (uint32 From, uint32& OutLen) const noexcept
	{
		switch (Mode)
		{
		case EStringTokenMode::Char:
			// IgnoreCase: as the one-unit view ('k' also finds U+212A KELVIN SIGN, three bytes long).
			if (Case == ESearchCase::IgnoreCase)
			{
				return FindNeedle(FStringView(&DelimiterChar, 1u), From, OutLen);
			}
			OutLen = 1u;
			return Case == ESearchCase::IgnoreCaseAscii ? Source.Find<ESearchCase::IgnoreCaseAscii>(DelimiterChar, From)
				: Source.Find(DelimiterChar, From);
		case EStringTokenMode::View:
			// An empty delimiter (asserted by Split) is never found: one token, never an endless run of empty ones.
			if (Delimiter.IsEmpty())
			{
				return FStringView::NotFound;
			}
			return FindNeedle(Delimiter, From, OutLen);
		default:
		{
			const uint32 position = string_impl::FindInSet(Source, Delimiter, From, false);
			if (position != FStringView::NotFound)
			{
				OutLen = unicode::DecodeUtf8(Source.GetData() + position, Source.Len() - position).Length;
			}
			return position;
		}
		}
	}

	// Needle at or after From under Case, with the haystack bytes it matched (IgnoreCase: they can differ
	// from Needle.Len()), or NotFound.
	constexpr uint32 FindNeedle (FStringView Needle, uint32 From, uint32& OutLen) const noexcept
	{
		SStringMatch match;
		switch (Case)
		{
		case ESearchCase::IgnoreCase:
			match = Source.FindMatch<ESearchCase::IgnoreCase>(Needle, From);
			break;
		case ESearchCase::IgnoreCaseAscii:
			match = Source.FindMatch<ESearchCase::IgnoreCaseAscii>(Needle, From);
			break;
		default:
			match = Source.FindMatch(Needle, From);
			break;
		}
		OutLen = match.Len;
		return match.Index;
	}

	constexpr uint32 FindLineTerminator (uint32 From) const noexcept
	{
		for (uint32 i = From; i < Source.Len(); ++i)
		{
			if (Source[i] == '\n' || Source[i] == '\r')
			{
				return i;
			}
		}
		return Source.Len();
	}

	// Skips a run of White_Space, then takes the run up to the next one. An ill-formed unit is never
	// whitespace.
	void AdvanceWhitespace () noexcept
	{
		const char* data = Source.GetData();
		const uint32 length = Source.Len();

		uint32 position = NextStart;
		bool bInToken = false;
		uint32 start = position;
		while (position < length)
		{
			const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(data + position, length - position);
			const bool bWhitespace = unit.bWellFormed && unicode::IsWhitespace(unit.CodePoint);
			if (!bInToken && !bWhitespace)
			{
				bInToken = true;
				start = position;
			}
			else if (bInToken && bWhitespace)
			{
				break;
			}
			position += unit.Length;
		}

		if (!bInToken)
		{
			bDone = true;
			NextStart = length;
			return;
		}
		TokenStart = start;
		TokenLen = position - start;
		NextStart = position;
	}
};


/**
 * The token ranges' shared body (Split, SplitAny, SplitWhitespace, Lines): lazy, allocation-free,
 * forward. A range views its source text and its delimiter; both must outlive the iteration.
 */
class FStringTokenRange
{
public:
	// Yields FStringView; the end is std::default_sentinel_t.
	class FIterator
	{
	public:
		using iterator_concept = std::forward_iterator_tag;
		using iterator_category = std::input_iterator_tag;
		using value_type = FStringView;
		using difference_type = int64;
		using reference = FStringView;
		using pointer = void;

		constexpr FIterator () noexcept = default;

		[[nodiscard]] constexpr FStringView operator* () const noexcept { return State.GetToken(); }

		constexpr FIterator& operator++ () noexcept
		{
			State.Advance();
			return *this;
		}

		constexpr FIterator operator++ (int) noexcept
		{
			const FIterator previous = *this;
			State.Advance();
			return previous;
		}

		[[nodiscard]] friend constexpr bool operator== (const FIterator& A, const FIterator& B) noexcept
		{
			return A.State == B.State;
		}

		[[nodiscard]] friend constexpr bool operator== (const FIterator& A, std::default_sentinel_t) noexcept
		{
			return A.State.bDone;
		}

	private:
		friend class FStringTokenRange;

		constexpr explicit FIterator (const SStringTokenizer& Start) noexcept
			: State(Start)
		{
			State.Advance();
		}

		SStringTokenizer State;
	};

	constexpr FStringTokenRange () noexcept = default;

	constexpr explicit FStringTokenRange (const SStringTokenizer& InStart) noexcept
		: Start(InStart)
	{}

	[[nodiscard]] constexpr FIterator begin () const noexcept { return FIterator(Start); }
	[[nodiscard]] constexpr std::default_sentinel_t end () const noexcept { return std::default_sentinel; }

	// Fills a container with every token: uses Add() when the container has it (TArray), else push_back().
	// value_type is constructed explicitly from FStringView, so TArray<FString> / std::vector<FString> work.
	// The default is std::vector: usable before the engine's memory pool exists.
	template <typename TContainer = std::vector<FStringView>>
	[[nodiscard]] TContainer Collect () const
	{
		using TValue = typename TContainer::value_type;

		TContainer result;
		for (const FStringView token : *this)
		{
			if constexpr (requires (TContainer& Container, TValue&& Value) { Container.Add(std::move(Value)); })
			{
				result.Add(TValue(token));
			}
			else
			{
				result.push_back(TValue(token));
			}
		}
		return result;
	}

	[[nodiscard]] uint32 Count () const noexcept
	{
		uint32 count = 0u;
		for (FIterator it = begin(); it != end(); ++it)
		{
			++count;
		}
		return count;
	}

private:
	SStringTokenizer Start;
};
}


// Split / SplitAny / SplitWhitespace.
class FStringSplitRange : public detail::FStringTokenRange
{
public:
	using detail::FStringTokenRange::FStringTokenRange;
};

static_assert(std::forward_iterator<FStringSplitRange::FIterator>);
static_assert(std::sentinel_for<std::default_sentinel_t, FStringSplitRange::FIterator>);


// Lines.
class FStringLineRange : public detail::FStringTokenRange
{
public:
	using detail::FStringTokenRange::FStringTokenRange;
};

static_assert(std::forward_iterator<FStringLineRange::FIterator>);
static_assert(std::sentinel_for<std::default_sentinel_t, FStringLineRange::FIterator>);


namespace detail
{
template <concepts::StringNumber T>
std::optional<T> ParseNumber (FStringView Text, uint32 Base, uint32& OutConsumed) noexcept
{
	if constexpr (std::is_floating_point_v<T>)
	{
		frt_assert(Base == 10u)
		(void)Base;
		if constexpr (std::is_same_v<T, float>)
		{
			float value = 0.0f;
			if (string_impl::ParseFloat(Text, value, OutConsumed))
			{
				return value;
			}
		}
		else
		{
			double value = 0.0;
			if (string_impl::ParseDouble(Text, value, OutConsumed))
			{
				return static_cast<T>(value);
			}
		}
	}
	else if constexpr (std::is_signed_v<T>)
	{
		int64 value = 0;
		if (string_impl::ParseSigned(Text, Base, value, OutConsumed)
			&& value >= static_cast<int64>((std::numeric_limits<T>::min)())
			&& value <= static_cast<int64>((std::numeric_limits<T>::max)()))
		{
			return static_cast<T>(value);
		}
	}
	else
	{
		uint64 value = 0u;
		if (string_impl::ParseUnsigned(Text, Base, value, OutConsumed)
			&& value <= static_cast<uint64>((std::numeric_limits<T>::max)()))
		{
			return static_cast<T>(value);
		}
	}
	return std::nullopt;
}


// The boundary rules on raw bytes, shared with StringView.cpp. Read at most 4 bytes.

// Offset Index is a boundary iff it is 0, Len, or no unit spans it: byte Index is not a continuation
// byte, or the unit of the nearest lead within the 3 bytes before it ends at or before Index (when there
// is no such lead, byte Index is a stray continuation byte: a unit of its own).
[[nodiscard]] constexpr bool IsCodePointBoundaryAt (const char* Data, uint32 Len, uint32 Index) noexcept
{
	if (Index == 0u || Index == Len)
	{
		return true;
	}
	if (Index > Len)
	{
		return false;
	}
	if (!unicode::IsContinuationByte(Data[Index]))
	{
		return true;
	}

	const uint32 lowest = Index >= 3u ? Index - 3u : 0u;
	for (uint32 lead = Index; lead-- > lowest;)
	{
		if (!unicode::IsContinuationByte(Data[lead]))
		{
			return lead + unicode::DecodeUtf8(Data + lead, Len - lead).Length <= Index;
		}
	}
	return true;
}

// The start of the unit spanning Index (Index itself when it is a boundary); Index clamped to Len.
[[nodiscard]] constexpr uint32 FloorCodePointBoundaryAt (const char* Data, uint32 Len, uint32 Index) noexcept
{
	if (Index >= Len)
	{
		return Len;
	}
	if (!unicode::IsContinuationByte(Data[Index]))
	{
		return Index;
	}

	const uint32 lowest = Index >= 3u ? Index - 3u : 0u;
	for (uint32 lead = Index; lead-- > lowest;)
	{
		if (!unicode::IsContinuationByte(Data[lead]))
		{
			return lead + unicode::DecodeUtf8(Data + lead, Len - lead).Length <= Index ? Index : lead;
		}
	}
	return Index;
}

// The end of the unit spanning Index (Index itself when it is a boundary); Index clamped to Len.
[[nodiscard]] constexpr uint32 CeilCodePointBoundaryAt (const char* Data, uint32 Len, uint32 Index) noexcept
{
	if (Index >= Len)
	{
		return Len;
	}
	if (!unicode::IsContinuationByte(Data[Index]))
	{
		return Index;
	}

	const uint32 lowest = Index >= 3u ? Index - 3u : 0u;
	for (uint32 lead = Index; lead-- > lowest;)
	{
		if (!unicode::IsContinuationByte(Data[lead]))
		{
			const uint32 end = lead + unicode::DecodeUtf8(Data + lead, Len - lead).Length;
			return end <= Index ? Index : end;
		}
	}
	return Index;
}

// A-Z folded to a-z, every other byte exact.
[[nodiscard]] constexpr bool EqualBytesIgnoreCaseAscii (const char* A, const char* B, uint32 Count) noexcept
{
	for (uint32 i = 0u; i < Count; ++i)
	{
		if (ascii::ToLower(A[i]) != ascii::ToLower(B[i]))
		{
			return false;
		}
	}
	return true;
}
}


// ---- Comparison: byte-exact. Covers FString, FStringView, FStringLiteral, const char*, std::string(_view). ----

[[nodiscard]] constexpr bool operator== (FStringView A, FStringView B) noexcept
{
	return A.Len() == B.Len() && std::char_traits<char>::compare(A.GetData(), B.GetData(), A.Len()) == 0;
}

// Unsigned bytes: code-point order on well-formed text.
[[nodiscard]] constexpr std::strong_ordering operator<=> (FStringView A, FStringView B) noexcept
{
	return std::string_view(A.GetData(), A.Len()).compare(std::string_view(B.GetData(), B.Len())) <=> 0;
}


namespace literals
{
inline namespace string_literals
{
// Exact length, embedded NULs included: "a\0b"_sv.Len() == 3.
// constexpr, not consteval: MSVC 14.38 rejects immediate invocations inside braced lists and conditional
// operators (C7595 on `std::vector<FStringView>{ "a"_sv }`, `b ? "x"_sv : "y"_sv`), and an operator only
// literals can call gains nothing from consteval.
[[nodiscard]] constexpr FStringView operator""_sv (const char* Text, size_t Len) noexcept
{
	return FStringView(Text, Len);
}
}
}


// Writes the bytes.
FRT_CORE_API std::ostream& operator<< (std::ostream& Out, FStringView Text);

// gtest: quoted and escaped (FString::Escape). Exact `const T&` signatures, so they beat gtest's own
// PrintTo(const T&, ...) template, which would print the types as containers of chars.
FRT_CORE_API void PrintTo (const FStringView& Text, std::ostream* Out);
FRT_CORE_API void PrintTo (const FStringLiteral& Text, std::ostream* Out);
}


// ---- TStringReadOps definitions ----

namespace frt::detail
{
template <typename TDerived>
constexpr const TDerived& TStringReadOps<TDerived>::Derived () const noexcept
{
	return static_cast<const TDerived&>(*this);
}

template <typename TDerived>
constexpr std::string_view TStringReadOps<TDerived>::AsStd () const noexcept
{
	return std::string_view(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
constexpr FStringView TStringReadOps<TDerived>::View () const noexcept
{
	return FStringView(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
template <math::SIndexStrategy::EType TIndex>
constexpr bool TStringReadOps<TDerived>::IsIndexValid (int64 Index) const noexcept
{
	return IndexStrategy::IsValid<TIndex>(Index, static_cast<int64>(Derived().Len()));
}

template <typename TDerived>
template <math::SIndexStrategy::EType TIndex>
constexpr char TStringReadOps<TDerived>::Get (int64 Index) const noexcept
{
	frt_assert(IsIndexValid<TIndex>(Index))
	return Derived().GetData()[IndexStrategy::ConvertToDefault<TIndex>(Index, static_cast<uint64>(Derived().Len()))];
}

template <typename TDerived>
constexpr char TStringReadOps<TDerived>::First () const noexcept
{
	frt_assert(Derived().Len() != 0u)
	return Derived().GetData()[0];
}

template <typename TDerived>
constexpr char TStringReadOps<TDerived>::Last () const noexcept
{
	frt_assert(Derived().Len() != 0u)
	return Derived().GetData()[Derived().Len() - 1u];
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::Equals (FStringView Other) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		return AsStd() == std::string_view(Other.GetData(), Other.Len());
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		// Equal length is a valid shortcut here: ASCII folding never changes a length.
		return Derived().Len() == Other.Len() && EqualBytesIgnoreCaseAscii(Derived().GetData(), Other.GetData(), Other.Len());
	}
	else
	{
		return string_impl::EqualsFolded(View(), Other, TCase);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr int32 TStringReadOps<TDerived>::Compare (FStringView Other) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		const int result = AsStd().compare(std::string_view(Other.GetData(), Other.Len()));
		return (result > 0) - (result < 0);
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		const char* data = Derived().GetData();
		const uint32 length = Derived().Len();
		const uint32 common = length < Other.Len() ? length : Other.Len();
		for (uint32 i = 0u; i < common; ++i)
		{
			const unsigned char a = static_cast<unsigned char>(ascii::ToLower(data[i]));
			const unsigned char b = static_cast<unsigned char>(ascii::ToLower(Other.GetData()[i]));
			if (a != b)
			{
				return a < b ? -1 : 1;
			}
		}
		return (length > Other.Len()) - (length < Other.Len());
	}
	else
	{
		return string_impl::CompareFolded(View(), Other, TCase);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr uint32 TStringReadOps<TDerived>::Find (FStringView Needle, uint32 Start) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		if (Start > Derived().Len())
		{
			return NotFound;
		}
		const size_t found = AsStd().find(std::string_view(Needle.GetData(), Needle.Len()), Start);
		return found == std::string_view::npos ? NotFound : static_cast<uint32>(found);
	}
	else
	{
		return string_impl::FindFolded(View(), Needle, Start, TCase).Index;
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr SStringMatch TStringReadOps<TDerived>::FindMatch (FStringView Needle, uint32 Start) const noexcept
{
	if constexpr (TCase == ESearchCase::IgnoreCase)
	{
		return string_impl::FindFolded(View(), Needle, Start, TCase);
	}
	else
	{
		const uint32 index = Find<TCase>(Needle, Start);
		return index == NotFound ? SStringMatch() : SStringMatch{ index, Needle.Len() };
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr uint32 TStringReadOps<TDerived>::Find (char Ch, uint32 Start) const noexcept
{
	frt_assert(ascii::IsAscii(Ch))
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		if (Start > Derived().Len())
		{
			return NotFound;
		}
		const size_t found = AsStd().find(Ch, Start);
		return found == std::string_view::npos ? NotFound : static_cast<uint32>(found);
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		const char* data = Derived().GetData();
		const char lower = ascii::ToLower(Ch);
		for (uint32 i = Start; i < Derived().Len(); ++i)
		{
			if (ascii::ToLower(data[i]) == lower)
			{
				return i;
			}
		}
		return NotFound;
	}
	else
	{
		// Exactly the one-unit view: 'k' also finds U+212A KELVIN SIGN, 's' U+017F LONG S.
		return Find<TCase>(FStringView(&Ch, 1u), Start);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
uint32 TStringReadOps<TDerived>::Find (char32_t CodePoint, uint32 Start) const noexcept
{
	frt_assert(unicode::IsScalarValue(CodePoint))
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
	return Find<TCase>(FStringView(buffer, count), Start);
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr uint32 TStringReadOps<TDerived>::FindLast (FStringView Needle, uint32 End) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		const uint32 end = End < Derived().Len() ? End : Derived().Len();
		const size_t found = std::string_view(Derived().GetData(), end).rfind(std::string_view(Needle.GetData(), Needle.Len()));
		return found == std::string_view::npos ? NotFound : static_cast<uint32>(found);
	}
	else
	{
		return string_impl::FindLastFolded(View(), Needle, End, TCase).Index;
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr SStringMatch TStringReadOps<TDerived>::FindLastMatch (FStringView Needle, uint32 End) const noexcept
{
	if constexpr (TCase == ESearchCase::IgnoreCase)
	{
		return string_impl::FindLastFolded(View(), Needle, End, TCase);
	}
	else
	{
		const uint32 index = FindLast<TCase>(Needle, End);
		return index == NotFound ? SStringMatch() : SStringMatch{ index, Needle.Len() };
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr uint32 TStringReadOps<TDerived>::FindLast (char Ch, uint32 End) const noexcept
{
	frt_assert(ascii::IsAscii(Ch))
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		const uint32 end = End < Derived().Len() ? End : Derived().Len();
		const size_t found = std::string_view(Derived().GetData(), end).rfind(Ch);
		return found == std::string_view::npos ? NotFound : static_cast<uint32>(found);
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		const char* data = Derived().GetData();
		const char lower = ascii::ToLower(Ch);
		for (uint32 i = End < Derived().Len() ? End : Derived().Len(); i-- > 0u;)
		{
			if (ascii::ToLower(data[i]) == lower)
			{
				return i;
			}
		}
		return NotFound;
	}
	else
	{
		return FindLast<TCase>(FStringView(&Ch, 1u), End);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
uint32 TStringReadOps<TDerived>::FindLast (char32_t CodePoint, uint32 End) const noexcept
{
	frt_assert(unicode::IsScalarValue(CodePoint))
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
	return FindLast<TCase>(FStringView(buffer, count), End);
}

template <typename TDerived>
uint32 TStringReadOps<TDerived>::FindFirstOf (FStringView CharSet, uint32 Start) const noexcept
{
	return string_impl::FindInSet(View(), CharSet, Start, false);
}

template <typename TDerived>
uint32 TStringReadOps<TDerived>::FindFirstNotOf (FStringView CharSet, uint32 Start) const noexcept
{
	return string_impl::FindInSet(View(), CharSet, Start, true);
}

template <typename TDerived>
uint32 TStringReadOps<TDerived>::FindLastOf (FStringView CharSet, uint32 End) const noexcept
{
	return string_impl::FindLastInSet(View(), CharSet, End, false);
}

template <typename TDerived>
uint32 TStringReadOps<TDerived>::FindLastNotOf (FStringView CharSet, uint32 End) const noexcept
{
	return string_impl::FindLastInSet(View(), CharSet, End, true);
}

template <typename TDerived>
template <typename TPredicate>
uint32 TStringReadOps<TDerived>::FindIf (TPredicate&& Pred, uint32 Start) const
{
	const char* data = Derived().GetData();
	const uint32 length = Derived().Len();
	for (uint32 i = Start; i < length;)
	{
		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(data + i, length - i);
		if (Pred(unit.CodePoint))
		{
			return i;
		}
		i += unit.Length;
	}
	return NotFound;
}

template <typename TDerived>
template <typename TPredicate>
uint32 TStringReadOps<TDerived>::FindLastIf (TPredicate&& Pred, uint32 End) const
{
	// Backwards by boundaries, which segment exactly like forward decoding does: the unit before a
	// boundary starts at the floor boundary of its last byte.
	const char* data = Derived().GetData();
	const uint32 length = Derived().Len();
	uint32 end = End < length ? End : length;
	while (end > 0u)
	{
		const uint32 start = FloorCodePointBoundaryAt(data, length, end - 1u);
		if (Pred(unicode::DecodeUtf8(data + start, length - start).CodePoint))
		{
			return start;
		}
		end = start;
	}
	return NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
uint32 TStringReadOps<TDerived>::FindAny (std::span<const FStringView> Needles, uint32 Start, uint32* OutNeedleIndex) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		uint32 best = NotFound;
		uint32 bestNeedle = NotFound;
		for (size_t i = 0u; i < Needles.size(); ++i)
		{
			if (Needles[i].IsEmpty())
			{
				continue;
			}
			// Strictly earlier only: a tie keeps the lower needle index.
			const uint32 found = Find(Needles[i], Start);
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
	else
	{
		return string_impl::FindAny(View(), Needles, Start, TCase, OutNeedleIndex);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
uint32 TStringReadOps<TDerived>::FindAny (std::initializer_list<FStringView> Needles, uint32 Start, uint32* OutNeedleIndex) const noexcept
{
	return FindAny<TCase>(std::span<const FStringView>(Needles.begin(), Needles.size()), Start, OutNeedleIndex);
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::Contains (FStringView Needle) const noexcept
{
	return Find<TCase>(Needle) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::Contains (char Ch) const noexcept
{
	return Find<TCase>(Ch) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
bool TStringReadOps<TDerived>::Contains (char32_t CodePoint) const noexcept
{
	return Find<TCase>(CodePoint) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::StartsWith (FStringView Prefix) const noexcept
{
	return MatchPrefix<TCase>(Prefix) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::StartsWith (char Ch) const noexcept
{
	frt_assert(ascii::IsAscii(Ch))
	if constexpr (TCase == ESearchCase::IgnoreCase)
	{
		return MatchPrefix<TCase>(FStringView(&Ch, 1u)) != NotFound;
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		return Derived().Len() != 0u && ascii::ToLower(Derived().GetData()[0]) == ascii::ToLower(Ch);
	}
	else
	{
		return Derived().Len() != 0u && Derived().GetData()[0] == Ch;
	}
}

template <typename TDerived>
template <ESearchCase TCase>
bool TStringReadOps<TDerived>::StartsWith (char32_t CodePoint) const noexcept
{
	frt_assert(unicode::IsScalarValue(CodePoint))
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
	return MatchPrefix<TCase>(FStringView(buffer, count)) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::EndsWith (FStringView Suffix) const noexcept
{
	return MatchSuffix<TCase>(Suffix) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr bool TStringReadOps<TDerived>::EndsWith (char Ch) const noexcept
{
	frt_assert(ascii::IsAscii(Ch))
	if constexpr (TCase == ESearchCase::IgnoreCase)
	{
		return MatchSuffix<TCase>(FStringView(&Ch, 1u)) != NotFound;
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		return Derived().Len() != 0u && ascii::ToLower(Derived().GetData()[Derived().Len() - 1u]) == ascii::ToLower(Ch);
	}
	else
	{
		return Derived().Len() != 0u && Derived().GetData()[Derived().Len() - 1u] == Ch;
	}
}

template <typename TDerived>
template <ESearchCase TCase>
bool TStringReadOps<TDerived>::EndsWith (char32_t CodePoint) const noexcept
{
	frt_assert(unicode::IsScalarValue(CodePoint))
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
	return MatchSuffix<TCase>(FStringView(buffer, count)) != NotFound;
}

template <typename TDerived>
template <ESearchCase TCase>
uint32 TStringReadOps<TDerived>::CountOccurrences (FStringView Needle) const noexcept
{
	if (Needle.IsEmpty())
	{
		return 0u;
	}

	uint32 count = 0u;
	uint32 start = 0u;
	for (;;)
	{
		const SStringMatch match = FindMatch<TCase>(Needle, start);
		if (!match)
		{
			return count;
		}
		++count;
		start = match.Index + match.Len;
	}
}

template <typename TDerived>
template <ESearchCase TCase>
bool TStringReadOps<TDerived>::MatchesWildcard (FStringView Pattern) const noexcept
{
	return string_impl::MatchesWildcard(View(), Pattern, TCase);
}

template <typename TDerived>
bool TStringReadOps<TDerived>::IsAscii () const noexcept
{
	return unicode::IsAscii(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
bool TStringReadOps<TDerived>::IsValidUtf8 () const noexcept
{
	return unicode::IsValidUtf8(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
bool TStringReadOps<TDerived>::IsValidWtf8 () const noexcept
{
	return unicode::IsValidWtf8(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
uint32 TStringReadOps<TDerived>::CountCodePoints () const noexcept
{
	return unicode::CountCodePoints(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
constexpr FCodePointRange TStringReadOps<TDerived>::CodePoints () const noexcept
{
	return FCodePointRange(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
constexpr bool TStringReadOps<TDerived>::IsCodePointBoundary (uint32 Index) const noexcept
{
	return IsCodePointBoundaryAt(Derived().GetData(), Derived().Len(), Index);
}

template <typename TDerived>
constexpr uint32 TStringReadOps<TDerived>::FloorCodePointBoundary (uint32 Index) const noexcept
{
	return FloorCodePointBoundaryAt(Derived().GetData(), Derived().Len(), Index);
}

template <typename TDerived>
constexpr uint32 TStringReadOps<TDerived>::CeilCodePointBoundary (uint32 Index) const noexcept
{
	return CeilCodePointBoundaryAt(Derived().GetData(), Derived().Len(), Index);
}

template <typename TDerived>
bool TStringReadOps<TDerived>::IsNumeric () const noexcept
{
	const char* data = Derived().GetData();
	const uint32 length = Derived().Len();

	uint32 i = 0u;
	if (i < length && (data[i] == '+' || data[i] == '-'))
	{
		++i;
	}

	bool bDigit = false;
	bool bPoint = false;
	for (; i < length; ++i)
	{
		if (ascii::IsDigit(data[i]))
		{
			bDigit = true;
		}
		else if (data[i] == '.' && !bPoint)
		{
			bPoint = true;
		}
		else
		{
			return false;
		}
	}
	return bDigit;
}

template <typename TDerived>
constexpr bool TStringReadOps<TDerived>::IsIdentifier () const noexcept
{
	const char* data = Derived().GetData();
	const uint32 length = Derived().Len();
	if (length == 0u || !ascii::IsIdentifierStart(data[0]))
	{
		return false;
	}
	for (uint32 i = 1u; i < length; ++i)
	{
		if (!ascii::IsIdentifierChar(data[i]))
		{
			return false;
		}
	}
	return true;
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr SStringSplit TStringReadOps<TDerived>::SplitFirst (char Separator) const noexcept
{
	frt_assert(ascii::IsAscii(Separator))
	if constexpr (TCase == ESearchCase::IgnoreCase)
	{
		// The one-unit view: 'k' also matches U+212A KELVIN SIGN, which is longer than one byte.
		return SplitFirst<TCase>(FStringView(&Separator, 1u));
	}
	else
	{
		const char* data = Derived().GetData();
		const uint32 length = Derived().Len();
		const uint32 position = Find<TCase>(Separator);
		if (position == NotFound)
		{
			return SStringSplit{ FStringView(data, length), FStringView(), false };
		}
		return SStringSplit{ FStringView(data, position), FStringView(data + position + 1, length - position - 1u), true };
	}
}

template <typename TDerived>
template <ESearchCase TCase>
SStringSplit TStringReadOps<TDerived>::SplitFirst (char32_t Separator) const noexcept
{
	frt_assert(unicode::IsScalarValue(Separator))
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(Separator, buffer);
	return SplitFirst<TCase>(FStringView(buffer, count));
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr SStringSplit TStringReadOps<TDerived>::SplitFirst (FStringView Separator) const noexcept
{
	frt_assert(!Separator.IsEmpty())
	const char* data = Derived().GetData();
	const uint32 length = Derived().Len();
	// An empty separator (asserted) is never found, as for Split.
	const SStringMatch match = Separator.IsEmpty() ? SStringMatch() : FindMatch<TCase>(Separator);
	if (!match)
	{
		return SStringSplit{ FStringView(data, length), FStringView(), false };
	}
	const uint32 rest = match.Index + match.Len;
	return SStringSplit{ FStringView(data, match.Index), FStringView(data + rest, length - rest), true };
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr SStringSplit TStringReadOps<TDerived>::SplitLast (char Separator) const noexcept
{
	frt_assert(ascii::IsAscii(Separator))
	if constexpr (TCase == ESearchCase::IgnoreCase)
	{
		return SplitLast<TCase>(FStringView(&Separator, 1u));
	}
	else
	{
		const char* data = Derived().GetData();
		const uint32 length = Derived().Len();
		const uint32 position = FindLast<TCase>(Separator);
		if (position == NotFound)
		{
			return SStringSplit{ FStringView(data, length), FStringView(), false };
		}
		return SStringSplit{ FStringView(data, position), FStringView(data + position + 1, length - position - 1u), true };
	}
}

template <typename TDerived>
template <ESearchCase TCase>
SStringSplit TStringReadOps<TDerived>::SplitLast (char32_t Separator) const noexcept
{
	frt_assert(unicode::IsScalarValue(Separator))
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(Separator, buffer);
	return SplitLast<TCase>(FStringView(buffer, count));
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr SStringSplit TStringReadOps<TDerived>::SplitLast (FStringView Separator) const noexcept
{
	frt_assert(!Separator.IsEmpty())
	const char* data = Derived().GetData();
	const uint32 length = Derived().Len();
	const SStringMatch match = Separator.IsEmpty() ? SStringMatch() : FindLastMatch<TCase>(Separator);
	if (!match)
	{
		return SStringSplit{ FStringView(data, length), FStringView(), false };
	}
	const uint32 rest = match.Index + match.Len;
	return SStringSplit{ FStringView(data, match.Index), FStringView(data + rest, length - rest), true };
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr FStringSplitRange TStringReadOps<TDerived>::Split (char Delimiter, EStringSplit Options) const noexcept
{
	frt_assert(ascii::IsAscii(Delimiter))
	return FStringSplitRange(SStringTokenizer(View(), FStringView(), Delimiter, EStringTokenMode::Char, Options, TCase));
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr FStringSplitRange TStringReadOps<TDerived>::Split (FStringView Delimiter, EStringSplit Options) const noexcept
{
	frt_assert(!Delimiter.IsEmpty())
	return FStringSplitRange(SStringTokenizer(View(), Delimiter, '\0', EStringTokenMode::View, Options, TCase));
}

template <typename TDerived>
FStringSplitRange TStringReadOps<TDerived>::SplitAny (FStringView DelimiterSet, EStringSplit Options) const noexcept
{
	return FStringSplitRange(SStringTokenizer(View(), DelimiterSet, '\0', EStringTokenMode::AnySet, Options, ESearchCase::CaseSensitive));
}

template <typename TDerived>
FStringSplitRange TStringReadOps<TDerived>::SplitWhitespace () const noexcept
{
	return FStringSplitRange(SStringTokenizer(View(), FStringView(), '\0', EStringTokenMode::Whitespace, EStringSplit::None,
		ESearchCase::CaseSensitive));
}

template <typename TDerived>
constexpr FStringLineRange TStringReadOps<TDerived>::Lines (EStringSplit Options) const noexcept
{
	return FStringLineRange(SStringTokenizer(View(), FStringView(), '\0', EStringTokenMode::Lines, Options, ESearchCase::CaseSensitive));
}

template <typename TDerived>
template <concepts::StringNumber T>
std::optional<T> TStringReadOps<TDerived>::TryParse (uint32 Base) const noexcept
{
	uint32 consumed = 0u;
	const std::optional<T> value = ParseNumber<T>(View(), Base, consumed);
	if (!value || consumed != Derived().Len())
	{
		return std::nullopt;
	}
	return value;
}

template <typename TDerived>
std::optional<int32> TStringReadOps<TDerived>::ToInt32 (uint32 Base) const noexcept
{
	return TryParse<int32>(Base);
}

template <typename TDerived>
std::optional<int64> TStringReadOps<TDerived>::ToInt64 (uint32 Base) const noexcept
{
	return TryParse<int64>(Base);
}

template <typename TDerived>
std::optional<uint32> TStringReadOps<TDerived>::ToUInt32 (uint32 Base) const noexcept
{
	return TryParse<uint32>(Base);
}

template <typename TDerived>
std::optional<uint64> TStringReadOps<TDerived>::ToUInt64 (uint32 Base) const noexcept
{
	return TryParse<uint64>(Base);
}

template <typename TDerived>
std::optional<float> TStringReadOps<TDerived>::ToFloat () const noexcept
{
	return TryParse<float>();
}

template <typename TDerived>
std::optional<double> TStringReadOps<TDerived>::ToDouble () const noexcept
{
	return TryParse<double>();
}

template <typename TDerived>
std::optional<bool> TStringReadOps<TDerived>::TryParseBool () const noexcept
{
	constexpr ESearchCase Case = ESearchCase::IgnoreCaseAscii;
	if (Equals<Case>("true") || Equals<Case>("1") || Equals<Case>("yes") || Equals<Case>("on"))
	{
		return true;
	}
	if (Equals<Case>("false") || Equals<Case>("0") || Equals<Case>("no") || Equals<Case>("off"))
	{
		return false;
	}
	return std::nullopt;
}

template <typename TDerived>
bool TStringReadOps<TDerived>::DecodeHex (std::span<uint8> Out) const noexcept
{
	return string_impl::DecodeHex(View(), Out);
}

template <typename TDerived>
std::filesystem::path TStringReadOps<TDerived>::ToPath () const noexcept
{
	std::filesystem::path path;
	string_impl::ToPath(View(), false, path);
	return path;
}

template <typename TDerived>
std::optional<std::filesystem::path> TStringReadOps<TDerived>::TryToPath () const noexcept
{
	std::filesystem::path path;
	if (!string_impl::ToPath(View(), true, path))
	{
		return std::nullopt;
	}
	return path;
}

template <typename TDerived>
std::string TStringReadOps<TDerived>::ToStdString () const
{
	return std::string(Derived().GetData(), Derived().Len());
}

template <typename TDerived>
constexpr TStringReadOps<TDerived>::operator std::string_view () const noexcept
{
	return AsStd();
}

template <typename TDerived>
uint32 TStringReadOps<TDerived>::CopyTo (char* Buffer, uint32 BufferSize) const noexcept
{
	if (BufferSize == 0u)
	{
		return 0u;
	}

	const uint32 count = string_impl::TruncateLength(View(), BufferSize - 1u);
	if (count != 0u)
	{
		std::memcpy(Buffer, Derived().GetData(), count);
	}
	Buffer[count] = '\0';
	return count;
}

template <typename TDerived>
template <ESearchCase TCase>
uint64 TStringReadOps<TDerived>::GetHash () const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		return hash::HashBytes(Derived().GetData(), Derived().Len());
	}
	else
	{
		return string_impl::HashFolded(View(), TCase);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr uint32 TStringReadOps<TDerived>::MatchPrefix (FStringView Prefix) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		return AsStd().starts_with(std::string_view(Prefix.GetData(), Prefix.Len())) ? Prefix.Len() : NotFound;
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		return Derived().Len() >= Prefix.Len() && EqualBytesIgnoreCaseAscii(Derived().GetData(), Prefix.GetData(), Prefix.Len())
			? Prefix.Len()
			: NotFound;
	}
	else
	{
		return string_impl::MatchPrefixFolded(View(), Prefix, TCase);
	}
}

template <typename TDerived>
template <ESearchCase TCase>
constexpr uint32 TStringReadOps<TDerived>::MatchSuffix (FStringView Suffix) const noexcept
{
	if constexpr (TCase == ESearchCase::CaseSensitive)
	{
		return AsStd().ends_with(std::string_view(Suffix.GetData(), Suffix.Len())) ? Suffix.Len() : NotFound;
	}
	else if constexpr (TCase == ESearchCase::IgnoreCaseAscii)
	{
		const uint32 length = Derived().Len();
		return length >= Suffix.Len()
			&& EqualBytesIgnoreCaseAscii(Derived().GetData() + (length - Suffix.Len()), Suffix.GetData(), Suffix.Len())
			? Suffix.Len()
			: NotFound;
	}
	else
	{
		return string_impl::MatchSuffixFolded(View(), Suffix, TCase);
	}
}

template <typename TDerived>
constexpr void TStringReadOps<TDerived>::ClampMid (uint32 InLen, uint32 Start, uint32 Count, uint32& OutBegin, uint32& OutEnd) noexcept
{
	OutBegin = Start < InLen ? Start : InLen;
	const uint32 available = InLen - OutBegin;
	OutEnd = OutBegin + (Count < available ? Count : available);
}

template <typename TDerived>
constexpr void TStringReadOps<TDerived>::ClampSlice (uint32 InLen, int64 Begin, int64 End, uint32& OutBegin, uint32& OutEnd) noexcept
{
	// MaxLength keeps every intermediate value far from int64's limits.
	const int64 length = static_cast<int64>(InLen);
	const int64 begin = Begin < 0 ? Begin + length : Begin;
	const int64 end = End < 0 ? End + length : End;
	OutBegin = static_cast<uint32>(begin < 0 ? 0 : (begin > length ? length : begin));
	OutEnd = static_cast<uint32>(end < 0 ? 0 : (end > length ? length : end));
}
}
