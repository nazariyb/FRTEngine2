#include "Input/InputChord.h"

#include "Containers/StringView.h"
#include "Input/InputSystem.h"


namespace frt::input
{
namespace
{
constexpr std::string_view Whitespace = " \t\r\n";

std::string_view Trim (std::string_view Text)
{
	const size_t first = Text.find_first_not_of(Whitespace);
	if (first == std::string_view::npos)
	{
		return {};
	}
	const size_t last = Text.find_last_not_of(Whitespace);
	return Text.substr(first, last - first + 1);
}

struct SKeyAlias
{
	std::string_view Name;
	KeyCode Key;
};

// Short spellings people write. The first alias of a key is also its display name.
constexpr SKeyAlias KeyAliases[] =
{
	{ "0", KeyCode::Digit0 }, { "1", KeyCode::Digit1 }, { "2", KeyCode::Digit2 }, { "3", KeyCode::Digit3 },
	{ "4", KeyCode::Digit4 }, { "5", KeyCode::Digit5 }, { "6", KeyCode::Digit6 }, { "7", KeyCode::Digit7 },
	{ "8", KeyCode::Digit8 }, { "9", KeyCode::Digit9 },
	{ "Up", KeyCode::ArrowUp }, { "Down", KeyCode::ArrowDown }, { "Left", KeyCode::ArrowLeft }, { "Right", KeyCode::ArrowRight },
	{ "Esc", KeyCode::Escape },
	{ "Del", KeyCode::Delete },
	{ "Ins", KeyCode::Insert },
	{ "PgUp", KeyCode::PageUp },
	{ "PgDn", KeyCode::PageDown },
	{ "Return", KeyCode::Enter },
	{ "NumpadAdd", KeyCode::NumpadPlus },
	{ "NumpadSubtract", KeyCode::NumpadMinus },
};

// Aliases that read better than the KeyCode name when shown in a menu.
constexpr KeyCode DisplayAliased[] =
{
	KeyCode::Digit0, KeyCode::Digit1, KeyCode::Digit2, KeyCode::Digit3, KeyCode::Digit4,
	KeyCode::Digit5, KeyCode::Digit6, KeyCode::Digit7, KeyCode::Digit8, KeyCode::Digit9,
	KeyCode::ArrowUp, KeyCode::ArrowDown, KeyCode::ArrowLeft, KeyCode::ArrowRight,
};

bool ParseModifier (std::string_view Text, EInputModifier& OutModifier)
{
	const auto matches = [Text] (FStringView Name) { return FStringView(Text).Equals<ESearchCase::IgnoreCaseAscii>(Name); };
	if (matches("Ctrl") || matches("Control"))
	{
		OutModifier = EInputModifier::Ctrl;
	}
	else if (matches("Alt"))
	{
		OutModifier = EInputModifier::Alt;
	}
	else if (matches("Shift"))
	{
		OutModifier = EInputModifier::Shift;
	}
	else if (matches("Super") || matches("Win") || matches("Cmd"))
	{
		OutModifier = EInputModifier::Super;
	}
	else
	{
		return false;
	}
	return true;
}

constexpr uint8 ChordModifierMask =
	static_cast<uint8>(EInputModifier::Shift) | static_cast<uint8>(EInputModifier::Ctrl) |
	static_cast<uint8>(EInputModifier::Alt) | static_cast<uint8>(EInputModifier::Super);
}

bool ParseKeyName (std::string_view Text, KeyCode& OutKey)
{
	Text = Trim(Text);
	for (const SKeyAlias& alias : KeyAliases)
	{
		if (FStringView(Text).Equals<ESearchCase::IgnoreCaseAscii>(alias.Name))
		{
			OutKey = alias.Key;
			return true;
		}
	}
	return enum_::TryParse(Text, &OutKey, true);
}

bool ParseInputChord (std::string_view Text, SInputChord& OutChord)
{
	SInputChord chord;
	Text = Trim(Text);
	if (Text.empty())
	{
		return false;
	}

	size_t start = 0u;
	while (true)
	{
		const size_t plus = Text.find('+', start);
		const std::string_view token = Trim(Text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
		if (plus == std::string_view::npos)
		{
			// The last token is the key.
			if (!ParseKeyName(token, chord.Key))
			{
				return false;
			}
			break;
		}
		EInputModifier modifier = EInputModifier::None;
		if (!ParseModifier(token, modifier))
		{
			return false;
		}
		chord.Modifiers += modifier;
		start = plus + 1u;
	}

	OutChord = chord;
	return true;
}

bool ParseInputChords (std::string_view Text, std::vector<SInputChord>& OutChords, std::string* OutError)
{
	OutChords.clear();
	Text = Trim(Text);
	if (Text.empty() || FStringView(Text).Equals<ESearchCase::IgnoreCaseAscii>("None"))
	{
		return true;
	}

	bool bAllValid = true;
	size_t start = 0u;
	while (start <= Text.size())
	{
		const size_t comma = Text.find(',', start);
		const std::string_view entry = Trim(Text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
		SInputChord chord;
		if (ParseInputChord(entry, chord))
		{
			OutChords.push_back(chord);
		}
		else
		{
			bAllValid = false;
			if (OutError)
			{
				if (!OutError->empty())
				{
					*OutError += ", ";
				}
				*OutError += '"';
				*OutError += entry;
				*OutError += '"';
			}
		}
		if (comma == std::string_view::npos)
		{
			break;
		}
		start = comma + 1u;
	}
	return bAllValid;
}

std::string_view GetKeyDisplayName (KeyCode Key)
{
	for (const KeyCode aliased : DisplayAliased)
	{
		if (aliased == Key)
		{
			for (const SKeyAlias& alias : KeyAliases)
			{
				if (alias.Key == Key)
				{
					return alias.Name;
				}
			}
		}
	}
	return enum_::ToString(Key);
}

std::string FormatInputChord (const SInputChord& Chord)
{
	if (!Chord.IsValid())
	{
		return {};
	}
	std::string text;
	if (Chord.Modifiers.HasFlag(EInputModifier::Ctrl))
	{
		text += "Ctrl+";
	}
	if (Chord.Modifiers.HasFlag(EInputModifier::Alt))
	{
		text += "Alt+";
	}
	if (Chord.Modifiers.HasFlag(EInputModifier::Shift))
	{
		text += "Shift+";
	}
	if (Chord.Modifiers.HasFlag(EInputModifier::Super))
	{
		text += "Super+";
	}
	text += GetKeyDisplayName(Chord.Key);
	return text;
}

bool WasChordPressed (const CInputSystem& Input, const SInputChord& Chord)
{
	if (!Chord.IsValid() || !Input.WasKeyPressed(Chord.Key))
	{
		return false;
	}
	const uint8 held = Input.GetModifiers().Flags & ChordModifierMask;
	const uint8 wanted = Chord.Modifiers.Flags & ChordModifierMask;
	return held == wanted;
}
}
