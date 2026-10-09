#include "SColor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

#include "Unicode/Ascii.h"


namespace frt
{
namespace
{
	bool TryParseHexColor (FStringView Hex, SColor& out)
	{
		(void)Hex.ConsumeChar('#');

		std::array<uint8, 4> bytes = { 0, 0, 0, 255 };
		if (Hex.Len() == 3)
		{
			// #RGB: each digit stands for itself twice (#F80 is #FF8800).
			for (uint32 i = 0; i < 3; ++i)
			{
				const int32 digit = ascii::HexValue(Hex[i]);
				if (digit < 0)
				{
					return false;
				}

				bytes[i] = static_cast<uint8>(digit * 17);
			}
		}
		else if (Hex.Len() == 6 || Hex.Len() == 8)
		{
			if (!Hex.DecodeHex(std::span(bytes).first(Hex.Len() / 2)))
			{
				return false;
			}
		}
		else
		{
			return false;
		}

		constexpr float inv = 1.0f / 255.0f;
		out = {
			static_cast<float>(bytes[0]) * inv,
			static_cast<float>(bytes[1]) * inv,
			static_cast<float>(bytes[2]) * inv,
			static_cast<float>(bytes[3]) * inv
		};
		return true;
	}

	// Channel clamped to [0, 1] (NaN to 0) and rounded to the nearest of the 256 values two hex
	// digits hold, so FromHex reads back what ToHex wrote.
	uint32 ToHexByte (float Channel)
	{
		const float clamped = Channel > 0.0f ? std::min(Channel, 1.0f) : 0.0f;
		return static_cast<uint32>(clamped * 255.0f + 0.5f);
	}
}


SColor SColor::FromHex (FStringView Hex)
{
	SColor color;
	if (!TryParseHexColor(Hex, color))
	{
		return {};
	}

	return color;
}

SColor SColor::FromString (FStringView String)
{
	// FillFromString leaves the default (White) alone when String is no color.
	SColor color;
	(void)color.FillFromString(String);
	return color;
}

bool SColor::FillFromString (FStringView String)
{
	if (String.StartsWith('#'))
	{
		return TryParseHexColor(String, *this);
	}

	FStringView channels = String;
	if (!channels.RemoveFromStart("RGBA(") || !channels.RemoveFromEnd(")"))
	{
		return false;
	}

	// Whitespace may surround the numbers and commas; inf and nan are no channel value.
	float rgba[4] = {};
	for (uint32 i = 0; i < 4; ++i)
	{
		channels.TrimStartInline();
		const std::optional<float> value = channels.ConsumeNumber<float>();
		if (!value || !std::isfinite(*value))
		{
			return false;
		}

		rgba[i] = *value;
		channels.TrimStartInline();
		if (i < 3 && !channels.ConsumeChar(','))
		{
			return false;
		}
	}

	if (!channels.IsEmpty())
	{
		return false;
	}

	R = rgba[0];
	G = rgba[1];
	B = rgba[2];
	A = rgba[3];
	return true;
}

FString SColor::ToHex () const
{
	return FString::Format("#{:02X}{:02X}{:02X}{:02X}", ToHexByte(R), ToHexByte(G), ToHexByte(B), ToHexByte(A));
}

FString SColor::ToString (bool bHex) const
{
	if (bHex)
	{
		return ToHex();
	}

	return FString::Format("RGBA({}, {}, {}, {})", R, G, B, A);
}
}
