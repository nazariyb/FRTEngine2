#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


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

	// "#RGB", "#RRGGBB" or "#RRGGBBAA" (the '#' optional, either case); White for anything else.
	FRT_CORE_API static SColor FromHex (FStringView Hex);
	// What ToString writes; White for anything else.
	FRT_CORE_API static SColor FromString (FStringView String);

	// "#..." as FromHex, or "RGBA(r, g, b, a)". False, and the color unchanged, for anything else.
	FRT_CORE_API bool FillFromString (FStringView String);

	// "#RRGGBBAA": each channel clamped to [0, 1] and rounded to the nearest of 256 steps.
	FRT_CORE_API FString ToHex () const;
	// "RGBA(r, g, b, a)" with the shortest numbers that read back to the same floats, or ToHex().
	FRT_CORE_API FString ToString (bool bHex = false) const;

	static const SColor White;
	static const SColor Black;
};


inline const SColor SColor::White = SColor(1.0f);
inline const SColor SColor::Black = SColor(0.0f);


struct SColor32
{
	uint32 Color;
};
}
