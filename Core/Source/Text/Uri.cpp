#include "Text/Uri.h"

#include <span>


namespace frt::text
{
std::optional<FString> DecodeUriEscapes (FStringView Uri)
{
	FString decoded;
	uint32 copied = 0u;         // Uri[0, copied) is in decoded
	for (uint32 i = Uri.Find('%'); i != FStringView::NotFound; i = Uri.Find('%', i + 1u))
	{
		uint8 byte = 0u;
		if (!Uri.Mid(i + 1u, 2u).DecodeHex(std::span(&byte, 1u)))
		{
			continue;
		}
		if (byte == 0u)
		{
			return std::nullopt;
		}
		decoded.Append(Uri.Mid(copied, i - copied)).AppendChar(static_cast<char>(byte));
		i += 2u;
		copied = i + 1u;
	}

	if (copied == 0u)
	{
		return std::nullopt;
	}
	decoded.Append(Uri.RightChop(copied));
	return decoded;
}
}
