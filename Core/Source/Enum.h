#pragma once

#include <array>
#include <type_traits>
#include <utility>

#include "CoreTypes.h"
#include "Containers/StringView.h"


///
/// === === === Flags === === ===
///

namespace frt::flags
{
template <typename T>
struct FlagEnumTag
{};


template <typename T>
struct TIsFlagEnum
{
	static constexpr bool value = requires (FlagEnumTag<T> Tag)
	{
		frt_flag_enum_marker(Tag);
	};
};


template <typename T>
inline constexpr bool IsFlagEnum = TIsFlagEnum<T>::value;

template <typename T>
concept FlagEnum = frt::concepts::Enum<T> && IsFlagEnum<T>;
}


#define FRT_DECLARE_FLAG_ENUM(EnumType)\
	constexpr void frt_flag_enum_marker(::frt::flags::FlagEnumTag<EnumType>) {}


template <frt::flags::FlagEnum E>
struct SNewFlagState
{
	E Flag;
	bool bNewState;
};


template <frt::flags::FlagEnum E>
SNewFlagState<E> operator + (E Flag)
{
	return { Flag, true };
}

template <frt::flags::FlagEnum E>
SNewFlagState<E> operator - (E Flag)
{
	return { Flag, false };
}

template <frt::flags::FlagEnum E, typename T>
	requires std::is_same_v<std::remove_cvref_t<T>, bool>
SNewFlagState<E> operator + (E Flag, T bNewState)
{
	return { Flag, bNewState };
}


template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType = std::underlying_type_t<E>>
struct SFlags
{
	UnderlyingType Flags = 0;

	SFlags () = default;
	SFlags (E InFlags) : Flags(static_cast<UnderlyingType>(InFlags)) {}

	SFlags& operator += (E InFlag);
	SFlags& operator -= (E InFlag);
	SFlags& operator += (SFlags InFlags);
	SFlags& operator -= (SFlags InFlags);

	SFlags& operator << (SNewFlagState<E> NewFlagState);
	SFlags& operator ^= (E Flag);

	bool operator && (E Flag) const;
	bool operator && (SFlags OtherFlags) const;
	bool operator || (SFlags OtherFlags) const;


	SFlags& AddFlag (E InFlag);
	SFlags& RemoveFlag (E InFlag);
	SFlags& AddFlags (SFlags InFlags);
	SFlags& RemoveFlags (SFlags InFlags);

	SFlags& SetFlagState (SNewFlagState<E> NewFlagState);
	SFlags& ToggleFlagState (E Flag);

	bool HasFlag (E Flag) const;
	bool HasAll (SFlags OtherFlags) const;
	bool HasAny (SFlags OtherFlags) const;
};


template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator+= (SFlags InFlags)
{
	return AddFlags(InFlags);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator-= (SFlags InFlags)
{
	return RemoveFlags(InFlags);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator+= (E InFlag)
{
	return AddFlag(InFlag);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator-= (E InFlag)
{
	return RemoveFlag(InFlag);
}


template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator<< (SNewFlagState<E> NewFlagState)
{
	return SetFlagState(NewFlagState);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator^= (E Flag)
{
	return ToggleFlagState(Flag);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
bool SFlags<E, UnderlyingType>::operator&& (E Flag) const
{
	return HasFlag(Flag);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
bool SFlags<E, UnderlyingType>::operator&& (SFlags OtherFlags) const
{
	return HasAll(OtherFlags);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
bool SFlags<E, UnderlyingType>::operator|| (SFlags OtherFlags) const
{
	return HasAny(OtherFlags);
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::AddFlag (E InFlag)
{
	Flags |= static_cast<UnderlyingType>(InFlag);
	return *this;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::RemoveFlag (E InFlag)
{
	Flags &= ~static_cast<UnderlyingType>(InFlag);
	return *this;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::AddFlags (SFlags InFlags)
{
	Flags |= InFlags.Flags;
	return *this;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::RemoveFlags (SFlags InFlags)
{
	Flags &= ~InFlags.Flags;
	return *this;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::SetFlagState (SNewFlagState<E> NewFlagState)
{
	if (NewFlagState.bNewState)
	{
		AddFlags(NewFlagState.Flag);
	}
	else
	{
		RemoveFlags(NewFlagState.Flag);
	}
	return *this;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::ToggleFlagState (E Flag)
{
	Flags ^= static_cast<UnderlyingType>(Flag);
	return *this;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
bool SFlags<E, UnderlyingType>::HasFlag (E Flag) const
{
	return (Flags & static_cast<UnderlyingType>(Flag)) != 0u;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
bool SFlags<E, UnderlyingType>::HasAll (SFlags OtherFlags) const
{
	return (Flags & OtherFlags.Flags) == OtherFlags.Flags;
}

template <frt::flags::FlagEnum E, frt::concepts::Unsigned UnderlyingType>
bool SFlags<E, UnderlyingType>::HasAny (SFlags OtherFlags) const
{
	return (Flags & OtherFlags.Flags) != 0u;
}


///
/// === === === Reflection === === ===
///

namespace frt::enum_
{
template <typename E>
struct TEnumTraits
{
	static constexpr bool bIsDefined = false;
};


template <typename E>
concept ReflectedEnum = frt::concepts::Enum<E> && TEnumTraits<E>::bIsDefined;

// Empty for a value outside the reflected set.
template <ReflectedEnum E>
constexpr FStringLiteral ToString (E Value)
{
	for (const auto& entry : TEnumTraits<E>::Entries)
	{
		if (entry.first == Value)
		{
			return entry.second;
		}
	}
	return {};
}

// Names are C++ identifiers: ignoring case is A-Z == a-z, as for INI keys and the command line.
template <ReflectedEnum E>
bool TryParse (FStringView Text, E* OutValue, bool bIgnoreCase = true)
{
	for (const auto& entry : TEnumTraits<E>::Entries)
	{
		if (bIgnoreCase ? Text.Equals<ESearchCase::IgnoreCaseAscii>(entry.second) : Text == entry.second)
		{
			*OutValue = entry.first;
			return true;
		}
	}
	return false;
}

template <ReflectedEnum E>
constexpr bool IsValid (E Value)
{
	for (const auto& entry : TEnumTraits<E>::Entries)
	{
		if (entry.first == Value)
		{
			return true;
		}
	}
	return false;
}

template <ReflectedEnum E, size_t... I>
constexpr auto GetValueNamesImpl (
	std::index_sequence<I...>)
	-> std::array<FStringLiteral, sizeof...(I)>
{
	return { TEnumTraits<E>::Entries[I].second... };
}

template <ReflectedEnum E>
constexpr auto GetValueNames ()
	-> std::array<FStringLiteral, TEnumTraits<E>::Entries.size()>
{
	return GetValueNamesImpl<E>(std::make_index_sequence<TEnumTraits<E>::Entries.size()>{});
}

template <concepts::Enum E>
constexpr E NextValue (E Value, E Count)
{
	using U = std::underlying_type_t<E>;
	return static_cast<E>((static_cast<U>(Value) + 1) % static_cast<U>(Count));
}

template <ReflectedEnum E>
constexpr E NextValue (E Value)
{
	return NextValue(Value, E::Count);
}
}


// The literal becomes an FStringLiteral here, not inside std::pair's constructor: MSVC does not
// take a forwarded parameter as the constant its consteval constructor needs.
#define FRT_ENUM_ENTRY(EnumType, Value)\
std::pair<EnumType, ::frt::FStringLiteral>{ EnumType::Value, ::frt::FStringLiteral(#Value) }

#define FRT_ENUM_ENTRY_NAME(EnumType, Value, Name)\
std::pair<EnumType, ::frt::FStringLiteral>{ EnumType::Value, ::frt::FStringLiteral(Name) }

#define FRT_DECLARE_ENUM_REFLECTION(EnumType, ...)\
namespace frt::enum_ {\
template <> struct TEnumTraits<EnumType>\
{\
	static constexpr auto Entries = std::to_array<std::pair<EnumType, ::frt::FStringLiteral>>({ __VA_ARGS__ });\
	static constexpr bool bIsDefined = true;\
};\
}
