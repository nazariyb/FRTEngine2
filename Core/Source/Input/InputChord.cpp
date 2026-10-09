#include "Input/InputChord.h"

#include "Input/InputSystem.h"


namespace frt::input
{
namespace
{
struct SKeyAlias
{
	FStringLiteral Name;
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

bool ParseModifier (FStringView Text, EInputModifier& OutModifier)
{
	const auto matches = [Text] (FStringView Name) { return Text.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
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

bool ParseKeyName (FStringView Text, KeyCode& OutKey)
{
	Text.TrimStartAndEndInline();
	for (const SKeyAlias& alias : KeyAliases)
	{
		if (Text.Equals<ESearchCase::IgnoreCaseAscii>(alias.Name))
		{
			OutKey = alias.Key;
			return true;
		}
	}
	return enum_::TryParse(Text, &OutKey, true);
}

bool ParseInputChord (FStringView Text, SInputChord& OutChord)
{
	SInputChord chord;
	Text.TrimStartAndEndInline();
	if (Text.IsEmpty())
	{
		return false;
	}

	// The last token is the key, every one before it a modifier.
	const SStringSplit keySplit = Text.SplitLast('+');
	if (keySplit)
	{
		for (const FStringView token : keySplit.Left.Split('+', EStringSplit::Trim))
		{
			EInputModifier modifier = EInputModifier::None;
			if (!ParseModifier(token, modifier))
			{
				return false;
			}
			chord.Modifiers += modifier;
		}
	}
	if (!ParseKeyName(keySplit ? keySplit.Right : keySplit.Left, chord.Key))
	{
		return false;
	}

	OutChord = chord;
	return true;
}

bool ParseInputChords (FStringView Text, std::vector<SInputChord>& OutChords, FString* OutError)
{
	OutChords.clear();
	Text.TrimStartAndEndInline();
	if (Text.IsEmpty() || Text.Equals<ESearchCase::IgnoreCaseAscii>("None"))
	{
		return true;
	}

	bool bAllValid = true;
	for (const FStringView entry : Text.Split(',', EStringSplit::Trim))
	{
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
				if (!OutError->IsEmpty())
				{
					*OutError << ", ";
				}
				*OutError << '"' << entry << '"';
			}
		}
	}
	return bAllValid;
}

FStringLiteral GetKeyDisplayName (KeyCode Key)
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

FString FormatInputChord (const SInputChord& Chord)
{
	if (!Chord.IsValid())
	{
		return {};
	}
	FString text;
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
