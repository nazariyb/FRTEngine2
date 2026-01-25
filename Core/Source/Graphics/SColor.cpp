#include "SColor.h"

#include <cctype>
#include <sstream>


namespace frt
{
namespace
{
	int HexValue (char c)
	{
		if (c >= '0' && c <= '9')
		{
			return c - '0';
		}

		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		if (c >= 'A' && c <= 'F')
		{
			return 10 + (c - 'A');
		}

		return -1;
	}

	bool ParseHexByte (const std::string_view& hex, size_t index, uint8& out)
	{
		const int hi = HexValue(hex[index]);
		const int lo = HexValue(hex[index + 1]);
		if (hi < 0 || lo < 0)
		{
			return false;
		}

		out = static_cast<uint8>((hi << 4) | lo);
		return true;
	}

	bool ParseFloat (std::stringstream& stream, float& out, bool bExpectComma)
	{
		stream >> out;
		if (!stream)
		{
			return false;
		}

		stream >> std::ws;
		if (bExpectComma)
		{
			if (stream.peek() != ',')
			{
				return false;
			}

			stream.get();
		}

		return true;
	}

	bool TryParseHexColor (const std::string_view& Hex, SColor& out)
	{
		std::string_view hex = Hex;
		if (hex.empty())
		{
			return false;
		}

		if (hex.front() == '#')
		{
			hex.remove_prefix(1);
		}

		uint8 r = 0;
		uint8 g = 0;
		uint8 b = 0;
		uint8 a = 255;

		if (hex.size() == 3)
		{
			const int rh = HexValue(hex[0]);
			const int gh = HexValue(hex[1]);
			const int bh = HexValue(hex[2]);
			if (rh < 0 || gh < 0 || bh < 0)
			{
				return false;
			}

			r = static_cast<uint8>(rh * 17);
			g = static_cast<uint8>(gh * 17);
			b = static_cast<uint8>(bh * 17);
		}
		else if (hex.size() == 6 || hex.size() == 8)
		{
			if (!ParseHexByte(hex, 0, r)
				|| !ParseHexByte(hex, 2, g)
				|| !ParseHexByte(hex, 4, b))
			{
				return false;
			}

			if (hex.size() == 8 && !ParseHexByte(hex, 6, a))
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
			static_cast<float>(r) * inv,
			static_cast<float>(g) * inv,
			static_cast<float>(b) * inv,
			static_cast<float>(a) * inv
		};
		return true;
	}
}


SColor SColor::FromHex (const std::string_view& Hex)
{
	SColor color;
	if (!TryParseHexColor(Hex, color))
	{
		return {};
	}

	return color;
}

SColor SColor::FromString (const std::string_view& String)
{
	SColor color;
	if (color.FillFromString(String))
	{
		return color;
	}

	return color;
}

bool SColor::FillFromString (const std::string_view& String)
{
	if (String.starts_with("#"))
	{
		return TryParseHexColor(String, *this);
	}

	if (String.starts_with("RGBA("))
	{
		if (!String.ends_with(")"))
		{
			return false;
		}

		const std::string_view channels = String.substr(5, String.size() - 6);
		std::stringstream ss{ std::string(channels) };

		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
		float a = 0.0f;

		if (!ParseFloat(ss, r, true)
			|| !ParseFloat(ss, g, true)
			|| !ParseFloat(ss, b, true)
			|| !ParseFloat(ss, a, false))
		{
			return false;
		}

		ss >> std::ws;
		if (ss.peek() != std::char_traits<char>::eof())
		{
			return false;
		}

		R = r;
		G = g;
		B = b;
		A = a;
		return true;
	}

	return false;
}

std::string SColor::ToHex () const
{
	std::stringstream ss;
	ss << "#" << std::hex << R << G << B;
	return ss.str();
}

std::string SColor::ToString (bool bHex) const
{
	if (bHex)
	{
		return ToHex();
	}

	std::stringstream ss;
	ss << "RGBA(" << R << ", " << G << ", " << B << ", " << A << ")";
	return ss.str();
}
}
