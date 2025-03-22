#pragma once

#include "Core.h"
#include <numbers>
#include <type_traits>

#include "CoreTypes.h"

NAMESPACE_FRT_START

namespace math
{
	static constexpr float PI = std::numbers::pi_v<float>;
	static constexpr float TWO_PI = 6.2831853f;
	static constexpr float PI_OVER_TWO = PI / 2.0f;
	static constexpr float PI_OVER_THREE = PI / 3.0f;
	static constexpr float PI_OVER_FOUR = PI / 4.0f;
	static constexpr float TWO_PI_OVER_FOUR = PI / 6.0f;
	static constexpr float TWO_PI_OVER_TWO = PI / 2.0f;
	static constexpr float TWO_PI_OVER_THREE = PI / 3.0f;

	static constexpr double PI_DOUBLE = std::numbers::pi;
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

	template<concepts::Numerical T, concepts::Numerical TIndexType>
	T ClampIndex(const T& Value, const TIndexType& MaxValue)
	{
		return Clamp(Value, (T)0, (T)MaxValue);
	}

	template<concepts::Numerical T, concepts::Indexable TIndexable>
	T ClampIndex(const T& Value, const TIndexable& Container)
	{
		return Clamp(Value, (T)0, (T)Container.GetMaxIndex());
	}

	template<concepts::Unsigned T>
	bool IsPowerOfTwo(const T& Value)
	{
		return Value && !(Value & (Value - 1));
	}

	/**
	 * ...00 0100 0000 -> 6
	 */
	template<concepts::Numerical T>
	uint8 GetIndexOfFirstOneBit(const T& Value)
	{
		if (Value == 0)
		{
			return 1;
		}

		uint64 highestBitIdx = 1;
#if defined(__GNUC__) || defined(__GNUG__) || defined(__clang__)
		if constexpr (sizeof(T) == 8u)
		{
			highestBitIdx = sizeof(T) * 8 - __builtin_ctzll(Value) - 1;
		}
		else
		{
			highestBitIdx = sizeof(T) * 8 - __builtin_ctz(Value) - 1;
		}
#elif defined(_MSC_VER)
		if constexpr (sizeof(T) == 8u)
		{
			_BitScanReverse64((unsigned long*)&highestBitIdx, Value);
		}
		else
		{
			_BitScanReverse((unsigned long*)&highestBitIdx, Value);
		}
#endif

		return highestBitIdx;
	}

	template<concepts::Unsigned T>
	T LowerBoundPowerOfTwo(const T& Value)
	{
		if (IsPowerOfTwo(Value))
		{
			return Value;
		}

		return 1u << GetIndexOfFirstOneBit(Value);
	}

	struct SIndexStrategy
	{
		enum EType : uint8
		{
			IS_Default = 0u,
			IS_CircularClamped,
			IS_Circular,
		};

		template <EType TParadigm>
		static bool IsValid(int64 InValue, int64 MaxValue);

		template <EType TFrom, concepts::Indexable TIndexable>
		static uint64 IsValid(int64 InValue, const TIndexable& Container)
		{
			return IsValid<TFrom>(InValue, Container.GetMaxIndex());
		}

		template <EType TFrom>
		static uint64 ConvertToDefault(int64 InValue, uint64 MaxValue)
		{
			if (!IsValid<TFrom>(InValue, MaxValue))
			{
				return MaxValue;
			}
			return (InValue % (int64)MaxValue + (int64)MaxValue) % MaxValue;
		}

		template <EType TFrom>
		static uint64 ConvertToDefaultPow2(int64 InValue, uint64 MaxValue)
		{
			if (!IsValid<TFrom>(InValue, MaxValue))
			{
				return MaxValue;
			}
			return InValue & (MaxValue - 1);
		}

		template <EType TFrom, concepts::Indexable TIndexable>
		static uint64 ConvertToDefault(int64 InValue, const TIndexable& Container)
		{
			return ConvertToDefault<TFrom>(InValue, Container.Count());
		}

	};

	template <>
	inline bool SIndexStrategy::IsValid<SIndexStrategy::IS_Default>(int64 InValue, int64 MaxValue)
	{
		return InValue >= 0 && InValue <= MaxValue;
	}

	template <>
	constexpr bool SIndexStrategy::IsValid<SIndexStrategy::IS_Circular>(int64 InValue, int64 MaxValue)
	{
		return true;
	}

	template <>
	inline bool SIndexStrategy::IsValid<SIndexStrategy::IS_CircularClamped>(int64 InValue, int64 MaxValue)
	{
		return InValue >= -MaxValue && InValue <= MaxValue;
	}
}

NAMESPACE_FRT_END
