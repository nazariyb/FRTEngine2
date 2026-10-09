#pragma once

#include <concepts>
#include <filesystem>
#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Config/IniFile.h"
#include "Containers/String.h"


namespace frt::config
{
// Weakest to strongest. A key present in a later layer wins over the same key earlier.
enum class EConfigLayer : int32
{
	Engine = 0, // Core/Config/Engine.ini      - shipped defaults, tracked
	Project,    // <project>/Config/Game.ini   - per-project overrides, tracked
	User,       // <project>/Local/UserSettings.ini - written by the running game, untracked
	CommandLine,// -Section:Key=Value arguments  - never saved
	Count,
};


/**
 * The layered config stack, and the first thing the engine loads.
 *
 * Lookup walks strongest layer to weakest and stops at the first hit. A key absent from
 * every layer falls through to the default the caller passed, so config is never required
 * to be complete: a missing file, a stale file, or a file with a typo in it degrades to
 * compiled-in defaults instead of failing startup.
 *
 * Writes always land in the User layer. That keeps the two tracked files hand-authored and
 * means Save() only ever rewrites the one machine-generated file.
 *
 * Like CIniFile this depends on the standard library, the string types and Enum.h and nothing
 * else - see the comment there for why that constraint is load-bearing rather than incidental.
 */
class FRT_CORE_API CConfig
{
public:
	// Loads all three layers from the locations in EnginePaths. Absent files are skipped
	// silently: the User layer does not exist on a first run, and the other two are
	// optional by design.
	void LoadDefaultLayers ();

	// Returns false when the file was absent or unreadable, which is not usually an error
	// - see LoadDefaultLayers. Loading the User layer also records Path as the Save target.
	bool LoadLayer (EConfigLayer Layer, const std::filesystem::path& Path);

	// Rewrites the User layer to the path it was loaded from. False when no path is known
	// or the write failed.
	bool Save () const;

	void SetUserLayerPath (const std::filesystem::path& Path) { UserLayerPath = Path; }
	const std::filesystem::path& GetUserLayerPath () const { return UserLayerPath; }

	bool Has (FStringView Section, FStringView Key) const;

	// Drops the User layer's in-memory contents. Does not touch the file until Save().
	void ResetUserLayer ();

	// Strongest layer, never written to disk. Used for "-Section:Key=Value" launch arguments
	// (automation, profiling, A/B comparisons) so they cannot leak into UserSettings.ini.
	void SetOverride (FStringView Section, FStringView Key, FStringView Value);

	// Parses arguments of the form -Section:Key=Value (or --Section:Key=Value). Others are
	// ignored. Returns the number of overrides applied.
	uint32 ApplyCommandLineArguments (std::span<const FString> Arguments);

	/////////////////////////
	///// Typed access  /////
	/////////////////////////
	//
	// Every Get takes the value to fall back to, so there is no such thing as a read that
	// fails - only one that returns what the caller already had. Values that fail to parse
	// are treated as absent for the same reason.
	//
	// The bool overloads take exactly bool: as plain overloads they would also take
	// Get(S, K, "text"), since const char* to bool is a standard conversion and beats
	// const char* to FStringView, which is not.

	// The spellings people actually type: true/1/yes/on, false/0/no/off, in any case. Anything
	// else is treated as absent rather than as false, so a typo keeps the default instead of
	// silently disabling a feature.
	template <typename T> requires std::same_as<T, bool>
	bool Get (FStringView Section, FStringView Key, T Default) const
	{
		const FString* raw = FindValue(Section, Key);
		return raw != nullptr ? raw->ToBool(Default) : Default;
	}
	int32 Get (FStringView Section, FStringView Key, int32 Default) const;
	uint32 Get (FStringView Section, FStringView Key, uint32 Default) const;
	float Get (FStringView Section, FStringView Key, float Default) const;
	FString Get (FStringView Section, FStringView Key, FStringView Default) const;

	template <typename T> requires std::same_as<T, bool>
	void Set (FStringView Section, FStringView Key, T Value)
	{
		SetValue(Section, Key, Value ? "true" : "false");
	}
	void Set (FStringView Section, FStringView Key, int32 Value);
	void Set (FStringView Section, FStringView Key, uint32 Value);
	void Set (FStringView Section, FStringView Key, float Value);
	void Set (FStringView Section, FStringView Key, FStringView Value);

	// Reflected enums round-trip by name, through the reflection already in Enum.h. Never
	// by integer value: an int in a config file silently changes meaning the moment
	// somebody inserts or reorders an enumerator, and the file gives no hint that it did.
	//
	// Scoped enums do not convert to the primitive overloads above, so these are the only
	// candidates for an enum argument and no ambiguity arises.
	template <enum_::ReflectedEnum E>
	E Get (FStringView Section, FStringView Key, E Default) const
	{
		const FString* raw = FindValue(Section, Key);
		if (raw == nullptr)
		{
			return Default;
		}

		// TryParse is case-insensitive by default, matching the rest of the format.
		E parsed{};
		return enum_::TryParse<E>(*raw, &parsed) ? parsed : Default;
	}

	template <enum_::ReflectedEnum E>
	void Set (FStringView Section, FStringView Key, E Value)
	{
		// A value outside the reflected set has no name. Writing the empty string is the
		// right degradation - it fails to parse on the next load and falls back to the
		// caller's default - but it means somebody cast an int into the enum, so catch it
		// where it happened rather than at the load two runs later.
		const FStringLiteral name = enum_::ToString(Value);
		frt_assert(!name.IsEmpty())

		SetValue(Section, Key, name);
	}

	// Only exists to turn "no overload matches" into something that says what to do.
	template <concepts::Enum E>
		requires (!enum_::ReflectedEnum<E>)
	E Get (FStringView Section, FStringView Key, E Default) const
	{
		static_assert(enum_::ReflectedEnum<E>,
			"Config enums must be reflected so they can round-trip by name - "
			"add FRT_DECLARE_ENUM_REFLECTION for this type.");
		return Default;
	}

private:
	const FString* FindValue (FStringView Section, FStringView Key) const;
	void SetValue (FStringView Section, FStringView Key, FStringView Value);

	CIniFile Layers[static_cast<size_t>(EConfigLayer::Count)];
	std::filesystem::path UserLayerPath;
};
}
