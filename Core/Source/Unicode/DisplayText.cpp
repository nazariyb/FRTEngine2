#include "Unicode/DisplayText.h"


namespace frt::unicode
{
FString ToDisplayText (FStringView OnDiskSpelling)
{
	if (OnDiskSpelling.IsValidUtf8())
	{
		return FString(OnDiskSpelling);
	}
	// A surrogate code point in WTF-8 is ED A0..BF 80..BF; strict UTF-8 decoding would show it as three
	// U+FFFD (one per maximal subpart), the name has one unit there.
	FString text;
	text.Reserve(OnDiskSpelling.Len() + 8u);
	const uint32 length = OnDiskSpelling.Len();
	for (uint32 i = 0u; i < length;)
	{
		const auto byte = [&OnDiskSpelling] (uint32 Index) { return static_cast<uint8>(OnDiskSpelling[Index]); };
		if (byte(i) == 0xEDu && i + 2u < length && byte(i + 1u) >= 0xA0u && byte(i + 1u) <= 0xBFu && byte(i + 2u) >= 0x80u && byte(i + 2u) <= 0xBFu)
		{
			text.AppendCodePoint(U'\xFFFD');
			i += 3u;
			continue;
		}
		text.AppendChar(OnDiskSpelling[i]);
		++i;
	}
	return text.View().IsValidUtf8() ? text : FString::FromUtf8Lossy(text.View());
}
}
