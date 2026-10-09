#include "EditorInput.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <system_error>

#include "Input/InputSystem.h"


namespace frt::editor
{
namespace
{
constexpr std::string_view CommandsSection = "Commands";
constexpr std::string_view CameraSection = "ViewportCamera";
constexpr std::string_view KeySetSections[] = { "FlightKeys", "KeyboardKeys" };
constexpr std::string_view CameraKeys[] =
{
	"Speed", "MouseSensitivity", "WheelDollyDistance",
	"InvertMouseLookY", "InvertOrbitY", "InvertMiddleMousePan", "InvertRightMouseDollyY",
};

// How often the files' timestamps are checked.
constexpr std::chrono::milliseconds PollInterval(250);

std::string_view TrimView (std::string_view Text)
{
	const size_t first = Text.find_first_not_of(" \t");
	if (first == std::string_view::npos)
	{
		return {};
	}
	return Text.substr(first, Text.find_last_not_of(" \t") - first + 1);
}

bool ParseFloat (std::string_view Text, float& OutValue)
{
	Text = TrimView(Text);
	float value = 0.0f;
	const auto [end, ec] = std::from_chars(Text.data(), Text.data() + Text.size(), value);
	// from_chars also reads "nan" and "inf", which no setting can take.
	if (ec != std::errc() || end != Text.data() + Text.size() || !std::isfinite(value))
	{
		return false;
	}
	OutValue = value;
	return true;
}

bool ParseBool (std::string_view Text, bool& OutValue)
{
	Text = TrimView(Text);
	const auto matches = [Text] (FStringView Spelling) { return FStringView(Text).Equals<ESearchCase::IgnoreCaseAscii>(Spelling); };
	if (matches("true") || matches("1") || matches("yes") || matches("on"))
	{
		OutValue = true;
		return true;
	}
	if (matches("false") || matches("0") || matches("no") || matches("off"))
	{
		OutValue = false;
		return true;
	}
	return false;
}

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

const FString* CEditorInput::Find (std::string_view Section, std::string_view Key) const
{
	if (const FString* value = User.File.Find(Section, Key))
	{
		return value;
	}
	return Defaults.File.Find(Section, Key);
}

void CEditorInput::Report (std::string Problem)
{
	std::printf("[input] %s\n", Problem.c_str());
	Problems.push_back(std::move(Problem));
}

std::vector<input::SInputChord> CEditorInput::ReadChords (std::string_view Section, std::string_view Key, bool bKeysOnly)
{
	std::vector<input::SInputChord> chords;
	const FString* value = Find(Section, Key);
	if (!value)
	{
		// Every binding is listed in the shipped file, unbound ones with an empty value: a
		// missing one means the file is older than the code.
		if (bHasDefaults)
		{
			Report(std::format("[{}] {} is missing from {}", Section, Key, Defaults.Path.filename().string()));
		}
		return chords;
	}

	FString error;
	if (!input::ParseInputChords(*value, chords, &error))
	{
		Report(std::format("[{}] {}: cannot read {}", Section, Key, error));
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
			Report(std::format("[{}] {}: {} is a modifier and cannot be bound on its own", Section, Key, input::FormatInputChord(*it)));
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
				Report(std::format("[{}] {}: {} - navigation keys take no modifiers; using the key alone",
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
			Report(std::format("cannot open {}; trying again", Layer.Path.string()));
		}
		Layer.bExists = bLoaded;
		return bLoaded;
	};
	bHasDefaults = load(Defaults);
	// The user file is optional: most users never override anything.
	load(User);
	if (!bHasDefaults)
	{
		Report(std::format("no editor input defaults at {}: nothing is bound", Defaults.Path.string()));
	}

	for (size_t i = 0u; i < CommandChords.size(); ++i)
	{
		const std::string_view name = enum_::ToString(static_cast<EEditorCommand>(i));
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
	const auto readFloat = [this] (std::string_view Key, float& Value, float Min, float Max)
	{
		if (const FString* text = Find(CameraSection, Key))
		{
			if (!ParseFloat(*text, Value))
			{
				Report(std::format("[{}] {}: \"{}\" is not a number", CameraSection, Key, *text));
			}
		}
		Value = std::clamp(Value, Min, Max);
	};
	const auto readBool = [this] (std::string_view Key, bool& Value)
	{
		if (const FString* text = Find(CameraSection, Key))
		{
			if (!ParseBool(*text, Value))
			{
				Report(std::format("[{}] {}: \"{}\" is not true or false", CameraSection, Key, *text));
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
	std::printf("[input] editor input read: %s%s, %zu problem(s)\n", Defaults.Path.string().c_str(),
		User.bExists ? (" + " + User.Path.string()).c_str() : "", Problems.size());
}

void CEditorInput::Validate ()
{
	// Names nobody reads: most likely a typo, which would otherwise fail silently.
	for (const SLayer* layer : { &Defaults, &User })
	{
		const std::string file = layer->Path.filename().string() + (layer == &User ? " (user)" : "");
		for (const config::CIniFile::SSection& section : layer->File.GetSections())
		{
			const bool bCommands = section.Name.Equals<ESearchCase::IgnoreCaseAscii>(CommandsSection);
			const bool bKeySet = section.Name.Equals<ESearchCase::IgnoreCaseAscii>(KeySetSections[0]) || section.Name.Equals<ESearchCase::IgnoreCaseAscii>(KeySetSections[1]);
			const bool bCamera = section.Name.Equals<ESearchCase::IgnoreCaseAscii>(CameraSection);
			if (!bCommands && !bKeySet && !bCamera)
			{
				if (!section.Entries.empty() || !section.Name.empty())
				{
					Report(std::format("{}: unknown section [{}]", file, section.Name));
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
						[&entry] (std::string_view Name) { return entry.Key.Equals<ESearchCase::IgnoreCaseAscii>(Name); });
				}
				if (!bKnown)
				{
					Report(std::format("{}: [{}] has no setting called {}", file, section.Name, entry.Key));
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
					Report(std::format("{} is bound to both {} and {}", input::FormatInputChord(chord),
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

const std::string& CEditorInput::GetShortcutText (EEditorCommand Command) const
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
