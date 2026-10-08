#pragma once

#include "CoreTypes.h"


/**
 * Locale-free ASCII character classes and case mapping for single bytes.
 *
 * The replacement for std::isspace, std::isdigit, std::tolower and friends, which have three
 * problems this has not: they take int and are undefined for a negative char (every byte of a
 * non-ASCII UTF-8 sequence, with signed char), they consult the C locale, and they are not
 * constexpr. Every function here answers false (or returns the byte unchanged) for bytes >= 0x80,
 * so they can be run over UTF-8 text without ever touching or splitting a multi-byte sequence.
 *
 * Code points are frt::unicode's business (Unicode/Unicode.h); text sets such as AsciiWhitespace
 * live with the string types (Containers/StringView.h).
 */
namespace frt::ascii
{
[[nodiscard]] constexpr bool IsAscii (char C) noexcept
{
	return static_cast<unsigned char>(C) < 0x80u;
}

// ' ' \t \n \v \f \r: the C locale's isspace, and what Lua's %s matches (Premake/project.lua reads
// the same INI files with it).
[[nodiscard]] constexpr bool IsWhitespace (char C) noexcept
{
	return C == ' ' || (C >= '\t' && C <= '\r');
}

[[nodiscard]] constexpr bool IsDigit (char C) noexcept
{
	return C >= '0' && C <= '9';
}

[[nodiscard]] constexpr bool IsHexDigit (char C) noexcept
{
	return IsDigit(C) || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F');
}

[[nodiscard]] constexpr bool IsUpper (char C) noexcept
{
	return C >= 'A' && C <= 'Z';
}

[[nodiscard]] constexpr bool IsLower (char C) noexcept
{
	return C >= 'a' && C <= 'z';
}

[[nodiscard]] constexpr bool IsAlpha (char C) noexcept
{
	return IsUpper(C) || IsLower(C);
}

[[nodiscard]] constexpr bool IsAlnum (char C) noexcept
{
	return IsAlpha(C) || IsDigit(C);
}

// 0x00-0x1F and 0x7F. The C1 controls (U+0080..U+009F) are two-byte sequences in UTF-8: see
// unicode::IsControl.
[[nodiscard]] constexpr bool IsControl (char C) noexcept
{
	const unsigned char byte = static_cast<unsigned char>(C);
	return byte < 0x20u || byte == 0x7Fu;
}

// 0x20-0x7E: space and the visible characters.
[[nodiscard]] constexpr bool IsPrintable (char C) noexcept
{
	const unsigned char byte = static_cast<unsigned char>(C);
	return byte >= 0x20u && byte < 0x7Fu;
}

// What may start / continue a C identifier (enum reflection names, INI keys, shader macros).
[[nodiscard]] constexpr bool IsIdentifierStart (char C) noexcept
{
	return IsAlpha(C) || C == '_';
}

[[nodiscard]] constexpr bool IsIdentifierChar (char C) noexcept
{
	return IsAlnum(C) || C == '_';
}

// a-z only; every other byte, including each byte of a UTF-8 sequence, is returned unchanged.
[[nodiscard]] constexpr char ToUpper (char C) noexcept
{
	return IsLower(C) ? static_cast<char>(C - ('a' - 'A')) : C;
}

// A-Z only.
[[nodiscard]] constexpr char ToLower (char C) noexcept
{
	return IsUpper(C) ? static_cast<char>(C + ('a' - 'A')) : C;
}

// 0..15 for a hex digit of either case, -1 for anything else.
[[nodiscard]] constexpr int32 HexValue (char C) noexcept
{
	if (IsDigit(C))
	{
		return C - '0';
	}
	if (C >= 'a' && C <= 'f')
	{
		return C - 'a' + 10;
	}
	if (C >= 'A' && C <= 'F')
	{
		return C - 'A' + 10;
	}
	return -1;
}

// The digit of Nibble & 15. Computed rather than indexed into "0123456789ABCDEF": no table, so it
// costs nothing in constant evaluation and needs no storage in any module.
[[nodiscard]] constexpr char HexDigit (uint32 Nibble, bool bUpperCase = true) noexcept
{
	const uint32 value = Nibble & 15u;
	if (value < 10u)
	{
		return static_cast<char>('0' + value);
	}
	return static_cast<char>((bUpperCase ? 'A' : 'a') + (value - 10u));
}
}
