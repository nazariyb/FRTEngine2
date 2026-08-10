#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Config/IniFile.h"


namespace frt::config
{
// Weakest to strongest. A key present in a later layer wins over the same key earlier.
enum class EConfigLayer : int32
{
	Engine = 0, // Core/Config/Engine.ini      - shipped defaults, tracked
	Project,    // <project>/Config/Game.ini   - per-project overrides, tracked
	User,       // Local/UserSettings.ini      - written by the running game, untracked
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
 * Like CIniFile this depends on the standard library and Enum.h and nothing else - see the
 * comment there for why that constraint is load-bearing rather than incidental.
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

	bool Has (std::string_view Section, std::string_view Key) const;

	// Drops the User layer's in-memory contents. Does not touch the file until Save().
	void ResetUserLayer ();

	/////////////////////////
	///// Typed access  /////
	/////////////////////////
	//
	// Every Get takes the value to fall back to, so there is no such thing as a read that
	// fails - only one that returns what the caller already had. Values that fail to parse
	// are treated as absent for the same reason.

	bool Get (std::string_view Section, std::string_view Key, bool Default) const;
	int32 Get (std::string_view Section, std::string_view Key, int32 Default) const;
	uint32 Get (std::string_view Section, std::string_view Key, uint32 Default) const;
	float Get (std::string_view Section, std::string_view Key, float Default) const;
	std::string Get (std::string_view Section, std::string_view Key, std::string_view Default) const;
	// Without this overload `Get(S, K, "text")` resolves to the bool one: const char* to
	// bool is a standard conversion and beats const char* to string_view, which is not.
	std::string Get (std::string_view Section, std::string_view Key, const char* Default) const;

	void Set (std::string_view Section, std::string_view Key, bool Value);
	void Set (std::string_view Section, std::string_view Key, int32 Value);
	void Set (std::string_view Section, std::string_view Key, uint32 Value);
	void Set (std::string_view Section, std::string_view Key, float Value);
	void Set (std::string_view Section, std::string_view Key, std::string_view Value);
	// Same trap as the Get above.
	void Set (std::string_view Section, std::string_view Key, const char* Value);

	// Reflected enums round-trip by name, through the reflection already in Enum.h. Never
	// by integer value: an int in a config file silently changes meaning the moment
	// somebody inserts or reorders an enumerator, and the file gives no hint that it did.
	//
	// Scoped enums do not convert to the primitive overloads above, so these are the only
	// candidates for an enum argument and no ambiguity arises.
	template <enum_::ReflectedEnum E>
	E Get (std::string_view Section, std::string_view Key, E Default) const
	{
		const std::string* raw = FindValue(Section, Key);
		if (raw == nullptr)
		{
			return Default;
		}

		// TryParse is case-insensitive by default, matching the rest of the format.
		E parsed{};
		return enum_::TryParse<E>(*raw, &parsed) ? parsed : Default;
	}

	template <enum_::ReflectedEnum E>
	void Set (std::string_view Section, std::string_view Key, E Value)
	{
		// A value outside the reflected set has no name. Writing the empty string is the
		// right degradation - it fails to parse on the next load and falls back to the
		// caller's default - but it means somebody cast an int into the enum, so catch it
		// where it happened rather than at the load two runs later.
		const std::string_view Name = enum_::ToString(Value);
		frt_assert(!Name.empty())

		SetValue(Section, Key, Name);
	}

	// Only exists to turn "no overload matches" into something that says what to do.
	template <concepts::Enum E>
		requires (!enum_::ReflectedEnum<E>)
	E Get (std::string_view Section, std::string_view Key, E Default) const
	{
		static_assert(enum_::ReflectedEnum<E>,
			"Config enums must be reflected so they can round-trip by name - "
			"add FRT_DECLARE_ENUM_REFLECTION for this type.");
		return Default;
	}

private:
	const std::string* FindValue (std::string_view Section, std::string_view Key) const;
	void SetValue (std::string_view Section, std::string_view Key, std::string_view Value);

	CIniFile Layers[static_cast<size_t>(EConfigLayer::Count)];
	std::filesystem::path UserLayerPath;
};
}
