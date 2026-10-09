#pragma once

#include <cstddef>
#include <string>
#include <type_traits>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::assets
{
// Visitor archive interfaces. Each format (YAML, binary, ...) implements these.
// Asset code is written once via free `Serialize(Archive, T&)` overloads,
// and works against any backend.
//
// Keys are FStringLiteral: they are part of the file format, so they are literals in the code.

class FRT_CORE_API IReadArchive
{
public:
	virtual ~IReadArchive () = default;

	virtual bool Read (FStringLiteral Name, bool& V) = 0;
	virtual bool Read (FStringLiteral Name, int32& V) = 0;
	virtual bool Read (FStringLiteral Name, uint32& V) = 0;
	virtual bool Read (FStringLiteral Name, float& V) = 0;
	virtual bool Read (FStringLiteral Name, FString& V) = 0;

	// For callers whose text is still std::string (the material serializers): an out-parameter
	// has no implicit conversion. V is left alone when the read fails, as for the others.
	bool Read (FStringLiteral Name, std::string& V)
	{
		FString value;
		if (!Read(Name, value))
		{
			return false;
		}
		V = value.ToStdString();
		return true;
	}

	virtual bool Has (FStringLiteral Name) const = 0;

	// Group navigation. BeginGroup returns false if the named group is missing.
	// Caller must pair every successful BeginGroup with EndGroup.
	virtual bool BeginGroup (FStringLiteral Name) = 0;
	virtual void EndGroup () = 0;
};

class FRT_CORE_API IWriteArchive
{
public:
	virtual ~IWriteArchive () = default;

	virtual void Write (FStringLiteral Name, bool V) = 0;
	virtual void Write (FStringLiteral Name, int32 V) = 0;
	virtual void Write (FStringLiteral Name, uint32 V) = 0;
	virtual void Write (FStringLiteral Name, float V) = 0;
	virtual void Write (FStringLiteral Name, FStringView V) = 0;

	// A C string is text. Without these overloads, pointer -> bool (a standard conversion) would beat
	// every route to FStringView: Write("cull", CullModeToString(Mode)) would write `cull: true`, and
	// Write("name", u8"...") `name: true`.
	void Write (FStringLiteral Name, const char* V) { Write(Name, FStringView(V)); }
	void Write (FStringLiteral Name, const char8_t* V) { Write(Name, FStringView(V)); }
	void Write (FStringLiteral, std::nullptr_t) = delete;
	// Any other pointer (wide text, void*, a pointer to a string object) is no text an archive stores,
	// and would reach bool the same way.
	template <typename T>
	void Write (FStringLiteral, const T*) = delete;

	virtual void BeginGroup (FStringLiteral Name) = 0;
	// Same as BeginGroup but request inline / flow style for the group when the backend
	// supports it (e.g. YAML emits `{ r: 1, g: 0, b: 0, a: 1 }` instead of multi-line block).
	// Backends without a notion of style fall through to BeginGroup.
	virtual void BeginGroupFlow (FStringLiteral Name) { BeginGroup(Name); }
	virtual void EndGroup () = 0;
};

// Generic Read for non-primitive types. Enters a group named `Name`,
// delegates to `Serialize(Ar, V)` (found by ADL), exits the group.
// Text of every kind goes to the archive's own overloads, never into a group.
template <typename T>
auto Read (IReadArchive& Ar, FStringLiteral Name, T& V)
	-> std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_convertible_v<const T&, FStringView>, bool>
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
auto Write (IWriteArchive& Ar, FStringLiteral Name, const T& V)
	-> std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_convertible_v<const T&, FStringView>, void>
{
	Ar.BeginGroup(Name);
	Serialize(Ar, V);
	Ar.EndGroup();
}

// Same as Write, but emits the group inline (flow style) when the backend supports it.
template <typename T>
auto WriteFlow (IWriteArchive& Ar, FStringLiteral Name, const T& V)
	-> std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_convertible_v<const T&, FStringView>, void>
{
	Ar.BeginGroupFlow(Name);
	Serialize(Ar, V);
	Ar.EndGroup();
}

// Optional primitive read: returns true if value was present.
template <typename T>
bool ReadOptional (IReadArchive& Ar, FStringLiteral Name, T& V)
{
	if (!Ar.Has(Name))
	{
		return false;
	}
	return Ar.Read(Name, V);
}
}
