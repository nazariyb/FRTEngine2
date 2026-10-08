#include "Containers/WideString.h"

#include <cwchar>

#include "Unicode/Unicode.h"


namespace frt
{
FWideString::FWideString (FStringView Utf8) noexcept
	: FWideString()
{
	// Our own transcoder, not MultiByteToWideChar: Win32 turns WTF-8 surrogates into U+FFFD, which would
	// break the path -> FString -> path round trip.
	wchar_t* data = Prepare(unicode::Utf16LengthOfWtf8(Utf8.GetData(), Utf8.Len()));
	unicode::ConvertWtf8ToUtf16(Utf8.GetData(), Utf8.Len(), data);
}

FWideString::FWideString (std::wstring_view Utf16) noexcept
	: FWideString()
{
	std::wmemcpy(Prepare(Utf16.size()), Utf16.data(), Utf16.size());
}

wchar_t* FWideString::Prepare (uint64 Units) noexcept
{
	if (Units > MaxLength)
	{
		string_detail::FatalLengthError();
	}

	const uint32 units = static_cast<uint32>(Units);
	if (units > Capacity)
	{
		ReleaseBlock();
		Data = static_cast<wchar_t*>(string_detail::Allocate((static_cast<size_t>(units) + 1u) * sizeof(wchar_t)));
		Capacity = units;
	}
	Size = units;
	Data[units] = 0;
	return Data;
}

FWideString ToWide (FStringView Utf8) noexcept
{
	return FWideString(Utf8);
}
}
