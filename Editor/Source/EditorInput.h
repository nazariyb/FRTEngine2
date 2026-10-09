#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "CoreTypes.h"
#include "Enum.h"
#include "Config/IniFile.h"
#include "Input/InputChord.h"

#include "EditorCommands.h"


namespace frt::input
{
class CInputSystem;
}


namespace frt::editor
{
// Keys that move a viewport camera while held (Unreal's FViewportNavigationCommands).
enum class EViewportKey : uint8
{
	Forward,
	Backward,
	Left,
	Right,
	WorldUp,              // along the world's up axis
	WorldDown,
	LocalUp,              // along the camera's up axis
	LocalDown,
	FovZoomOut,           // widen the field of view while held; it springs back after
	FovZoomIn,
	Count
};

// When a viewport key moves the camera (Unreal's FlightCameraControlType WASD_RMBOnly, and its
// arrow / numpad keys that need no mouse button).
enum class EViewportKeySet : uint8
{
	Flight,               // while a mouse button navigates the viewport
	Keyboard,             // whenever the viewport has keyboard focus
	Count
};

// How the Scene camera responds - the part of Unreal's ULevelEditorViewportSettings that applies.
struct SViewportCameraSettings
{
	float Speed = 1.0f;                   // camera speed at start-up (Unreal's default 1)
	float MouseSensitivity = 0.2f;        // degrees per mouse count: look, LMB turn, orbit
	float WheelDollyDistance = 0.96f;     // metres per wheel notch, whatever the speed
	bool bInvertMouseLookY = false;
	bool bInvertOrbitY = false;
	bool bInvertMiddleMousePan = false;
	bool bInvertRightMouseDollyY = false;
};

/**
 * The editor's input: which key chords run which command, which keys move the viewport camera,
 * and how the camera responds. Plain bindings, read from two files with the same layout:
 *   - Editor/Config/EditorInput.ini       the shipped defaults (tracked), every binding listed;
 *   - <engine>/Local/Editor/EditorInput.ini  the user's overrides (untracked; the engine
 *                                         checkout's Local/, never a project's), key by key - a
 *                                         key present there wins, an empty value unbinds.
 * Both are re-read when either changes on disk, so a binding can be tried while the editor
 * runs. Problems - unknown names, chords that do not parse - are logged and listed, and never
 * stop the editor: a bad entry is simply skipped.
 *
 * Mouse navigation is fixed, as in Unreal; only its settings are configurable.
 */
class CEditorInput
{
public:
	void Load (const std::filesystem::path& DefaultsPath, const std::filesystem::path& UserPath);
	// Re-reads both files if either changed since the last read. Polls a few times a second.
	bool ReloadIfChanged ();
	// The next ReloadIfChanged checks the files whenever it last did (after writing one).
	void PollNow ();

	// ---- commands ----
	// A chord bound to the command was pressed this frame, with exactly its modifiers.
	bool WasCommandPressed (EEditorCommand Command, const input::CInputSystem& Input) const;
	const std::vector<input::SInputChord>& GetChords (EEditorCommand Command) const;
	// The first chord, as menus show it ("Ctrl+P"); empty when the command is unbound.
	const std::string& GetShortcutText (EEditorCommand Command) const;

	// ---- viewport camera ----
	// A key bound to Key in Set is held. Modifiers are not part of navigation keys.
	bool IsKeyDown (EViewportKeySet Set, EViewportKey Key, const input::CInputSystem& Input) const;
	const SViewportCameraSettings& GetCameraSettings () const { return Camera; }

	// ---- state ----
	bool HasDefaults () const { return bHasDefaults; }
	// What was wrong with the files at the last read; empty when all is well.
	const std::vector<std::string>& GetProblems () const { return Problems; }
	// Bumped by every read, initial or reload.
	uint32 GetGeneration () const { return Generation; }
	const std::filesystem::path& GetUserPath () const { return User.Path; }

private:
	struct SLayer
	{
		std::filesystem::path Path;
		config::CIniFile File;
		std::filesystem::file_time_type WriteTime = {};
		bool bExists = false;
	};

	void Read ();
	void Validate ();
	const FString* Find (std::string_view Section, std::string_view Key) const;
	std::vector<input::SInputChord> ReadChords (std::string_view Section, std::string_view Key, bool bKeysOnly);
	void Report (std::string Problem);

	SLayer Defaults;
	SLayer User;
	std::chrono::steady_clock::time_point LastPoll = {};

	std::array<std::vector<input::SInputChord>, static_cast<size_t>(EEditorCommand::Count)> CommandChords;
	std::array<std::string, static_cast<size_t>(EEditorCommand::Count)> ShortcutTexts;
	std::array<std::array<std::vector<input::KeyCode>, static_cast<size_t>(EViewportKey::Count)>, static_cast<size_t>(EViewportKeySet::Count)> ViewportKeys;
	SViewportCameraSettings Camera;

	bool bHasDefaults = false;
	std::vector<std::string> Problems;
	uint32 Generation = 0u;
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::editor::EViewportKey,
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, Forward),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, Backward),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, Left),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, Right),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, WorldUp),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, WorldDown),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, LocalUp),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, LocalDown),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, FovZoomOut),
	FRT_ENUM_ENTRY(frt::editor::EViewportKey, FovZoomIn)
);
