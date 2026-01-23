#pragma once

#include <type_traits>

#include "CoreTypes.h"


namespace frt::flags
{
template <typename T>
struct FlagEnumTag
{};

template <typename T>
struct TIsFlagEnum
{
	static constexpr bool value = requires(FlagEnumTag<T> Tag)
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

	SFlags& operator += (SFlags InFlags);
	SFlags& operator -= (SFlags InFlags);

	SFlags& operator << (SNewFlagState<E> NewFlagState);

	bool operator && (E Flag) const;
	bool operator && (SFlags OtherFlags) const;
	bool operator || (SFlags OtherFlags) const;


	SFlags& AddFlags (SFlags InFlags);
	SFlags& RemoveFlags (SFlags InFlags);

	SFlags& SetFlagState (SNewFlagState<E> NewFlagState);

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
SFlags<E, UnderlyingType>& SFlags<E, UnderlyingType>::operator<< (SNewFlagState<E> NewFlagState)
{
	return SetFlagState(NewFlagState);
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
