#pragma once

#include <optional>
#include <type_traits>

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;


namespace frt::concepts
{
template <typename T>
concept Numerical = std::is_arithmetic_v<T>;

template <typename T>
concept Unsigned = std::is_unsigned_v<T>;

template <typename Type, typename Base>
concept Derived = std::is_base_of_v<Base, Type>;

template <typename T>
concept Enum = std::is_enum_v<T>;


// True when converting TFrom -> TTo drops mantissa bits, i.e. double -> float.
// Floating-point only by design: integral -> floating conversions are a separate
// concern and must not trip the diagnostics built on this.
template <typename TFrom, typename TTo>
concept LosesPrecision =
	std::is_floating_point_v<TFrom>
	&& std::is_floating_point_v<TTo>
	&& (sizeof(TFrom) > sizeof(TTo));


template <typename T>
struct SIsIndexable : std::false_type
{};


template <typename T>
concept Indexable = SIsIndexable<T>::value;
}


using Bool = std::optional<bool>;
