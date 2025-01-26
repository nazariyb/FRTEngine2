#pragma once

#include "Core.h"

NAMESPACE_FRT_START

namespace math
{
	static constexpr float PI = 3.14159265358979323846f;
	static constexpr float TWO_PI = 6.2831853f;
	static constexpr float PI_OVER_TWO = PI / 2.0f;
	static constexpr float PI_OVER_THREE = PI / 3.0f;
	static constexpr float PI_OVER_FOUR = PI / 4.0f;
	static constexpr float TWO_PI_OVER_FOUR = PI / 6.0f;
	static constexpr float TWO_PI_OVER_TWO = PI / 2.0f;
	static constexpr float TWO_PI_OVER_THREE = PI / 3.0f;

	static constexpr double PI_DOUBLE = 3.14159265358979323846;
	static constexpr double TWO_PI_DOUBLE = 6.2831853;
	static constexpr double PI_OVER_TWO_DOUBLE = PI_DOUBLE / 2.0;
	static constexpr double PI_OVER_THREE_DOUBLE = PI_DOUBLE / 3.0;
	static constexpr double PI_OVER_FOUR_DOUBLE = PI_DOUBLE / 4.0;
	static constexpr double TWO_PI_OVER_FOUR_DOUBLE = PI_DOUBLE / 6.0;
	static constexpr double TWO_PI_OVER_TWO_DOUBLE = PI_DOUBLE / 2.0;
	static constexpr double TWO_PI_OVER_THREE_DOUBLE = PI_DOUBLE / 3.0;

	static constexpr float RadiansToDegrees(float Radians) { return Radians * 180.0f / PI; }
	static constexpr double RadiansToDegrees(double Radians) { return Radians * 180.0 / PI_DOUBLE; }

	template<typename T>
	static T Min(const T& A, const T& B) { return A < B ? A : B; }

	template<typename T>
	static T Max(const T& A, const T& B) { return A > B ? A : B; }

	template<typename TSrc, typename TDst>
	TDst EncodeTwoIntoOne(const TSrc& InA, const TSrc& InB) requires (sizeof(TSrc) * 2 == sizeof(TDst))
	{
		return InA + (((TDst)InB) << (sizeof(TSrc) * 8));
	}

	template<typename TSrc, typename TDst>
	void DecodeTwoFromOne(const TDst& InValue, TSrc& OutA, TSrc& OutB) requires (sizeof(TSrc) * 2 == sizeof(TDst))
	{
		OutA = (TSrc)InValue;
		OutB = (InValue >> (sizeof(TSrc) * 8));
	}

	template<typename T>
	T Clamp(const T& Value, const T& Min, const T& Max)
	{
		return (Value < Min) ? Min : ((Value > Max) ? Max : Value);
	}

	template<typename T, typename TContainer>
	T ClampIndex(const T& Value, const TContainer& Container)
	{
		return Clamp(Value, (T)0, (T)Container.size() - 1);
	}
}

NAMESPACE_FRT_END
