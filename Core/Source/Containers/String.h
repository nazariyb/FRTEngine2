#pragma once

#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "Containers/StringView.h"
#include "Core.h"
#include "CoreTypes.h"


/**
 * FString: owned UTF-8 text, always NUL-terminated, with 22 bytes inline (small string optimization).
 *
 * Narrow text is UTF-8 everywhere in the engine (every project builds with /utf-8, and every executable
 * runs with the UTF-8 process code page), so string literals convert implicitly; UTF-16 exists only at
 * the OS call (FromWide / ToWide, FromPath / ToPath). The read-only API is TStringReadOps, shared with
 * FStringView and FStringLiteral (Containers/StringView.h).
 *
 *	- Memory: every block comes from string_detail::Allocate, the CRT heap of Core's runtime - never the
 *	  engine's memory pool, which is not thread-safe, does not exist yet when the first strings are made
 *	  and is gone before static destructors run. Strings cross Core.dll, the game DLL and hot reloads.
 *	- Moves, destruction, short copies and appends that fit are header-inline: they are compiled into
 *	  every module, so TArray<FString> relocation never calls into the DLL.
 *	- Every mutating function behaves as if its arguments were copied before the string changes:
 *	  S.Append(S), S.InsertAt(2, S.View()) and S = S.CStr() + 2 are all fine.
 *	- Everything is noexcept except Format / AppendFormat (user formatters may throw): running out of
 *	  memory or past MaxLength ends the process (string_detail::FatalAllocationError / FatalLengthError).
 */

namespace frt::string_detail
{
// CRT heap of Core's runtime. Every FString/FWideString block, whichever module's inline code asked,
// is allocated and freed here: strings cross Core.dll, the game DLL and hot reloads freely.
[[nodiscard]] FRT_CORE_API void* Allocate (size_t Bytes) noexcept;                  // never null (fatal on failure)
[[nodiscard]] FRT_CORE_API void* Reallocate (void* Block, size_t Bytes) noexcept;    // never null (fatal on failure)
FRT_CORE_API void Free (void* Block) noexcept;                                      // null-safe
[[noreturn]] FRT_CORE_API void FatalLengthError () noexcept;
[[noreturn]] FRT_CORE_API void FatalAllocationError (size_t Bytes) noexcept;
// Allocate/Reallocate calls made by the calling thread since it started, for tests: other threads
// (job workers, async texture decode) cannot disturb it.
[[nodiscard]] FRT_CORE_API uint64 GetThreadHeapAllocationCount () noexcept;

inline constexpr uint64 MaxLength = 0x7FFFFFFFu;

// Usable bytes of a block for Count bytes of text: blocks are multiples of 16 bytes (the CRT's
// granularity), one of which is the terminator.
[[nodiscard]] constexpr uint64 RoundCapacity (uint64 Count) noexcept
{
	return ((Count + 1u + 15u) & ~static_cast<uint64>(15u)) - 1u;
}

// Capacity after growing a string of capacity Capacity to hold Required bytes (Required <= MaxLength):
// the TArray factor 1.5, clamped to MaxLength; leaving the inline buffer goes to at least 33 bytes, so
// the first growth past 22 gives 47. Computed in uint64: near MaxLength the factor would overflow uint32.
[[nodiscard]] constexpr uint32 GetGrowCapacity (uint32 Capacity, uint64 Required, bool bInline) noexcept
{
	const uint64 grown = bInline ? 33u : static_cast<uint64>(Capacity) + Capacity / 2u;
	const uint64 wanted = Required > grown ? Required : grown;
	return static_cast<uint32>(RoundCapacity(wanted < MaxLength ? wanted : MaxLength));
}

// Narrows a computed length, or ends the process when it exceeds MaxLength.
[[nodiscard]] inline uint32 CheckLength (uint64 Length) noexcept
{
	if (Length > MaxLength)
	{
		FatalLengthError();
	}
	return static_cast<uint32>(Length);
}

// A Printf/Appendf argument as it goes through "...". C varargs promote unscoped enums but pass scoped
// ones unchanged, and %d reading an `enum class E : uint8` is undefined: every enum goes as its
// underlying value, promoted by the unary +.
template <typename T>
[[nodiscard]] constexpr auto PrintfArgument (T Value) noexcept
{
	if constexpr (std::is_enum_v<T>)
	{
		return +std::to_underlying(Value);
	}
	else
	{
		return Value;
	}
}
}


namespace frt
{
/**
 * Owned UTF-8 text with small string optimization. See the top of this file.
 *
 * Layout (24 bytes; all-zero bytes are a valid empty string): either 22 bytes of text + terminator
 * inline with the length in the last byte, or a heap block pointer, length and capacity with bit 7 of
 * the last byte set. Lengths and positions are bytes; positions that split a code point assert.
 *
 * Names: TArray's where TArray has the operation (Clear, Reset, Free, ShrinkToFit, GetCapacity, IsIndexValid,
 * Get, RemoveAt), UE FString's otherwise (Reserve, which only grows; InsertAt(Index, Text), index first,
 * unlike TArray::Insert; Left, Mid, RightChop), and Resize / ResizeUninitialized for std::string's resize.
 * Counts and positions are bytes. Substrings, trims and case conversions have three forms each: X() const&
 * copies, X() && reuses this buffer, XInline() mutates and returns *this.
 */
class FString : public detail::TStringReadOps<FString>
{
public:
	using value_type = char;
	using size_type = uint32;
	using difference_type = int64;
	using iterator = char*;
	using const_iterator = const char*;
	using reference = char&;
	using const_reference = const char&;

	static constexpr uint32 InlineCapacity = 22u;

	// ---- Construction / assignment ----
	// Empty, inline, all-zero storage: namespace-scope FStrings are constant-initialized.
	constexpr FString () noexcept
		: Storage{}
	{}

	// Inline source: 24 bytes copied. Heap source: an exact-size block of our own.
	FString (const FString& Other) noexcept
		: Storage{}
	{
		if (Other.IsHeapAllocated())
		{
			CopyFrom(Other);
		}
		else
		{
			Storage = Other.Storage;
		}
	}

	// Other becomes empty.
	FString (FString&& Other) noexcept
		: Storage(Other.Storage)
	{
		Other.Storage = UStorage{};
	}

	// Reuses this string's block when it is large enough.
	FString& operator= (const FString& Other) noexcept
	{
		if (this == &Other)
		{
			return *this;
		}

		if (Other.IsHeapAllocated())
		{
			CopyFrom(Other);
		}
		else if (IsHeapAllocated())
		{
			// Inline text always fits a heap block: keep the block, copy the bytes and the terminator.
			const uint32 length = Other.Storage.Inline.Tag;
			std::memcpy(Storage.Heap.Data, Other.Storage.Inline.Data, length + 1u);
			Storage.Heap.Len = length;
		}
		else
		{
			Storage = Other.Storage;
		}
		return *this;
	}

	FString& operator= (FString&& Other) noexcept
	{
		if (this != &Other)
		{
			if (IsHeapAllocated())
			{
				string_detail::Free(Storage.Heap.Data);
			}
			Storage = Other.Storage;
			Other.Storage = UStorage{};
		}
		return *this;
	}

	~FString ()
	{
		if (IsHeapAllocated())
		{
			string_detail::Free(Storage.Heap.Data);
		}
	}

	// Implicit; a null pointer value -> empty.
	FString (const char* NullTerminated) noexcept
		: Storage{}
	{
		if (NullTerminated != nullptr)
		{
			InitBytes(NullTerminated, std::strlen(NullTerminated));
		}
	}

	// The literal nullptr: say FString() instead.
	FString (std::nullptr_t) = delete;

	// Exactly InLen bytes (embedded NULs kept); a typed pointer for (null, 0).
	FString (const char* InData, size_t InLen) noexcept
		: Storage{}
	{
		frt_assert(InData != nullptr || InLen == 0u)
		InitBytes(InData, InLen);
	}

	// Implicit (u8"" literals).
	FString (const char8_t* NullTerminated) noexcept
		: FString(reinterpret_cast<const char*>(NullTerminated))
	{}

	FString (const char8_t* InData, size_t InLen) noexcept
		: FString(reinterpret_cast<const char*>(InData), InLen)
	{}

	// Implicit: static text, like const char*.
	FString (FStringLiteral Literal) noexcept
		: FString(Literal.GetData(), Literal.Len())
	{}

	// Explicit: the allocation is visible. Covers std::string, std::string_view and every other text range.
	explicit FString (FStringView Text) noexcept
		: FString(Text.GetData(), Text.Len())
	{}

	// Alias-safe: S = S.CStr() + 2.
	FString& operator= (const char* NullTerminated) noexcept
	{
		if (NullTerminated == nullptr)
		{
			Clear();
			return *this;
		}
		AssignText(NullTerminated, std::strlen(NullTerminated));
		return *this;
	}

	// S = u8"x" (else ambiguous).
	FString& operator= (const char8_t* NullTerminated) noexcept
	{
		return *this = reinterpret_cast<const char*>(NullTerminated);
	}

	// S = Lit (else ambiguous).
	FString& operator= (FStringLiteral Literal) noexcept
	{
		AssignText(Literal.GetData(), Literal.Len());
		return *this;
	}

	// Alias-safe: Text may view this string.
	FString& operator= (FStringView Text) noexcept
	{
		AssignText(Text.GetData(), Text.Len());
		return *this;
	}

	// S.Clear() / S = FString(). Note: `S = {}` is ambiguous (several pointer overloads); write S.Clear().
	FString& operator= (std::nullptr_t) = delete;

	// ---- Named constructors ----
	// UTF-16 -> canonical WTF-8: a lone surrogate keeps its 3-byte form, so path -> FString -> path is
	// lossless. nullptr -> empty.
	[[nodiscard]] FRT_CORE_API static FString FromWide (const wchar_t* NullTerminated) noexcept;
	// InLen in UTF-16 units; a result over MaxLength is fatal.
	[[nodiscard]] FRT_CORE_API static FString FromWide (const wchar_t* InData, size_t InLen) noexcept;
	[[nodiscard]] FRT_CORE_API static FString FromWide (std::wstring_view Utf16) noexcept;
	// The path's native UTF-16 as canonical WTF-8, separators per Style. The only producer of path keys
	// (TPathMap): byte concatenation of pieces does not re-canonicalize surrogate halves.
	[[nodiscard]] FRT_CORE_API static FString FromPath (const std::filesystem::path& Path, EPathStyle Style = EPathStyle::Native) noexcept;
	// Narrow text never becomes a path implicitly (path(const char*) decodes with the code page): deleted.
	static FString FromPath (const char*, EPathStyle = EPathStyle::Native) = delete;
	static FString FromPath (const std::string&, EPathStyle = EPathStyle::Native) = delete;
	static FString FromPath (std::string_view, EPathStyle = EPathStyle::Native) = delete;
	// Ill-formed sequences -> U+FFFD (one per maximal subpart): third-party diagnostics shown in UI,
	// exception messages.
	[[nodiscard]] FRT_CORE_API static FString FromUtf8Lossy (FStringView Bytes) noexcept;

	// Integers: decimal; floats: shortest round-trip (to_chars, may use an exponent).
	template <concepts::StringNumber T>
	[[nodiscard]] static FString FromNumber (T Value) noexcept
	{
		FString result;
		result.AppendNumber(Value);
		return result;
	}

	// "true" / "false"
	[[nodiscard]] static FString FromBool (bool Value) noexcept
	{
		return Value ? FString("true", 4u) : FString("false", 5u);
	}

	// Fixed notation, the shortest digits that round-trip T, at least MinFractionalDigits; "-0" -> "0.0";
	// nan/inf/-inf.
	template <std::floating_point T>
	[[nodiscard]] static FString SanitizeFloat (T Value, uint32 MinFractionalDigits = 1) noexcept
	{
		FString result;
		result.AppendFixed(static_cast<double>(Value), MinFractionalDigits, std::is_same_v<T, float>);
		return result;
	}

	[[nodiscard]] FRT_CORE_API static FString FromHexBytes (std::span<const uint8> Bytes, bool bUpperCase = true) noexcept;

	// C-style: \\ \" \n \r \t \0; \xHH for: other bytes < 0x20 and 0x7F, every byte of an ill-formed unit,
	// and every byte of well-formed code points of category Cc, Zl, Zp or in {U+061C, U+200E, U+200F,
	// U+202A..202E, U+2066..2069} (C1 controls, line/paragraph separators and bidi controls can hide or
	// reorder text in terminals and test logs). Other well-formed non-ASCII is kept.
	[[nodiscard]] FRT_CORE_API static FString Escape (FStringView Text) noexcept;
	// Inverse of Escape (also accepts \'); an unknown escape, a bad \xHH or a trailing backslash -> nullopt.
	// A static of FString, not a read op: StringView.h cannot define functions returning FString.
	[[nodiscard]] FRT_CORE_API static std::optional<FString> Unescape (FStringView Text) noexcept;
	// Length checked in uint64.
	[[nodiscard]] FRT_CORE_API static FString Repeat (FStringView Text, uint32 Count) noexcept;

	// Elements convertible to FStringView.
	template <typename TRange>
	[[nodiscard]] static FString Join (const TRange& Parts, FStringView Separator) noexcept;

	// Projection(element) returns anything Append accepts (FStringView-convertible, FString, char).
	template <typename TRange, typename TProjection>
	[[nodiscard]] static FString JoinBy (const TRange& Parts, FStringView Separator, TProjection&& Projection);

	// std::format into a new string. Width and precision are std::format's: display width (CJK = 2
	// columns) and code points.
	template <typename... TArgs>
	[[nodiscard]] static FString Format (std::format_string<TArgs...> Fmt, TArgs&&... Args);

	// C printf, only where std::format cannot express the format. Width and precision are bytes: never
	// %.Ns on text that may be cut inside a code point (Truncate first). No compile-time format checking
	// exists (_Printf_format_string_ is /analyze-only and does not see through the template), so the
	// arguments are limited to numbers, enums (passed as their promoted underlying value, scoped ones
	// too), C strings and void*.
	template <typename... TArgs>
	[[nodiscard]] static FString Printf (_Printf_format_string_ const char* Fmt, TArgs... Args) noexcept;

	// ---- Access ----
	// Never null, NUL-terminated.
	[[nodiscard]] const char* GetData () const noexcept FRT_LIFETIMEBOUND
	{
		return IsHeapAllocated() ? Storage.Heap.Data : Storage.Inline.Data;
	}

	// Writable [0, Len()); the terminator is not yours.
	[[nodiscard]] char* GetData () noexcept FRT_LIFETIMEBOUND
	{
		return IsHeapAllocated() ? Storage.Heap.Data : Storage.Inline.Data;
	}

	[[nodiscard]] const char* CStr () const noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] uint32 Len () const noexcept { return IsHeapAllocated() ? Storage.Heap.Len : Storage.Inline.Tag; }
	[[nodiscard]] bool IsEmpty () const noexcept { return Len() == 0u; }
	// 22 while inline.
	[[nodiscard]] uint32 GetCapacity () const noexcept { return IsHeapAllocated() ? Storage.Heap.Capacity : InlineCapacity; }
	[[nodiscard]] bool IsHeapAllocated () const noexcept { return (Storage.Inline.Tag & HeapTag) != 0u; }

	// A byte; asserts Index < Len().
	[[nodiscard]] char operator[] (uint32 Index) const noexcept
	{
		frt_assert(Index < Len())
		return GetData()[Index];
	}

	[[nodiscard]] char& operator[] (uint32 Index) noexcept
	{
		frt_assert(Index < Len())
		return GetData()[Index];
	}

	// ---- Capacity (Clear, Reset, Free, ShrinkToFit as in TArray; Reserve as in UE's FString) ----
	// Grow-only, exactly RoundCapacity(MinCapacity); MinCapacity > MaxLength is fatal.
	FRT_CORE_API void Reserve (uint32 MinCapacity) noexcept;
	// Len() <= 22 moves the text inline and frees the block.
	FRT_CORE_API void ShrinkToFit () noexcept;

	// Len 0, keeps the block.
	void Clear () noexcept { SetLength(0u); }

	// Clear + Reserve.
	void Reset (uint32 WantedCapacity) noexcept
	{
		Clear();
		Reserve(WantedCapacity);
	}

	// Releases the block: empty and inline.
	void Free () noexcept
	{
		if (IsHeapAllocated())
		{
			string_detail::Free(Storage.Heap.Data);
		}
		Storage = UStorage{};
	}

	// Fill: a byte. Shrinking is boundary-asserted.
	void Resize (uint32 NewLen, char Fill = '\0') noexcept
	{
		const uint32 length = Len();
		if (NewLen <= length)
		{
			FRT_STRING_ASSERT_BOUNDARY(NewLen);
			SetLength(NewLen);
			return;
		}
		EnsureCapacity(NewLen - length);
		std::memset(GetData() + length, static_cast<unsigned char>(Fill), NewLen - length);
		SetLength(NewLen);
	}

	// New bytes unspecified, terminator written: for C APIs that fill the buffer.
	void ResizeUninitialized (uint32 NewLen) noexcept
	{
		const uint32 length = Len();
		if (NewLen > length)
		{
			EnsureCapacity(NewLen - length);
		}
		SetLength(NewLen);
	}

	// Len = strlen(GetData()) after a C API wrote into it.
	void TrimToNullTerminator () noexcept { SetLength(static_cast<uint32>(std::strlen(GetData()))); }

	// ---- Append (all alias-safe, all return *this) ----
	FString& Append (FStringView Text) noexcept
	{
		const uint32 length = Len();
		const uint32 count = Text.Len();
		if (count <= GetCapacity() - length)
		{
			// memmove: Text may view this string.
			std::memmove(GetData() + length, Text.GetData(), count);
			SetLength(length + count);
			return *this;
		}
		AppendSlow(Text.GetData(), count);
		return *this;
	}

	// A byte.
	FString& AppendChar (char Ch) noexcept
	{
		const uint32 length = Len();
		if (length < GetCapacity())
		{
			GetData()[length] = Ch;
			SetLength(length + 1u);
			return *this;
		}
		AppendSlow(&Ch, 1u);
		return *this;
	}

	// A byte, Count times.
	FString& AppendChars (char Ch, uint32 Count) noexcept
	{
		const uint32 length = Len();
		EnsureCapacity(Count);
		std::memset(GetData() + length, static_cast<unsigned char>(Ch), Count);
		SetLength(length + Count);
		return *this;
	}

	// Non-scalar -> U+FFFD.
	FString& AppendCodePoint (char32_t CodePoint) noexcept
	{
		char buffer[4] = {};
		const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
		return Append(FStringView(buffer, count));
	}

	// Canonical WTF-8; merges a pair split across the join (this ends with a lead surrogate, Utf16 starts
	// with a trail surrogate) into one 4-byte sequence.
	FRT_CORE_API FString& AppendWide (std::wstring_view Utf16) noexcept;
	// Same merge.
	FRT_CORE_API FString& AppendPath (const std::filesystem::path& Path, EPathStyle Style = EPathStyle::Native) noexcept;
	FString& AppendPath (const char*, EPathStyle = EPathStyle::Native) = delete;
	FString& AppendPath (const std::string&, EPathStyle = EPathStyle::Native) = delete;
	FString& AppendPath (std::string_view, EPathStyle = EPathStyle::Native) = delete;

	template <concepts::StringNumber T>
	FString& AppendNumber (T Value) noexcept
	{
		if constexpr (std::is_same_v<T, float>)
		{
			return AppendFloat(Value);
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			return AppendDouble(static_cast<double>(Value));
		}
		else if constexpr (std::is_signed_v<T>)
		{
			return AppendSigned(static_cast<int64>(Value));
		}
		else
		{
			return AppendUnsigned(static_cast<uint64>(Value));
		}
	}

	FString& AppendBool (bool Value) noexcept { return Append(Value ? "true" : "false"); }

	// Formats into a 256-byte stack buffer first (an argument may view this string), so output that fits
	// allocates nothing beyond this string's own growth: Clear() + AppendFormat on a reserved string is
	// allocation-free per frame. Appendf alike. Into a char buffer instead: FormatToBuffer.
	template <typename... TArgs>
	FString& AppendFormat (std::format_string<TArgs...> Fmt, TArgs&&... Args)
	{
		return VAppendFormat(Fmt.get(), std::make_format_args(Args...));
	}

	// Printf's argument rules.
	template <typename... TArgs>
	FString& Appendf (_Printf_format_string_ const char* Fmt, TArgs... Args) noexcept
	{
		static_assert(((std::is_arithmetic_v<TArgs> || std::is_enum_v<TArgs> || std::is_same_v<TArgs, const char*>
			|| std::is_same_v<TArgs, char*> || std::is_same_v<TArgs, const void*> || std::is_same_v<TArgs, void*>) && ...),
			"Appendf: numbers, enums, C strings (FString: .CStr()) and void* only");
		return AppendfImpl(Fmt, string_detail::PrintfArgument(Args)...);
	}

	FString& operator+= (FStringView Text) noexcept { return Append(Text); }
	// A byte.
	FString& operator+= (char Ch) noexcept { return AppendChar(Ch); }
	FString& operator+= (char32_t CodePoint) noexcept { return AppendCodePoint(CodePoint); }
	// S += 65 would append 'A', S += Count one byte, S += 1.5f '\x01', S += SomeD3DEnum a byte.
	template <typename T> requires concepts::NonTextScalar<T>
	FString& operator+= (T) = delete;

	// Builder-style streaming. Plain char is a byte; char32_t a code point; int8/uint8 are numbers; bool
	// prints true/false. No non-template bool overload: pointer -> bool is a standard conversion and would
	// beat every pointer -> FStringView route (S << u8"x", S << L"x", S << SomePtr would append "true").
	FString& operator<< (const char* NullTerminated) noexcept { return Append(NullTerminated); }
	FString& operator<< (const char8_t* NullTerminated) noexcept { return Append(NullTerminated); }
	FString& operator<< (FStringView Text) noexcept { return Append(Text); }
	FString& operator<< (char Ch) noexcept { return AppendChar(Ch); }
	FString& operator<< (char32_t CodePoint) noexcept { return AppendCodePoint(CodePoint); }

	// "true"/"false"
	template <typename T> requires std::same_as<T, bool>
	FString& operator<< (T Value) noexcept { return AppendBool(Value); }

	template <concepts::StringNumber T>
	FString& operator<< (T Value) noexcept { return AppendNumber(Value); }

	// Reflected enums: << enum_::ToString(E).
	template <typename T> requires std::is_enum_v<T>
	FString& operator<< (T) = delete;

	FString& operator<< (std::nullptr_t) = delete;

	// ---- Insert / remove (TArray-style index strategies; Index == Len() inserts at the end; boundary-asserted) ----
	// Asserts a valid index; alias-safe.
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default>
	FString& InsertAt (int64 Index, FStringView Text) noexcept
	{
		const uint32 index = ResolveInsertIndex<TIndex>(Index);
		InsertBytes(index, Text.GetData(), Text.Len());
		return *this;
	}

	// A byte.
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default>
	FString& InsertAt (int64 Index, char Ch) noexcept
	{
		const uint32 index = ResolveInsertIndex<TIndex>(Index);
		InsertBytes(index, &Ch, 1u);
		return *this;
	}

	// A code point, as AppendCodePoint encodes it (non-scalar -> U+FFFD).
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default>
	FString& InsertAt (int64 Index, char32_t CodePoint) noexcept
	{
		char buffer[4] = {};
		const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
		const uint32 index = ResolveInsertIndex<TIndex>(Index);
		InsertBytes(index, buffer, count);
		return *this;
	}

	// Numbers and enums would insert one truncated byte.
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default, typename T> requires concepts::NonTextScalar<T>
	FString& InsertAt (int64 Index, T) = delete;

	// Asserts a valid index; Count clamped to the end.
	template <IndexStrategy::EType TIndex = IndexStrategy::IS_Default>
	FString& RemoveAt (int64 Index, uint32 Count = 1) noexcept
	{
		frt_assert(IsIndexValid<TIndex>(Index))
		const uint32 length = Len();
		const uint32 index = static_cast<uint32>(IndexStrategy::ConvertToDefault<TIndex>(Index, length));
		if (index >= length)
		{
			return *this;
		}
		const uint32 count = Count < length - index ? Count : length - index;
		FRT_STRING_ASSERT_BOUNDARY(index);
		FRT_STRING_ASSERT_BOUNDARY(index + count);
		RemoveBytes(index, count);
		return *this;
	}

	// The matched length is removed (it can differ from Prefix.Len() under IgnoreCase).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	bool RemoveFromStart (FStringView Prefix) noexcept
	{
		const uint32 matched = MatchPrefix<TCase>(Prefix);
		if (matched == NotFound)
		{
			return false;
		}
		RemoveLeadingBytes(matched);
		return true;
	}

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	bool RemoveFromEnd (FStringView Suffix) noexcept
	{
		const uint32 matched = MatchSuffix<TCase>(Suffix);
		if (matched == NotFound)
		{
			return false;
		}
		SetLength(Len() - matched);
		return true;
	}

	// Count removed; alias-safe.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	uint32 RemoveAll (FStringView Needle) noexcept
	{
		return RemoveAllImpl(Needle, TCase);
	}

	// An ASCII character (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	uint32 RemoveAll (char Ch) noexcept
	{
		frt_assert(ascii::IsAscii(Ch))
		if constexpr (TCase != ESearchCase::CaseSensitive)
		{
			// The one-unit view: under IgnoreCase 'k' also removes U+212A KELVIN SIGN, three bytes long.
			return RemoveAllImpl(FStringView(&Ch, 1u), TCase);
		}
		else
		{
			char* data = GetData();
			const uint32 length = Len();
			uint32 written = 0u;
			for (uint32 read = 0u; read < length; ++read)
			{
				if (data[read] != Ch)
				{
					data[written++] = data[read];
				}
			}
			SetLength(written);
			return length - written;
		}
	}

	// A scalar value (asserted).
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	uint32 RemoveAll (char32_t CodePoint) noexcept
	{
		frt_assert(unicode::IsScalarValue(CodePoint))
		char buffer[4] = {};
		const uint32 count = unicode::EncodeUtf8(CodePoint, buffer);
		return RemoveAllImpl(FStringView(buffer, count), TCase);
	}

	// ---- Replace (non-overlapping, left to right; From non-empty, asserted; alias-safe) ----
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] FString Replace (FStringView From, FStringView To) const& noexcept
	{
		FString result(*this);
		result.ReplaceImpl(From, To, TCase);
		return result;
	}

	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	[[nodiscard]] FString Replace (FStringView From, FStringView To) && noexcept
	{
		ReplaceImpl(From, To, TCase);
		return std::move(*this);
	}

	// Count replaced.
	template <ESearchCase TCase = ESearchCase::CaseSensitive>
	uint32 ReplaceInline (FStringView From, FStringView To) noexcept
	{
		return ReplaceImpl(From, To, TCase);
	}

	// Both ASCII (asserted); in place. Count replaced.
	uint32 ReplaceCharInline (char From, char To) noexcept
	{
		frt_assert(ascii::IsAscii(From) && ascii::IsAscii(To))
		char* data = GetData();
		const uint32 length = Len();
		uint32 count = 0u;
		for (uint32 i = 0u; i < length; ++i)
		{
			if (data[i] == From)
			{
				data[i] = To;
				++count;
			}
		}
		return count;
	}

	// Byte for byte, so not for code points, whose replacement may be longer or shorter: ReplaceInline("ї", "і").
	template <typename TFrom, typename TTo> requires std::same_as<TFrom, char32_t> || std::same_as<TTo, char32_t>
	uint32 ReplaceCharInline (TFrom, TTo) = delete;

	// ---- Substrings: X() const& copies, X() && reuses this buffer, XInline() mutates ----
	// Same semantics (clamping + boundary asserts) as the FStringView functions of the same name.
	[[nodiscard]] FString Left (uint32 Count) const& noexcept { return FString(View().Left(Count)); }
	[[nodiscard]] FString Left (uint32 Count) && noexcept { LeftInline(Count); return std::move(*this); }
	FString& LeftInline (uint32 Count) noexcept
	{
		const uint32 length = Len();
		const uint32 newLength = Count < length ? Count : length;
		FRT_STRING_ASSERT_BOUNDARY(newLength);
		SetLength(newLength);
		return *this;
	}

	[[nodiscard]] FString LeftChop (uint32 Count) const& noexcept { return FString(View().LeftChop(Count)); }
	[[nodiscard]] FString LeftChop (uint32 Count) && noexcept { LeftChopInline(Count); return std::move(*this); }
	FString& LeftChopInline (uint32 Count) noexcept
	{
		const uint32 length = Len();
		const uint32 newLength = length - (Count < length ? Count : length);
		FRT_STRING_ASSERT_BOUNDARY(newLength);
		SetLength(newLength);
		return *this;
	}

	[[nodiscard]] FString Right (uint32 Count) const& noexcept { return FString(View().Right(Count)); }
	[[nodiscard]] FString Right (uint32 Count) && noexcept { RightInline(Count); return std::move(*this); }
	FString& RightInline (uint32 Count) noexcept
	{
		const uint32 length = Len();
		const uint32 start = length - (Count < length ? Count : length);
		FRT_STRING_ASSERT_BOUNDARY(start);
		RemoveLeadingBytes(start);
		return *this;
	}

	[[nodiscard]] FString RightChop (uint32 Count) const& noexcept { return FString(View().RightChop(Count)); }
	[[nodiscard]] FString RightChop (uint32 Count) && noexcept { RightChopInline(Count); return std::move(*this); }
	FString& RightChopInline (uint32 Count) noexcept
	{
		const uint32 length = Len();
		const uint32 start = Count < length ? Count : length;
		FRT_STRING_ASSERT_BOUNDARY(start);
		RemoveLeadingBytes(start);
		return *this;
	}

	[[nodiscard]] FString Mid (uint32 Start, uint32 Count = MaxLength) const& noexcept { return FString(View().Mid(Start, Count)); }
	[[nodiscard]] FString Mid (uint32 Start, uint32 Count = MaxLength) && noexcept { MidInline(Start, Count); return std::move(*this); }
	FString& MidInline (uint32 Start, uint32 Count = MaxLength) noexcept
	{
		uint32 begin = 0u;
		uint32 end = 0u;
		ClampMid(Len(), Start, Count, begin, end);
		FRT_STRING_ASSERT_BOUNDARY(begin);
		FRT_STRING_ASSERT_BOUNDARY(end);
		SetLength(end);
		RemoveLeadingBytes(begin);
		return *this;
	}

	[[nodiscard]] FString Slice (int64 Begin, int64 End = SliceEnd) const& noexcept { return FString(View().Slice(Begin, End)); }
	[[nodiscard]] FString Slice (int64 Begin, int64 End = SliceEnd) && noexcept { SliceInline(Begin, End); return std::move(*this); }
	FString& SliceInline (int64 Begin, int64 End = SliceEnd) noexcept
	{
		uint32 begin = 0u;
		uint32 end = 0u;
		ClampSlice(Len(), Begin, End, begin, end);
		if (end <= begin)
		{
			Clear();
			return *this;
		}
		FRT_STRING_ASSERT_BOUNDARY(begin);
		FRT_STRING_ASSERT_BOUNDARY(end);
		SetLength(end);
		RemoveLeadingBytes(begin);
		return *this;
	}

	// FStringView::Truncate's label-safe cut.
	[[nodiscard]] FString Truncate (uint32 MaxBytes) const& noexcept { return FString(View().Truncate(MaxBytes)); }
	[[nodiscard]] FString Truncate (uint32 MaxBytes) && noexcept { TruncateInline(MaxBytes); return std::move(*this); }
	FString& TruncateInline (uint32 MaxBytes) noexcept
	{
		SetLength(string_impl::TruncateLength(View(), MaxBytes));
		return *this;
	}

	// ---- Trim (Unicode White_Space, or any unit of CharSet; an empty CharSet trims nothing) ----
	// The lengths are computed before anything moves: CharSet may view this string.
	[[nodiscard]] FString TrimStart () const& noexcept { return FString(View().TrimStart()); }
	[[nodiscard]] FString TrimStart () && noexcept { TrimStartInline(); return std::move(*this); }
	FString& TrimStartInline () noexcept { RemoveLeadingBytes(Len() - View().TrimStart().Len()); return *this; }

	[[nodiscard]] FString TrimEnd () const& noexcept { return FString(View().TrimEnd()); }
	[[nodiscard]] FString TrimEnd () && noexcept { TrimEndInline(); return std::move(*this); }
	FString& TrimEndInline () noexcept { SetLength(View().TrimEnd().Len()); return *this; }

	[[nodiscard]] FString TrimStartAndEnd () const& noexcept { return FString(View().TrimStartAndEnd()); }
	[[nodiscard]] FString TrimStartAndEnd () && noexcept { TrimStartAndEndInline(); return std::move(*this); }
	FString& TrimStartAndEndInline () noexcept { return TrimEndInline().TrimStartInline(); }

	[[nodiscard]] FString TrimStart (FStringView CharSet) const& noexcept { return FString(View().TrimStart(CharSet)); }
	[[nodiscard]] FString TrimStart (FStringView CharSet) && noexcept { TrimStartInline(CharSet); return std::move(*this); }
	FString& TrimStartInline (FStringView CharSet) noexcept { RemoveLeadingBytes(Len() - View().TrimStart(CharSet).Len()); return *this; }

	[[nodiscard]] FString TrimEnd (FStringView CharSet) const& noexcept { return FString(View().TrimEnd(CharSet)); }
	[[nodiscard]] FString TrimEnd (FStringView CharSet) && noexcept { TrimEndInline(CharSet); return std::move(*this); }
	FString& TrimEndInline (FStringView CharSet) noexcept { SetLength(View().TrimEnd(CharSet).Len()); return *this; }

	[[nodiscard]] FString TrimStartAndEnd (FStringView CharSet) const& noexcept { return FString(View().TrimStartAndEnd(CharSet)); }
	[[nodiscard]] FString TrimStartAndEnd (FStringView CharSet) && noexcept { TrimStartAndEndInline(CharSet); return std::move(*this); }
	FString& TrimStartAndEndInline (FStringView CharSet) noexcept
	{
		// Both lengths first: trimming the end could move the bytes CharSet views.
		const FStringView trimmed = View().TrimStartAndEnd(CharSet);
		const uint32 start = trimmed.IsEmpty() ? 0u : static_cast<uint32>(trimmed.GetData() - GetData());
		SetLength(start + trimmed.Len());
		RemoveLeadingBytes(start);
		return *this;
	}

	// QuoteChars: ASCII (asserted).
	[[nodiscard]] FString TrimQuotes (FStringView QuoteChars = "\"") const& noexcept { return FString(View().TrimQuotes(QuoteChars)); }
	[[nodiscard]] FString TrimQuotes (FStringView QuoteChars = "\"") && noexcept { TrimQuotesInline(QuoteChars); return std::move(*this); }
	FString& TrimQuotesInline (FStringView QuoteChars = "\"") noexcept
	{
		const uint32 length = Len();
		if (View().TrimQuotes(QuoteChars).Len() != length)
		{
			SetLength(length - 1u);
			RemoveBytes(0u, 1u);
		}
		return *this;
	}

	// ---- Case: Unicode simple mappings; the byte length may change; ill-formed bytes and WTF-8 surrogates
	//      are kept. Views: FString(View).ToUpperInline(). ----
	[[nodiscard]] FString ToUpper () const& noexcept { FString result(*this); result.ToUpperInline(); return result; }
	[[nodiscard]] FString ToUpper () && noexcept { ToUpperInline(); return std::move(*this); }
	FString& ToUpperInline () noexcept { return ConvertCase(unicode::ECaseMapping::Upper); }

	[[nodiscard]] FString ToLower () const& noexcept { FString result(*this); result.ToLowerInline(); return result; }
	[[nodiscard]] FString ToLower () && noexcept { ToLowerInline(); return std::move(*this); }
	FString& ToLowerInline () noexcept { return ConvertCase(unicode::ECaseMapping::Lower); }

	// Caseless keys.
	[[nodiscard]] FString FoldCase () const& noexcept { FString result(*this); result.FoldCaseInline(); return result; }
	[[nodiscard]] FString FoldCase () && noexcept { FoldCaseInline(); return std::move(*this); }
	FString& FoldCaseInline () noexcept { return ConvertCase(unicode::ECaseMapping::Fold); }

	void Swap (FString& Other) noexcept
	{
		const UStorage storage = Storage;
		Storage = Other.Storage;
		Other.Storage = storage;
	}

	friend void swap (FString& A, FString& B) noexcept { A.Swap(B); }

	// ---- STL (for std algorithms, std::back_inserter, std::format_to, gtest). Engine code uses the names above. ----
	[[nodiscard]] char* data () noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] const char* data () const noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] const char* c_str () const noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] size_type size () const noexcept { return Len(); }
	[[nodiscard]] size_type length () const noexcept { return Len(); }
	[[nodiscard]] size_type capacity () const noexcept { return GetCapacity(); }
	[[nodiscard]] bool empty () const noexcept { return Len() == 0u; }
	// A byte; inline fast path.
	void push_back (char Ch) noexcept { AppendChar(Ch); }
	[[nodiscard]] iterator begin () noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] const_iterator begin () const noexcept FRT_LIFETIMEBOUND { return GetData(); }
	[[nodiscard]] iterator end () noexcept FRT_LIFETIMEBOUND { return GetData() + Len(); }
	[[nodiscard]] const_iterator end () const noexcept FRT_LIFETIMEBOUND { return GetData() + Len(); }

private:
	static constexpr uint8 HeapTag = 0x80u;

	struct SHeap
	{
		char* Data;          // Capacity + 1 bytes from string_detail::Allocate
		uint32 Len;
		uint32 Capacity;     // usable bytes, terminator excluded; (Capacity + 1) % 16 == 0
		uint8 Unused[7];
		uint8 Tag;           // == HeapTag
	};

	struct SInline
	{
		char Data[InlineCapacity + 1];   // up to 22 bytes + terminator
		uint8 Tag;                       // == Len (0..22); bit 7 clear
	};

	union UStorage
	{
		SInline Inline;      // first member: Storage{} zero-initializes it -> empty string
		SHeap Heap;
	};

	// Length and terminator, once the bytes [0, NewLen) are in place. NewLen <= GetCapacity().
	void SetLength (uint32 NewLen) noexcept
	{
		if (IsHeapAllocated())
		{
			Storage.Heap.Len = NewLen;
			Storage.Heap.Data[NewLen] = '\0';
		}
		else
		{
			Storage.Inline.Tag = static_cast<uint8>(NewLen);
			Storage.Inline.Data[NewLen] = '\0';
		}
	}

	// Most trims and substrings remove nothing at the front: no call then.
	void RemoveLeadingBytes (uint32 Count) noexcept
	{
		if (Count != 0u)
		{
			RemoveBytes(0u, Count);
		}
	}

	// Room for Len() + Extra bytes, growing geometrically.
	void EnsureCapacity (uint32 Extra) noexcept
	{
		if (Extra > GetCapacity() - Len())
		{
			GrowForAppend(Extra);
		}
	}

	// Construction from text: inline when it fits (the storage is all zero: the terminator is there),
	// otherwise exactly RoundCapacity(InLen).
	void InitBytes (const char* InData, size_t InLen) noexcept
	{
		if (InLen <= InlineCapacity)
		{
			if (InLen != 0u)
			{
				std::memcpy(Storage.Inline.Data, InData, InLen);
			}
			Storage.Inline.Tag = static_cast<uint8>(InLen);
			return;
		}
		AssignBytes(InData, string_detail::CheckLength(InLen));
	}

	// Assignment from text; alias-safe. Short text into an inline string stays here: memmove covers
	// S = S.CStr() + 2.
	void AssignText (const char* InData, size_t InLen) noexcept
	{
		if (InLen <= InlineCapacity && !IsHeapAllocated())
		{
			std::memmove(Storage.Inline.Data, InData, InLen);
			Storage.Inline.Data[InLen] = '\0';
			Storage.Inline.Tag = static_cast<uint8>(InLen);
			return;
		}
		AssignBytes(InData, string_detail::CheckLength(InLen));
	}

	template <IndexStrategy::EType TIndex>
	uint32 ResolveInsertIndex (int64 Index) const noexcept
	{
		// Len() itself appends, as in TArray::Insert.
		const uint32 length = Len();
		const bool bAppend = Index == static_cast<int64>(length);
		frt_assert(bAppend || IsIndexValid<TIndex>(Index))
		const uint32 index = bAppend ? length : static_cast<uint32>(IndexStrategy::ConvertToDefault<TIndex>(Index, length));
		FRT_STRING_ASSERT_BOUNDARY(index);
		return index;
	}

	// The out-of-line implementations (String.cpp). Member templates dispatch to these: a template cannot
	// be exported without explicit instantiations in every client.
	FRT_CORE_API void CopyFrom (const FString& Other) noexcept;                               // heap source
	FRT_CORE_API void GrowForAppend (uint32 Extra) noexcept;                                  // capacity for Len() + Extra
	FRT_CORE_API void AppendSlow (const char* InData, uint32 Count) noexcept;                 // growth + alias-safe copy
	FRT_CORE_API void AssignBytes (const char* InData, uint32 Count) noexcept;                // alias-safe assign
	FRT_CORE_API void InsertBytes (uint32 Index, const char* InData, uint32 Count) noexcept;  // alias-safe
	FRT_CORE_API void RemoveBytes (uint32 Index, uint32 Count) noexcept;
	FRT_CORE_API uint32 ReplaceImpl (FStringView From, FStringView To, ESearchCase Case) noexcept;
	FRT_CORE_API uint32 RemoveAllImpl (FStringView Needle, ESearchCase Case) noexcept;
	FRT_CORE_API FString& AppendSigned (int64 Value) noexcept;
	FRT_CORE_API FString& AppendUnsigned (uint64 Value) noexcept;
	FRT_CORE_API FString& AppendFloat (float Value) noexcept;                                 // shortest round trip
	FRT_CORE_API FString& AppendDouble (double Value) noexcept;
	FRT_CORE_API FString& AppendFixed (double Value, uint32 MinFractionalDigits, bool bSinglePrecision) noexcept;
	FRT_CORE_API FString& VAppendFormat (std::string_view Fmt, std::format_args Args);
	FRT_CORE_API FString& AppendfImpl (const char* Fmt, ...) noexcept;
	FRT_CORE_API FString& ConvertCase (unicode::ECaseMapping Mapping) noexcept;
	// Moves the text into a block of NewCapacity (> InlineCapacity, >= Len()). Only the functions above call it.
	void ChangeCapacity (uint32 NewCapacity) noexcept;

	UStorage Storage;

	friend struct FStringLayoutCheck;   // String.cpp: offsetof static_asserts
};

// ABI: inline code of the string types is compiled into game modules. A change of their layout or inline
// semantics (sizes, InlineCapacity, the tag encoding, the growth policy of the inline fast paths) bumps
// FRT_GAME_API_VERSION (Engine/GameModule.h), so a stale game DLL fails to load.
static_assert(sizeof(FString) == 24 && alignof(FString) == 8);
static_assert(FString::InlineCapacity == 22u);

// gtest: quoted and escaped, not a list of chars.
FRT_CORE_API void PrintTo (const FString& Text, std::ostream* Out);
}


// Right after the type (StringView.h explains why).
template <>
struct std::formatter<frt::FString, char> : std::formatter<frt::FStringView, char>
{
	template <typename TContext>
	auto format (const frt::FString& Value, TContext& Context) const
	{
		return std::formatter<frt::FStringView, char>::format(Value.View(), Context);
	}
};

// The same value as for FStringView and FStringLiteral of the same bytes.
template <>
struct std::hash<frt::FString>
{
	size_t operator() (const frt::FString& S) const noexcept { return static_cast<size_t>(S.GetHash()); }
};


namespace frt
{
template <typename TRange>
FString FString::Join (const TRange& Parts, FStringView Separator) noexcept
{
	// One allocation: the length first.
	uint64 total = 0u;
	bool bFirst = true;
	for (const auto& part : Parts)
	{
		total += FStringView(part).Len() + (bFirst ? 0u : Separator.Len());
		bFirst = false;
	}

	FString result;
	if (total > InlineCapacity)
	{
		result.Reserve(string_detail::CheckLength(total));
	}

	bFirst = true;
	for (const auto& part : Parts)
	{
		if (!bFirst)
		{
			result.Append(Separator);
		}
		result.Append(FStringView(part));
		bFirst = false;
	}
	return result;
}

template <typename TRange, typename TProjection>
FString FString::JoinBy (const TRange& Parts, FStringView Separator, TProjection&& Projection)
{
	FString result;
	bool bFirst = true;
	for (const auto& part : Parts)
	{
		if (!bFirst)
		{
			result.Append(Separator);
		}
		bFirst = false;

		decltype(auto) projected = Projection(part);
		if constexpr (std::is_same_v<std::remove_cvref_t<decltype(projected)>, char>)
		{
			result.AppendChar(projected);
		}
		else
		{
			result.Append(FStringView(projected));
		}
	}
	return result;
}

template <typename... TArgs>
FString FString::Format (std::format_string<TArgs...> Fmt, TArgs&&... Args)
{
	FString result;
	result.VAppendFormat(Fmt.get(), std::make_format_args(Args...));
	return result;
}

template <typename... TArgs>
FString FString::Printf (_Printf_format_string_ const char* Fmt, TArgs... Args) noexcept
{
	static_assert(((std::is_arithmetic_v<TArgs> || std::is_enum_v<TArgs> || std::is_same_v<TArgs, const char*>
		|| std::is_same_v<TArgs, char*> || std::is_same_v<TArgs, const void*> || std::is_same_v<TArgs, void*>) && ...),
		"Printf: numbers, enums, C strings (FString: .CStr()) and void* only");

	FString result;
	result.AppendfImpl(Fmt, string_detail::PrintfArgument(Args)...);
	return result;
}


// ---- Concatenation ----
// The buffer-reusing overloads are constrained templates on purpose: FString converts implicitly from
// const char* and FStringLiteral, so a plain `FString&&` parameter would also accept a temporary built
// from a literal, and `"x" + Name` / `Name + "x"` would become ambiguous with the (FStringView,
// FStringView) overload. Deduction never applies conversions, so `TString&&` with
// `std::same_as<TString, FString>` matches only a real FString rvalue. Never add a plain FString&& or
// const FString& overload.
// (`"lit" + 'c'` and `"lit" + U'ї'` stay built-in pointer arithmetic, as with std::string: never write them.)

[[nodiscard]] inline FString operator+ (FStringView A, FStringView B) noexcept
{
	const uint64 total = static_cast<uint64>(A.Len()) + B.Len();
	FString result;
	if (total > FString::InlineCapacity)
	{
		result.Reserve(string_detail::CheckLength(total));
	}
	result.Append(A);
	result.Append(B);
	return result;
}

// Appends into A's buffer.
template <typename TString> requires std::same_as<TString, FString>
[[nodiscard]] FString operator+ (TString&& A, FStringView B) noexcept
{
	A.Append(B);
	return std::move(A);
}

// Inserts at B's front (alias-safe).
template <typename TString> requires std::same_as<TString, FString>
[[nodiscard]] FString operator+ (FStringView A, TString&& B) noexcept
{
	B.InsertAt(0, A);
	return std::move(B);
}

// Both rvalues: reuses A.
template <typename TStringA, typename TStringB> requires std::same_as<TStringA, FString> && std::same_as<TStringB, FString>
[[nodiscard]] FString operator+ (TStringA&& A, TStringB&& B) noexcept
{
	A.Append(B);
	return std::move(A);
}

// B: a byte.
[[nodiscard]] inline FString operator+ (FStringView A, char B) noexcept
{
	const uint64 total = static_cast<uint64>(A.Len()) + 1u;
	FString result;
	if (total > FString::InlineCapacity)
	{
		result.Reserve(string_detail::CheckLength(total));
	}
	result.Append(A);
	result.AppendChar(B);
	return result;
}

// A: a byte.
[[nodiscard]] inline FString operator+ (char A, FStringView B) noexcept
{
	const uint64 total = static_cast<uint64>(B.Len()) + 1u;
	FString result;
	if (total > FString::InlineCapacity)
	{
		result.Reserve(string_detail::CheckLength(total));
	}
	result.AppendChar(A);
	result.Append(B);
	return result;
}

template <typename TString> requires std::same_as<TString, FString>
[[nodiscard]] FString operator+ (TString&& A, char B) noexcept
{
	A.AppendChar(B);
	return std::move(A);
}

template <typename TString> requires std::same_as<TString, FString>
[[nodiscard]] FString operator+ (char A, TString&& B) noexcept
{
	B.InsertAt(0, A);
	return std::move(B);
}

// B: a code point, as AppendCodePoint encodes it (non-scalar -> U+FFFD).
[[nodiscard]] inline FString operator+ (FStringView A, char32_t B) noexcept
{
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(B, buffer);
	return A + FStringView(buffer, count);
}

[[nodiscard]] inline FString operator+ (char32_t A, FStringView B) noexcept
{
	char buffer[4] = {};
	const uint32 count = unicode::EncodeUtf8(A, buffer);
	return FStringView(buffer, count) + B;
}

template <typename TString> requires std::same_as<TString, FString>
[[nodiscard]] FString operator+ (TString&& A, char32_t B) noexcept
{
	A.AppendCodePoint(B);
	return std::move(A);
}

template <typename TString> requires std::same_as<TString, FString>
[[nodiscard]] FString operator+ (char32_t A, TString&& B) noexcept
{
	B.InsertAt(0, A);
	return std::move(B);
}

// `Name + 5`, `Name + 1.5`, `Name + UnscopedEnum` would otherwise append one (truncated) byte.
template <typename T> requires concepts::NonTextScalar<T>
FString operator+ (FStringView A, T B) = delete;

template <typename T> requires concepts::NonTextScalar<T>
FString operator+ (T A, FStringView B) = delete;


// ---- FormatToBuffer ----

namespace string_impl
{
// FormatToBuffer's cut when the output did not fit: Truncate's rules on Data[0, Written) as a prefix of the
// full output, of which Lookahead holds the LookaheadLen (<= 4) bytes that follow. Returns the length to keep.
[[nodiscard]] FRT_CORE_API uint32 TruncateFormattedLength (const char* Data, uint32 Written, const char* Lookahead, uint32 LookaheadLen) noexcept;
}

namespace detail
{
// FormatToBuffer's output: the first Capacity bytes go to the buffer, the next 4 are kept, because the cut
// needs to see the code point it lands in front of (a combining mark is cut with its base).
struct SFormatToBufferState
{
	char* Data = nullptr;
	uint32 Capacity = 0u;
	uint32 Written = 0u;
	uint32 Overflow = 0u;      // bytes past Capacity; the first 4 are in Lookahead
	char Lookahead[4] = {};
};

class FFormatToBufferIterator
{
public:
	using difference_type = std::ptrdiff_t;

	FFormatToBufferIterator () noexcept = default;

	explicit FFormatToBufferIterator (SFormatToBufferState& InState) noexcept
		: State(&InState)
	{}

	FFormatToBufferIterator& operator* () noexcept { return *this; }
	FFormatToBufferIterator& operator++ () noexcept { return *this; }
	FFormatToBufferIterator operator++ (int) noexcept { return *this; }

	FFormatToBufferIterator& operator= (char Ch) noexcept
	{
		if (State->Written < State->Capacity)
		{
			State->Data[State->Written++] = Ch;
		}
		else
		{
			if (State->Overflow < 4u)
			{
				State->Lookahead[State->Overflow] = Ch;
			}
			++State->Overflow;
		}
		return *this;
	}

private:
	SFormatToBufferState* State = nullptr;
};
}

// Allocation-free formatting for per-frame UI text: std::format into Buffer (at most Buffer.size() - 1
// bytes), cut back by Truncate's rules if the output did not fit, NUL-terminated. Returns a view of the
// written text; .GetData() is a valid C string while Buffer lives.
// Usage: char label[48]; ImGui::BeginMenu(FormatToBuffer(label, "Speed {:.2f}###CameraSpeed", Speed).GetData());
template <typename... TArgs>
FStringView FormatToBuffer (std::span<char> Buffer FRT_LIFETIMEBOUND, std::format_string<TArgs...> Fmt, TArgs&&... Args)
{
	if (Buffer.empty())
	{
		return FStringView();
	}

	const size_t capacity = Buffer.size() - 1u;
	detail::SFormatToBufferState state;
	state.Data = Buffer.data();
	state.Capacity = static_cast<uint32>(capacity < string_detail::MaxLength ? capacity : string_detail::MaxLength);
	std::format_to(detail::FFormatToBufferIterator(state), Fmt, std::forward<TArgs>(Args)...);

	uint32 length = state.Written;
	if (state.Overflow != 0u)
	{
		length = string_impl::TruncateFormattedLength(state.Data, state.Written, state.Lookahead, state.Overflow < 4u ? state.Overflow : 4u);
	}
	Buffer[length] = '\0';
	return FStringView(Buffer.data(), length);
}


// ---- Hashing functors (transparent: map lookups by view, literal or std::string do not allocate) ----
// All bodies inline: the structs are not exported, and an out-of-line member would be LNK2019 in other modules.

struct SStringHash
{
	using is_transparent = void;
	size_t operator() (FStringView S) const noexcept { return static_cast<size_t>(S.GetHash()); }
};

struct SStringEqual
{
	using is_transparent = void;
	bool operator() (FStringView A, FStringView B) const noexcept { return A == B; }
};

struct SStringLess
{
	using is_transparent = void;
	bool operator() (FStringView A, FStringView B) const noexcept { return A < B; }
};

struct SStringHashIgnoreCase
{
	using is_transparent = void;
	size_t operator() (FStringView S) const noexcept { return static_cast<size_t>(string_impl::HashFolded(S, ESearchCase::IgnoreCase)); }
};

struct SStringEqualIgnoreCase
{
	using is_transparent = void;
	bool operator() (FStringView A, FStringView B) const noexcept { return string_impl::EqualsFolded(A, B, ESearchCase::IgnoreCase); }
};

// A strict weak order consistent with SStringEqualIgnoreCase: safe for std::map and std::sort.
struct SStringLessIgnoreCase
{
	using is_transparent = void;
	bool operator() (FStringView A, FStringView B) const noexcept { return string_impl::CompareFolded(A, B, ESearchCase::IgnoreCase) < 0; }
};
}
