#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

#include "CoreTypes.h"


/**
 * What every string header shares, and what a header that only names the string types needs:
 * the lifetime attribute, forward declarations, the small enums and the concepts behind the
 * conversion rules.
 *
 * Text in the engine is UTF-8 in char (Containers/String.h): FString owns it, FStringView borrows
 * it, FStringLiteral names static text, FWideString carries UTF-16 to the OS.
 */

// Catches views bound to temporaries (C26815/C26816 under /analyze; clang -Wdangling).
#if defined(__has_cpp_attribute) && __has_cpp_attribute(msvc::lifetimebound)
	#define FRT_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif defined(__has_cpp_attribute) && __has_cpp_attribute(clang::lifetimebound)
	#define FRT_LIFETIMEBOUND [[clang::lifetimebound]]
#else
	#define FRT_LIFETIMEBOUND
#endif


namespace frt
{
class FString;
class FStringView;
class FStringLiteral;
class FWideString;

// Every function that looks for or compares a text or a character takes this as a template parameter
// defaulting to CaseSensitive: Equals, Compare, the Find / Contains / StartsWith / EndsWith families,
// Split, SplitFirst / SplitLast, ConsumeToken / ConsumeChar, RemoveFromStart / RemoveFromEnd, RemoveAll,
// Replace, CountOccurrences, MatchesWildcard, GetHash. Exact by design: the code-point set functions
// (FindFirstOf and its family, SplitAny, Trim(CharSet), TrimQuotes), FString::ReplaceCharInline (bytes),
// the == / <=> operators and SStringHash / SStringEqual (caseless maps: StringMap.h).
enum class ESearchCase : uint8
{
	CaseSensitive,     // byte-exact
	IgnoreCase,        // Unicode simple case folding (CaseFolding.txt C+S), ASCII fast path; locale-free
	IgnoreCaseAscii,   // A-Z == a-z, every other byte exact. INI files, command line, enum names: the rules
	                   // Premake/project.lua implements with Lua's string.lower
};

enum class EStringSplit : uint8
{
	None      = 0,
	SkipEmpty = 1u << 0,   // drop empty tokens (after Trim, when both are set)
	Trim      = 1u << 1,   // trim Unicode White_Space from each token
};

[[nodiscard]] constexpr EStringSplit operator| (EStringSplit A, EStringSplit B) noexcept
{
	return static_cast<EStringSplit>(static_cast<uint8>(A) | static_cast<uint8>(B));
}

[[nodiscard]] constexpr EStringSplit operator& (EStringSplit A, EStringSplit B) noexcept
{
	return static_cast<EStringSplit>(static_cast<uint8>(A) & static_cast<uint8>(B));
}

[[nodiscard]] constexpr bool HasFlag (EStringSplit Flags, EStringSplit Flag) noexcept
{
	return (Flags & Flag) == Flag;
}

// Separators FString::FromPath writes. Paths themselves accept both.
enum class EPathStyle : uint8
{
	Native,      // path::native() as stored
	Preferred,   // '\\' (as path::make_preferred: how Windows and its dialogs show paths)
	Generic,     // '/'  (paths stored in assets and config)
};

// platform::ReadFileText
enum class ETextFileEncoding : uint8
{
	Detect,   // EF BB BF stripped; FF FE / FE FF -> UTF-16LE/BE transcoded (WTF-8); anything else as is
	Raw,      // bytes as stored
};


namespace concepts
{
// Arithmetic but not text: bool and the character types are excluded (plain char is text, int8/uint8
// are numbers).
template <typename T>
concept StringNumber = std::is_arithmetic_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>
	&& !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t>
	&& !std::is_same_v<T, char32_t>;

// Values that must never reach a char/bool parameter by a standard conversion (deleted overloads use it).
template <typename T>
concept NonTextScalar = (std::is_arithmetic_v<T> || std::is_enum_v<T>) && !std::is_same_v<T, char>
	&& !std::is_same_v<T, char32_t>;
}


namespace detail
{
template <typename T>
struct TIsStdString : std::false_type
{};

template <typename TAlloc>
struct TIsStdString<std::basic_string<char, std::char_traits<char>, TAlloc>> : std::true_type
{};

// Contiguous char storage that is text by construction: converts to FStringView implicitly.
template <typename T>
concept StringLikeType = TIsStdString<T>::value || std::is_same_v<T, std::string_view> || std::is_same_v<T, FString>;

// Any contiguous sized range of char. Excludes C arrays (they decay to const char* -> strlen) and the view
// types themselves (copy / FStringLiteral's conversion operator). Only needs the forward declarations above.
template <typename T>
concept ContiguousCharRange =
	!std::is_array_v<std::remove_cvref_t<T>>
	&& !std::is_same_v<std::remove_cvref_t<T>, FStringView>
	&& !std::is_same_v<std::remove_cvref_t<T>, FStringLiteral>
	&& requires (T&& R)
	{
		{ std::data(R) } -> std::convertible_to<const char*>;
		{ std::size(R) } -> std::convertible_to<size_t>;
	}
	&& std::is_same_v<std::remove_cvref_t<decltype(*std::data(std::declval<T&>()))>, char>;
}
}
