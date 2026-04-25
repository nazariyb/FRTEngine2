#pragma once

#include <string>
#include <type_traits>

#include "Core.h"
#include "CoreTypes.h"


namespace frt::assets
{
// Visitor archive interfaces. Each format (YAML, binary, ...) implements these.
// Asset code is written once via free `Serialize(Archive, T&)` overloads,
// and works against any backend.

class FRT_CORE_API IReadArchive
{
public:
	virtual ~IReadArchive () = default;

	virtual bool Read (const char* Name, bool& V) = 0;
	virtual bool Read (const char* Name, int32& V) = 0;
	virtual bool Read (const char* Name, uint32& V) = 0;
	virtual bool Read (const char* Name, float& V) = 0;
	virtual bool Read (const char* Name, std::string& V) = 0;

	virtual bool Has (const char* Name) const = 0;

	// Group navigation. BeginGroup returns false if the named group is missing.
	// Caller must pair every successful BeginGroup with EndGroup.
	virtual bool BeginGroup (const char* Name) = 0;
	virtual void EndGroup () = 0;
};

class FRT_CORE_API IWriteArchive
{
public:
	virtual ~IWriteArchive () = default;

	virtual void Write (const char* Name, bool V) = 0;
	virtual void Write (const char* Name, int32 V) = 0;
	virtual void Write (const char* Name, uint32 V) = 0;
	virtual void Write (const char* Name, float V) = 0;
	virtual void Write (const char* Name, const std::string& V) = 0;

	virtual void BeginGroup (const char* Name) = 0;
	virtual void EndGroup () = 0;
};

// Generic Read for non-primitive types. Enters a group named `Name`,
// delegates to `Serialize(Ar, V)` (found by ADL), exits the group.
template <typename T>
auto Read (IReadArchive& Ar, const char* Name, T& V)
	-> std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_same_v<T, std::string>, bool>
{
	if (!Ar.BeginGroup(Name))
	{
		return false;
	}

	const bool bOk = Serialize(Ar, V);
	Ar.EndGroup();
	return bOk;
}

template <typename T>
auto Write (IWriteArchive& Ar, const char* Name, const T& V)
	-> std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_same_v<T, std::string>, void>
{
	Ar.BeginGroup(Name);
	Serialize(Ar, V);
	Ar.EndGroup();
}

// Optional primitive read: returns true if value was present.
template <typename T>
bool ReadOptional (IReadArchive& Ar, const char* Name, T& V)
{
	if (!Ar.Has(Name))
	{
		return false;
	}
	return Ar.Read(Name, V);
}
}
