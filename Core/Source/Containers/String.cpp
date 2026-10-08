#include "Containers/String.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <iterator>
#include <ostream>

#include <Windows.h>

#include "Unicode/UnicodeTablesInternal.h"


namespace
{
using namespace frt;

constinit std::atomic<uint64> GHeapAllocations{ 0u };
// constinit: Allocate is safe from any dynamic initializer, whatever the initialization order.
constinit thread_local uint64 GThreadHeapAllocations = 0u;

// Reports and ends the process. The string types cannot throw (every function but Format is noexcept)
// and cannot carry on without memory.
[[noreturn]] void Fatal (const char* Message) noexcept
{
	std::fputs(Message, stderr);
	std::fflush(stderr);

	// The message is ASCII: widened byte by byte (a windowed build only ever shows the debugger output).
	wchar_t wide[256] = {};
	for (size_t i = 0u; i + 1u < std::size(wide) && Message[i] != '\0'; ++i)
	{
		wide[i] = static_cast<wchar_t>(Message[i]);
	}
	OutputDebugStringW(wide);

	if (IsDebuggerPresent())
	{
		__debugbreak();
	}
	std::abort();
}

// Whether Text points into the block [Base, Base + Capacity] (terminator included). Integer comparison:
// relational operators on unrelated pointers are unspecified.
bool PointsInto (const char* Text, const char* Base, uint32 Capacity) noexcept
{
	const uintptr_t text = reinterpret_cast<uintptr_t>(Text);
	const uintptr_t base = reinterpret_cast<uintptr_t>(Base);
	return text >= base && text - base <= Capacity;
}

SStringMatch FindForReplace (FStringView Text, FStringView Needle, uint32 Start, ESearchCase Case) noexcept
{
	if (Case == ESearchCase::CaseSensitive)
	{
		const uint32 index = Text.Find(Needle, Start);
		return index == FStringView::NotFound ? SStringMatch() : SStringMatch{ index, Needle.Len() };
	}
	return string_impl::FindFolded(Text, Needle, Start, Case);
}

// Code points Escape writes as \xHH bytes besides the controls: line/paragraph separators and the bidi
// controls, which can hide or reorder text in terminals and test logs.
bool IsEscapedCodePoint (char32_t CodePoint) noexcept
{
	switch (unicode::tables::LookupGeneralCategory(CodePoint))
	{
	case unicode::EGeneralCategory::Control:
	case unicode::EGeneralCategory::LineSeparator:
	case unicode::EGeneralCategory::ParagraphSeparator:
		return true;
	default:
		break;
	}
	return CodePoint == 0x061Cu || CodePoint == 0x200Eu || CodePoint == 0x200Fu
		|| (CodePoint >= 0x202Au && CodePoint <= 0x202Eu) || (CodePoint >= 0x2066u && CodePoint <= 0x2069u);
}

void AppendHexEscape (FString& Out, char Byte) noexcept
{
	const uint32 value = static_cast<unsigned char>(Byte);
	const char escape[4] = { '\\', 'x', ascii::HexDigit(value >> 4), ascii::HexDigit(value) };
	Out.Append(FStringView(escape, 4u));
}

// The 3 bytes ED A0..AF 80..BF: a lead surrogate in WTF-8, waiting for its trail.
bool EndsWithLeadSurrogate (FStringView Text) noexcept
{
	if (Text.Len() < 3u)
	{
		return false;
	}
	const unsigned char* tail = reinterpret_cast<const unsigned char*>(Text.GetData() + Text.Len() - 3u);
	return tail[0] == 0xEDu && tail[1] >= 0xA0u && tail[1] <= 0xAFu && tail[2] >= 0x80u && tail[2] <= 0xBFu;
}

constexpr char ReplacementCharacterUtf8[] = "\xEF\xBF\xBD";
}


namespace frt::string_detail
{
void* Allocate (size_t Bytes) noexcept
{
	void* block = std::malloc(Bytes);
	if (block == nullptr)
	{
		FatalAllocationError(Bytes);
	}
	GHeapAllocations.fetch_add(1u, std::memory_order_relaxed);
	++GThreadHeapAllocations;
	return block;
}

void* Reallocate (void* Block, size_t Bytes) noexcept
{
	void* block = std::realloc(Block, Bytes);
	if (block == nullptr)
	{
		FatalAllocationError(Bytes);
	}
	GHeapAllocations.fetch_add(1u, std::memory_order_relaxed);
	++GThreadHeapAllocations;
	return block;
}

void Free (void* Block) noexcept
{
	std::free(Block);
}

void FatalLengthError () noexcept
{
	Fatal("FString: text longer than MaxLength (0x7FFFFFFF bytes)\n");
}

void FatalAllocationError (size_t Bytes) noexcept
{
	char message[96] = "FString: out of memory allocating ";
	const size_t prefix = std::strlen(message);
	const std::to_chars_result result = std::to_chars(message + prefix, message + sizeof(message) - 8u, Bytes);
	std::memcpy(result.ptr, " bytes\n", 8u);
	Fatal(message);
}

uint64 GetThreadHeapAllocationCount () noexcept
{
	return GThreadHeapAllocations;
}

uint64 GetHeapAllocationCount () noexcept
{
	return GHeapAllocations.load(std::memory_order_relaxed);
}
}


namespace frt
{
// The layout the natvis and the inline code rely on.
struct FStringLayoutCheck
{
	static_assert(offsetof(FString::SInline, Tag) == 23u);
	static_assert(offsetof(FString::SHeap, Tag) == offsetof(FString::SInline, Tag));
	static_assert(sizeof(FString::UStorage) == 24u);
	static_assert(std::is_standard_layout_v<FString>);
};


// ---- Allocation and growth ----

void FString::ChangeCapacity (uint32 NewCapacity) noexcept
{
	frt_assert(NewCapacity > InlineCapacity && NewCapacity >= Len())
	if (IsHeapAllocated())
	{
		Storage.Heap.Data = static_cast<char*>(string_detail::Reallocate(Storage.Heap.Data, static_cast<size_t>(NewCapacity) + 1u));
		Storage.Heap.Capacity = NewCapacity;
		return;
	}

	// The bytes leave the union before the heap fields overwrite them.
	const uint32 length = Storage.Inline.Tag;
	char* block = static_cast<char*>(string_detail::Allocate(static_cast<size_t>(NewCapacity) + 1u));
	std::memcpy(block, Storage.Inline.Data, length + 1u);

	Storage.Heap.Data = block;
	Storage.Heap.Len = length;
	Storage.Heap.Capacity = NewCapacity;
	std::memset(Storage.Heap.Unused, 0, sizeof(Storage.Heap.Unused));
	Storage.Heap.Tag = HeapTag;
}

void FString::Reserve (uint32 MinCapacity) noexcept
{
	if (MinCapacity <= GetCapacity())
	{
		return;
	}
	if (MinCapacity > MaxLength)
	{
		string_detail::FatalLengthError();
	}
	ChangeCapacity(static_cast<uint32>(string_detail::RoundCapacity(MinCapacity)));
}

void FString::ShrinkToFit () noexcept
{
	if (!IsHeapAllocated())
	{
		return;
	}

	const uint32 length = Storage.Heap.Len;
	if (length <= InlineCapacity)
	{
		char* block = Storage.Heap.Data;
		UStorage storage{};
		std::memcpy(storage.Inline.Data, block, length);
		storage.Inline.Tag = static_cast<uint8>(length);
		Storage = storage;
		string_detail::Free(block);
		return;
	}

	const uint32 capacity = static_cast<uint32>(string_detail::RoundCapacity(length));
	if (capacity < Storage.Heap.Capacity)
	{
		ChangeCapacity(capacity);
	}
}

void FString::GrowForAppend (uint32 Extra) noexcept
{
	const uint64 required = static_cast<uint64>(Len()) + Extra;
	if (required > MaxLength)
	{
		string_detail::FatalLengthError();
	}
	if (required <= GetCapacity())
	{
		return;
	}
	ChangeCapacity(string_detail::GetGrowCapacity(GetCapacity(), required, !IsHeapAllocated()));
}

void FString::CopyFrom (const FString& Other) noexcept
{
	AssignBytes(Other.GetData(), Other.Len());
}

void FString::AppendSlow (const char* InData, uint32 Count) noexcept
{
	// Growing may move or free the block InData points into: keep the offset instead.
	const bool bAliased = PointsInto(InData, GetData(), GetCapacity());
	const size_t offset = bAliased ? static_cast<size_t>(InData - GetData()) : 0u;

	const uint32 length = Len();
	GrowForAppend(Count);
	const char* source = bAliased ? GetData() + offset : InData;
	std::memmove(GetData() + length, source, Count);
	SetLength(length + Count);
}

void FString::AssignBytes (const char* InData, uint32 Count) noexcept
{
	const uint32 capacity = GetCapacity();
	if (PointsInto(InData, GetData(), capacity))
	{
		// Our own text: it fits where it is.
		std::memmove(GetData(), InData, Count);
		SetLength(Count);
		return;
	}

	if (Count <= capacity)
	{
		std::memcpy(GetData(), InData, Count);
		SetLength(Count);
		return;
	}

	// Construction and assignment copy no slack: exactly RoundCapacity(Count).
	const uint32 newCapacity = static_cast<uint32>(string_detail::RoundCapacity(Count));
	char* block = static_cast<char*>(string_detail::Allocate(static_cast<size_t>(newCapacity) + 1u));
	std::memcpy(block, InData, Count);
	block[Count] = '\0';

	if (IsHeapAllocated())
	{
		string_detail::Free(Storage.Heap.Data);
	}
	Storage.Heap.Data = block;
	Storage.Heap.Len = Count;
	Storage.Heap.Capacity = newCapacity;
	std::memset(Storage.Heap.Unused, 0, sizeof(Storage.Heap.Unused));
	Storage.Heap.Tag = HeapTag;
}

void FString::InsertBytes (uint32 Index, const char* InData, uint32 Count) noexcept
{
	if (Count == 0u)
	{
		return;
	}
	frt_assert(Index <= Len())

	// A source inside this string can straddle the insertion point: no offset fixes that, a copy does.
	if (PointsInto(InData, GetData(), GetCapacity()))
	{
		const FString copy(InData, Count);
		InsertBytes(Index, copy.GetData(), Count);
		return;
	}

	const uint32 length = Len();
	GrowForAppend(Count);
	char* data = GetData();
	std::memmove(data + Index + Count, data + Index, length - Index);
	std::memcpy(data + Index, InData, Count);
	SetLength(length + Count);
}

void FString::RemoveBytes (uint32 Index, uint32 Count) noexcept
{
	const uint32 length = Len();
	frt_assert(Index <= length && Count <= length - Index)
	char* data = GetData();
	std::memmove(data + Index, data + Index + Count, length - Index - Count);
	SetLength(length - Count);
}


// ---- Replace ----

uint32 FString::ReplaceImpl (FStringView From, FStringView To, ESearchCase Case) noexcept
{
	frt_assert(!From.IsEmpty())
	if (From.IsEmpty())
	{
		return 0u;
	}

	// Arguments viewing this string would change under the rewrite: copy them first.
	FString fromCopy;
	FString toCopy;
	if (PointsInto(From.GetData(), GetData(), GetCapacity()))
	{
		fromCopy = From;
		From = fromCopy.View();
	}
	if (PointsInto(To.GetData(), GetData(), GetCapacity()))
	{
		toCopy = To;
		To = toCopy.View();
	}

	// Pass 1: the count, the result's length, and the most the output ever runs ahead of the input
	// (under IgnoreCase one match may be shorter than To while another is longer).
	const uint32 length = Len();
	uint32 count = 0u;
	uint64 outLength = 0u;
	uint64 lead = 0u;
	for (uint32 start = 0u;;)
	{
		const SStringMatch match = FindForReplace(View(), From, start, Case);
		if (!match)
		{
			outLength += length - start;
			break;
		}
		outLength += (match.Index - start) + To.Len();
		const uint64 consumed = static_cast<uint64>(match.Index) + match.Len;
		if (outLength > consumed && outLength - consumed > lead)
		{
			lead = outLength - consumed;
		}
		++count;
		start = match.Index + match.Len;
	}
	if (count == 0u)
	{
		return 0u;
	}

	const uint32 newLength = string_detail::CheckLength(outLength);
	const uint32 shift = string_detail::CheckLength(lead);
	EnsureCapacity(string_detail::CheckLength(newLength > length + static_cast<uint64>(shift) ? newLength - length : shift));

	// Pass 2, in place: the text moves up by the largest lead, so writing from the front never reaches
	// bytes still to be read. The block stays the same whenever it is large enough.
	char* data = GetData();
	if (shift != 0u)
	{
		std::memmove(data + shift, data, length);
	}
	const FStringView source(data + shift, length);
	uint32 written = 0u;
	for (uint32 start = 0u;;)
	{
		const SStringMatch match = FindForReplace(source, From, start, Case);
		const uint32 segmentEnd = match ? match.Index : length;
		std::memmove(data + written, source.GetData() + start, segmentEnd - start);
		written += segmentEnd - start;
		if (!match)
		{
			break;
		}
		std::memcpy(data + written, To.GetData(), To.Len());
		written += To.Len();
		start = match.Index + match.Len;
	}
	frt_assert(written == newLength)
	SetLength(written);
	return count;
}

uint32 FString::RemoveAllImpl (FStringView Needle, ESearchCase Case) noexcept
{
	return Needle.IsEmpty() ? 0u : ReplaceImpl(Needle, FStringView(), Case);
}


// ---- Numbers and formatting ----

FString& FString::AppendSigned (int64 Value) noexcept
{
	char buffer[24] = {};
	const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), Value);
	return Append(FStringView(buffer, static_cast<size_t>(result.ptr - buffer)));
}

FString& FString::AppendUnsigned (uint64 Value) noexcept
{
	char buffer[24] = {};
	const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), Value);
	return Append(FStringView(buffer, static_cast<size_t>(result.ptr - buffer)));
}

FString& FString::AppendFloat (float Value) noexcept
{
	char buffer[64] = {};
	const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), Value);
	return Append(FStringView(buffer, static_cast<size_t>(result.ptr - buffer)));
}

FString& FString::AppendDouble (double Value) noexcept
{
	char buffer[64] = {};
	const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), Value);
	return Append(FStringView(buffer, static_cast<size_t>(result.ptr - buffer)));
}

FString& FString::AppendFixed (double Value, uint32 MinFractionalDigits, bool bSinglePrecision) noexcept
{
	if (std::isnan(Value))
	{
		return Append("nan");
	}
	if (std::isinf(Value))
	{
		return Append(Value < 0.0 ? "-inf" : "inf");
	}

	// Shortest round trip in fixed notation: the smallest double needs ~330 characters.
	char buffer[512] = {};
	const char* end = buffer + 1;
	if (Value == 0.0)
	{
		// -0 too: a sign on zero only confuses the reader.
		buffer[0] = '0';
	}
	else if (bSinglePrecision)
	{
		end = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(Value), std::chars_format::fixed).ptr;
	}
	else
	{
		end = std::to_chars(buffer, buffer + sizeof(buffer), Value, std::chars_format::fixed).ptr;
	}

	const FStringView digits(buffer, static_cast<size_t>(end - buffer));
	Append(digits);
	if (MinFractionalDigits == 0u)
	{
		return *this;
	}

	const uint32 point = digits.Find('.');
	const uint32 fractional = point == NotFound ? 0u : digits.Len() - point - 1u;
	if (point == NotFound)
	{
		AppendChar('.');
	}
	if (fractional < MinFractionalDigits)
	{
		AppendChars('0', MinFractionalDigits - fractional);
	}
	return *this;
}

FString& FString::VAppendFormat (std::string_view Fmt, std::format_args Args)
{
	// Into a local first: an argument may view this string, and vformat_to reads the arguments while it
	// writes.
	FString formatted;
	std::vformat_to(std::back_inserter(formatted), Fmt, Args);
	if (IsEmpty() && !IsHeapAllocated())
	{
		return *this = std::move(formatted);
	}
	return Append(formatted.View());
}

FString& FString::AppendfImpl (const char* Fmt, ...) noexcept
{
	va_list args;
	va_start(args, Fmt);

	va_list measure;
	va_copy(measure, args);
	const int length = std::vsnprintf(nullptr, 0u, Fmt, measure);
	va_end(measure);

	frt_assert(length >= 0)
	if (length < 0)
	{
		va_end(args);
		return *this;
	}

	// Into a local: a %s argument may point into this string.
	FString formatted;
	formatted.ResizeUninitialized(string_detail::CheckLength(static_cast<uint64>(length)));
	std::vsnprintf(formatted.GetData(), static_cast<size_t>(length) + 1u, Fmt, args);
	va_end(args);

	if (IsEmpty() && !IsHeapAllocated())
	{
		return *this = std::move(formatted);
	}
	return Append(formatted.View());
}


// ---- Case ----

FString& FString::ConvertCase (unicode::ECaseMapping Mapping) noexcept
{
	// In place while the output stays behind the input: ASCII, ill-formed units and mappings that keep or
	// shrink the UTF-8 length. The first one that grows sends the rest into an exactly sized new string.
	char* data = GetData();
	const uint32 length = Len();
	uint32 read = 0u;
	uint32 written = 0u;
	while (read < length)
	{
		const char byte = data[read];
		if (ascii::IsAscii(byte))
		{
			data[written++] = Mapping == unicode::ECaseMapping::Upper ? ascii::ToUpper(byte) : ascii::ToLower(byte);
			++read;
			continue;
		}

		// Ill-formed bytes and WTF-8 surrogates (ill-formed for DecodeUtf8) stay as they are.
		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(data + read, length - read);
		if (!unit.bWellFormed)
		{
			std::memmove(data + written, data + read, unit.Length);
			written += unit.Length;
			read += unit.Length;
			continue;
		}

		char encoded[4] = {};
		const uint32 count = unicode::EncodeUtf8(unicode::tables::LookupMapCase(unit.CodePoint, Mapping), encoded);
		if (written + count > read + unit.Length)
		{
			break;
		}
		std::memcpy(data + written, encoded, count);
		written += count;
		read += unit.Length;
	}

	if (read == length)
	{
		SetLength(written);
		return *this;
	}

	// A mapping grows: measure the rest exactly, then finish into a new string.
	uint64 remaining = 0u;
	for (uint32 i = read; i < length;)
	{
		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(data + i, length - i);
		remaining += unit.bWellFormed ? unicode::Utf8Length(unicode::tables::LookupMapCase(unit.CodePoint, Mapping)) : unit.Length;
		i += unit.Length;
	}

	FString result;
	result.Reserve(string_detail::CheckLength(written + remaining));
	result.Append(FStringView(data, written));
	for (uint32 i = read; i < length;)
	{
		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(data + i, length - i);
		if (unit.bWellFormed)
		{
			result.AppendCodePoint(unicode::tables::LookupMapCase(unit.CodePoint, Mapping));
		}
		else
		{
			result.Append(FStringView(data + i, unit.Length));
		}
		i += unit.Length;
	}
	return *this = std::move(result);
}


// ---- Wide text and paths ----

FString FString::FromWide (const wchar_t* NullTerminated) noexcept
{
	if (NullTerminated == nullptr)
	{
		return FString();
	}
	return FromWide(std::wstring_view(NullTerminated));
}

FString FString::FromWide (const wchar_t* InData, size_t InLen) noexcept
{
	frt_assert(InData != nullptr || InLen == 0u)
	return FromWide(std::wstring_view(InData, InLen));
}

FString FString::FromWide (std::wstring_view Utf16) noexcept
{
	FString result;
	result.AppendWide(Utf16);
	return result;
}

FString& FString::AppendWide (std::wstring_view Utf16) noexcept
{
	if (Utf16.empty())
	{
		return *this;
	}

	const wchar_t* source = Utf16.data();
	size_t sourceLen = Utf16.size();

	// Canonical WTF-8 across the join: a lead surrogate we end with and a trail surrogate the text starts
	// with are one code point, written as its 4-byte sequence.
	if (EndsWithLeadSurrogate(View()) && unicode::IsLowSurrogate(static_cast<uint16>(source[0])))
	{
		const unsigned char* tail = reinterpret_cast<const unsigned char*>(GetData() + Len() - 3u);
		const char32_t lead = 0xD000u | ((tail[1] & 0x3Fu) << 6) | (tail[2] & 0x3Fu);
		const char32_t trail = static_cast<uint16>(source[0]);
		SetLength(Len() - 3u);
		AppendCodePoint(0x10000u + ((lead - 0xD800u) << 10) + (trail - 0xDC00u));
		++source;
		--sourceLen;
	}

	const uint32 length = Len();
	const uint32 extra = string_detail::CheckLength(length + unicode::Wtf8LengthOfUtf16(source, sourceLen)) - length;
	if (length == 0u && !IsHeapAllocated())
	{
		// Construction (FromWide, FromPath): exactly the size, no growth slack.
		Reserve(extra);
	}
	else
	{
		EnsureCapacity(extra);
	}

	const uint32 written = unicode::ConvertUtf16ToWtf8(source, sourceLen, GetData() + length);
	SetLength(length + written);
	return *this;
}

FString FString::FromPath (const std::filesystem::path& Path, EPathStyle Style) noexcept
{
	FString result;
	result.AppendPath(Path, Style);
	return result;
}

FString& FString::AppendPath (const std::filesystem::path& Path, EPathStyle Style) noexcept
{
	const uint32 start = Len();
	AppendWide(Path.native());

	if (Style != EPathStyle::Native)
	{
		// Separators are ASCII, never part of a multi-byte sequence: a byte replacement over the new text
		// (a pair merged at the join holds no separator either).
		const char from = Style == EPathStyle::Generic ? '\\' : '/';
		const char to = Style == EPathStyle::Generic ? '/' : '\\';
		char* data = GetData();
		for (uint32 i = start; i < Len(); ++i)
		{
			if (data[i] == from)
			{
				data[i] = to;
			}
		}
	}
	return *this;
}

FString FString::FromUtf8Lossy (FStringView Bytes) noexcept
{
	if (unicode::IsValidUtf8(Bytes.GetData(), Bytes.Len()))
	{
		return FString(Bytes);
	}

	uint64 length = 0u;
	for (uint32 i = 0u; i < Bytes.Len();)
	{
		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(Bytes.GetData() + i, Bytes.Len() - i);
		length += unit.bWellFormed ? unit.Length : 3u;
		i += unit.Length;
	}

	FString result;
	result.Reserve(string_detail::CheckLength(length));
	for (uint32 i = 0u; i < Bytes.Len();)
	{
		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(Bytes.GetData() + i, Bytes.Len() - i);
		result.Append(unit.bWellFormed ? FStringView(Bytes.GetData() + i, unit.Length) : FStringView(ReplacementCharacterUtf8, 3u));
		i += unit.Length;
	}
	return result;
}


// ---- Other named constructors ----

FString FString::FromHexBytes (std::span<const uint8> Bytes, bool bUpperCase) noexcept
{
	const uint32 length = string_detail::CheckLength(2u * static_cast<uint64>(Bytes.size()));
	FString result;
	result.Reserve(length);
	result.ResizeUninitialized(length);
	char* data = result.GetData();
	for (size_t i = 0u; i < Bytes.size(); ++i)
	{
		data[2u * i] = ascii::HexDigit(Bytes[i] >> 4u, bUpperCase);
		data[2u * i + 1u] = ascii::HexDigit(Bytes[i], bUpperCase);
	}
	return result;
}

FString FString::Escape (FStringView Text) noexcept
{
	FString result;
	result.Reserve(Text.Len());

	const char* data = Text.GetData();
	for (uint32 i = 0u; i < Text.Len();)
	{
		const char byte = data[i];
		if (ascii::IsAscii(byte))
		{
			switch (byte)
			{
			case '\\': result.Append("\\\\"); break;
			case '"': result.Append("\\\""); break;
			case '\n': result.Append("\\n"); break;
			case '\r': result.Append("\\r"); break;
			case '\t': result.Append("\\t"); break;
			case '\0': result.Append("\\0"); break;
			default:
				if (ascii::IsControl(byte))
				{
					AppendHexEscape(result, byte);
				}
				else
				{
					result.AppendChar(byte);
				}
				break;
			}
			++i;
			continue;
		}

		const unicode::SDecodedCodePoint unit = unicode::DecodeUtf8(data + i, Text.Len() - i);
		if (unit.bWellFormed && !IsEscapedCodePoint(unit.CodePoint))
		{
			result.Append(FStringView(data + i, unit.Length));
		}
		else
		{
			for (uint32 k = 0u; k < unit.Length; ++k)
			{
				AppendHexEscape(result, data[i + k]);
			}
		}
		i += unit.Length;
	}
	return result;
}

std::optional<FString> FString::Unescape (FStringView Text) noexcept
{
	FString result;
	result.Reserve(Text.Len());

	const char* data = Text.GetData();
	const uint32 length = Text.Len();
	for (uint32 i = 0u; i < length; ++i)
	{
		if (data[i] != '\\')
		{
			result.AppendChar(data[i]);
			continue;
		}

		if (++i == length)
		{
			return std::nullopt;   // a trailing backslash
		}
		switch (data[i])
		{
		case '\\': result.AppendChar('\\'); break;
		case '"': result.AppendChar('"'); break;
		case '\'': result.AppendChar('\''); break;
		case 'n': result.AppendChar('\n'); break;
		case 'r': result.AppendChar('\r'); break;
		case 't': result.AppendChar('\t'); break;
		case '0': result.AppendChar('\0'); break;
		case 'x':
		{
			if (length - i < 3u)
			{
				return std::nullopt;
			}
			const int32 high = ascii::HexValue(data[i + 1u]);
			const int32 low = ascii::HexValue(data[i + 2u]);
			if (high < 0 || low < 0)
			{
				return std::nullopt;
			}
			result.AppendChar(static_cast<char>((high << 4) | low));
			i += 2u;
			break;
		}
		default:
			return std::nullopt;
		}
	}
	return result;
}

FString FString::Repeat (FStringView Text, uint32 Count) noexcept
{
	FString result;
	result.Reserve(string_detail::CheckLength(static_cast<uint64>(Text.Len()) * Count));
	for (uint32 i = 0u; i < Count; ++i)
	{
		result.Append(Text);
	}
	return result;
}


void PrintTo (const FString& Text, std::ostream* Out)
{
	PrintTo(Text.View(), Out);
}
}
