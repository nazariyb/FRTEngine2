#include "Assets/Yaml/YamlString.h"

#include "Unicode/Unicode.h"


namespace frt::assets
{
bool IsYamlStorable (FStringView Text) noexcept
{
	const char* data = Text.GetData();
	const uint32 length = Text.Len();
	uint32 i = 0u;
	while (i < length)
	{
		// ASCII passes yaml-cpp's decoder unchanged (control characters are written as escapes, which
		// read back as the same bytes).
		if (static_cast<unsigned char>(data[i]) < 0x80u)
		{
			++i;
			continue;
		}

		const unicode::SDecodedCodePoint decoded = unicode::DecodeUtf8(data + i, length - i);
		if (!decoded.bWellFormed || unicode::IsNoncharacter(decoded.CodePoint))
		{
			return false;
		}
		i += decoded.Length;
	}
	return true;
}
}
