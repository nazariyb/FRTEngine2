#include "EditorInput.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <system_error>

#include "Input/InputSystem.h"


namespace frt::editor
{
namespace
{
constexpr FStringLiteral CommandsSection = "Commands";
constexpr FStringLiteral CameraSection = "ViewportCamera";
constexpr FStringLiteral KeySetSections[] = { "FlightKeys", "KeyboardKeys" };
constexpr FStringLiteral CameraKeys[] =
{
	"Speed", "MouseSensitivity", "WheelDollyDistance",
	"InvertMouseLookY", "InvertOrbitY", "InvertMiddleMousePan", "InvertRightMouseDollyY",
};

// How often the files' timestamps are checked.
constexpr std::chrono::milliseconds PollInterval(250);

std::filesystem::file_time_type GetWriteTime (const std::filesystem::path& Path, bool& bOutExists)
{
	std::error_code ec;
	bOutExists = std::filesystem::exists(Path, ec) && !ec;
	if (!bOutExists)
	{
		return {};
	}
	const std::filesystem::file_time_type time = std::filesystem::last_write_time(Path, ec);
	return ec ? std::filesystem::file_time_type() : time;
}
}

void CEditorInput::Load (const std::filesystem::path& DefaultsPath, const std::filesystem::path& UserPath)
{
	Defaults.Path = DefaultsPath;
	User.Path = UserPath;
	Read();
	LastPoll = std::chrono::steady_clock::now();
}

void CEditorInput::PollNow ()
{
	LastPoll = {};
}

bool CEditorInput::ReloadIfChanged ()
{
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (now - LastPoll < PollInterval)
	{
		return false;
	}
	LastPoll = now;

	const auto changed = [] (const SLayer& Layer)
	{
		bool bExists = false;
		const std::filesystem::file_time_type time = GetWriteTime(Layer.Path, bExists);
		return bExists != Layer.bExists || (bExists && time != Layer.WriteTime);
	};
	if (!changed(Defaults) && !changed(User))
	{
		return false;
	}
	Read();
	return true;
}

const FString* CEditorInput::Find (FStringView Section, FStringView Key) const
{
	if (const FString* value = User.File.Find(Section, Key))
	{
		return value;
	}
	return Defaults.File.Find(Section, Key);
}

void CEditorInput::Report (FString Problem)
{
	std::printf("[input] %s\n", Problem.CStr());
	Problems.push_back(std::move(Problem));
}

std::vector<input::SInputChord> CEditorInput::ReadChords (FStringView Section, FStringView Key, bool bKeysOnly)
{
	std::vector<input::SInputChord> chords;
	const FString* value = Find(Section, Key);
	if (!value)
	{
		// Every binding is listed in the shipped file, unbound ones with an empty value: a
		// missing one means the file is older than the code.
		if (bHasDefaults)
		{
			Report(FString::Format("[{}] {} is missing from {}", Section, Key, FString::FromPath(Defaults.Path.filename())));
		}
		return chords;
	}

	FString error;
	if (!input::ParseInputChords(*value, chords, &error))
	{
		Report(FString::Format("[{}] {}: cannot read {}", Section, Key, error));
	}
	// A modifier key alone can never match: holding it is what makes it a modifier.
	const auto isModifier = [] (const input::SInputChord& Chord)
	{
		using input::KeyCode;
		switch (Chord.Key)
		{
			case KeyCode::LeftShift: case KeyCode::RightShift:
			case KeyCode::LeftCtrl: case KeyCode::RightCtrl:
			case KeyCode::LeftAlt: case KeyCode::RightAlt:
			case KeyCode::LeftMeta: case KeyCode::RightMeta:
				return true;
			default:
				return false;
		}
	};
	for (auto it = chords.begin(); it != chords.end();)
	{
		if (isModifier(*it))
		{
			Report(FString::Format("[{}] {}: {} is a modifier and cannot be bound on its own", Section, Key, input::FormatInputChord(*it)));
			it = chords.erase(it);
		}
		else
		{
			++it;
		}
	}
	if (bKeysOnly)
	{
		for (input::SInputChord& chord : chords)
		{
			if (chord.Modifiers.Flags != 0u)
			{
				Report(FString::Format("[{}] {}: {} - navigation keys take no modifiers; using the key alone",
					Section, Key, input::FormatInputChord(chord)));
				chord.Modifiers = input::EInputModifier::None;
			}
		}
	}
	return chords;
}

void CEditorInput::Read ()
{
	Problems.clear();

	// The timestamp is taken before the content: a write that lands while the file is read
	// leaves a newer timestamp behind, and the next poll reads it again.
	const auto load = [this] (SLayer& Layer)
	{
		bool bExists = false;
		Layer.WriteTime = GetWriteTime(Layer.Path, bExists);
		const bool bLoaded = bExists && Layer.File.LoadFromFile(Layer.Path);
		if (!bLoaded)
		{
			Layer.File.Clear();
		}
		if (bExists && !bLoaded)
		{
			// Locked by its writer, say. Recorded as absent, so the next poll tries again.
			Report(FString::Format("cannot open {}; trying again", FString::FromPath(Layer.Path)));
		}
		Layer.bExists = bLoaded;
		return bLoaded;
	};
	bHasDefaults = load(Defaults);
	// The user file is optional: most users never override anything.
	load(User);
	if (!bHasDefaults)
	{
		Report(FString::Format("no editor input defaults at {}: nothing is bound", FString::FromPath(Defaults.Path)));
	}

	for (size_t i = 0u; i < CommandChords.size(); ++i)
	{
		const FStringLiteral name = enum_::ToString(static_cast<EEditorCommand>(i));
		CommandChords[i] = ReadChords(CommandsSection, name, false);
		ShortcutTexts[i] = CommandChords[i].empty() ? FString() : input::FormatInputChord(CommandChords[i].front());
	}

	for (size_t set = 0u; set < ViewportKeys.size(); ++set)
	{
		for (size_t key = 0u; key < ViewportKeys[set].size(); ++key)
		{
			const std::vector<input::SInputChord> chords = ReadChords(KeySetSections[set], enum_::ToString(static_cast<EViewportKey>(key)), true);
			ViewportKeys[set][key].clear();
			for (const input::SInputChord& chord : chords)
			{
				ViewportKeys[set][key].push_back(chord.Key);
			}
		}
	}

	Camera = SViewportCameraSettings();
	// Trimmed as the chords are (any White_Space; the INI trims only ASCII's), so a no-break
	// space pasted after a value does not turn it into an error.
	const auto readFloat = [this] (FStringLiteral Key, float& Value, float Min, float Max)
	{
		if (const FString* text = Find(CameraSection, Key))
		{
			// ToFloat also reads "nan" and "inf", which no setting can take.
			const std::optional<float> value = text->View().TrimStartAndEnd().ToFloat();
			if (value && std::isfinite(*value))
			{
				Value = *value;
			}
			else
			{
				Report(FString::Format("[{}] {}: \"{}\" is not a number", CameraSection, Key, *text));
			}
		}
		Value = std::clamp(Value, Min, Max);
	};
	const auto readBool = [this] (FStringLiteral Key, bool& Value)
	{
		if (const FString* text = Find(CameraSection, Key))
		{
			if (const std::optional<bool> value = text->View().TrimStartAndEnd().TryParseBool())
			{
				Value = *value;
			}
			else
			{
				Report(FString::Format("[{}] {}: \"{}\" is not true or false", CameraSection, Key, *text));
			}
		}
	};
	// Ranges are Unreal's.
	readFloat("Speed", Camera.Speed, 0.0001f, 10000.0f);
	readFloat("MouseSensitivity", Camera.MouseSensitivity, 0.01f, 1.0f);
	readFloat("WheelDollyDistance", Camera.WheelDollyDistance, 0.0f, 1000.0f);
	readBool("InvertMouseLookY", Camera.bInvertMouseLookY);
	readBool("InvertOrbitY", Camera.bInvertOrbitY);
	readBool("InvertMiddleMousePan", Camera.bInvertMiddleMousePan);
	readBool("InvertRightMouseDollyY", Camera.bInvertRightMouseDollyY);

	Validate();
	++Generation;
	FString files = FString::FromPath(Defaults.Path);
	if (User.bExists)
	{
		files.Append(" + ").AppendPath(User.Path);
	}
	std::printf("[input] editor input read: %s, %zu problem(s)\n", files.CStr(), Problems.size());
}

void CEditorInput::Validate ()
{
	// Names nobody reads: most likely a typo, which would otherwise fail silently.
	for (const SLayer* layer : { &Defaults, &User })
	{
		const FString file = FString::FromPath(layer->Path.filename()) + (layer == &User ? " (user)" : "");
		for (const config::CIniFile::SSection& section : layer->File.GetSections())
		{
			const bool bCommands = section.Name.Equals<ESearchCase::IgnoreCaseAscii>(CommandsSection);
			const bool bKeySet = section.Name.Equals<ESearchCase::IgnoreCaseAscii>(KeySetSections[0]) || section.Name.Equals<ESearchCase::IgnoreCaseAscii>(KeySetSections[1]);
			const bool bCamera = section.Name.Equals<ESearchCase::IgnoreCaseAscii>(CameraSection);
			if (!bCommands && !bKeySet && !bCamera)
			{
				if (!section.Entries.empty() || !section.Name.IsEmpty())
				{
					Report(FString::Format("{}: unknown section [{}]", file, section.Name));
				}
				continue;
			}
			for (const config::CIniFile::SEntry& entry : section.Entries)
			{
				bool bKnown = false;
				if (bCommands)
				{
					EEditorCommand command;
					bKnown = enum_::TryParse(entry.Key, &command, true);
				}
				else if (bKeySet)
				{
					EViewportKey key;
					bKnown = enum_::TryParse(entry.Key, &key, true);
				}
				else
				{
					bKnown = std::any_of(std::begin(CameraKeys), std::end(CameraKeys),
						[&entry] (FStringLiteral Name) { return entry.Key.Equals<ESearchCase::IgnoreCaseAscii>(Name); });
				}
				if (!bKnown)
				{
					Report(FString::Format("{}: [{}] has no setting called {}", file, section.Name, entry.Key));
				}
			}
		}
	}

	// One chord, two commands: both would run.
	for (size_t a = 0u; a < CommandChords.size(); ++a)
	{
		for (size_t b = a + 1u; b < CommandChords.size(); ++b)
		{
			for (const input::SInputChord& chord : CommandChords[a])
			{
				if (std::find(CommandChords[b].begin(), CommandChords[b].end(), chord) != CommandChords[b].end())
				{
					Report(FString::Format("{} is bound to both {} and {}", input::FormatInputChord(chord),
						enum_::ToString(static_cast<EEditorCommand>(a)), enum_::ToString(static_cast<EEditorCommand>(b))));
				}
			}
		}
	}
}

bool CEditorInput::WasCommandPressed (EEditorCommand Command, const input::CInputSystem& Input) const
{
	for (const input::SInputChord& chord : GetChords(Command))
	{
		if (input::WasChordPressed(Input, chord))
		{
			return true;
		}
	}
	return false;
}

const std::vector<input::SInputChord>& CEditorInput::GetChords (EEditorCommand Command) const
{
	return CommandChords[static_cast<size_t>(Command)];
}

const FString& CEditorInput::GetShortcutText (EEditorCommand Command) const
{
	return ShortcutTexts[static_cast<size_t>(Command)];
}

bool CEditorInput::IsKeyDown (EViewportKeySet Set, EViewportKey Key, const input::CInputSystem& Input) const
{
	for (const input::KeyCode code : ViewportKeys[static_cast<size_t>(Set)][static_cast<size_t>(Key)])
	{
		if (Input.IsKeyDown(code))
		{
			return true;
		}
	}
	return false;
}
}
