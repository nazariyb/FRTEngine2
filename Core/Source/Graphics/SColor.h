#pragma once

#include <string_view>

#include "Core.h"
#include "CoreTypes.h"


namespace frt
{
struct SColor
{
	float R, G, B, A;

	SColor () : SColor(1.0f) {}

	SColor (float InR, float InG, float InB, float InA = 1.0f)
		: R(InR)
		, G(InG)
		, B(InB)
		, A(InA) {}

	SColor (float InColor, float Alpha = 1.0f)
		: R(InColor)
		, G(InColor)
		, B(InColor)
		, A(Alpha) {}

	static SColor FromHex (const std::string_view& Hex);
	static SColor FromString (const std::string_view& String);

	bool FillFromString (const std::string_view& String);

	std::string ToHex () const;
	std::string ToString (bool bHex = false) const;

	static const SColor White;
};


inline const SColor SColor::White = SColor(1.0f);


struct SColor32
{
	uint32 Color;
};
}
