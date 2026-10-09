#pragma once

#include <vector>

#include "Core.h"
#include "Containers/String.h"
#include "Input/InputTypes.h"


namespace frt::input
{
class CInputSystem;

/**
 * A key and the modifiers held with it - "Ctrl+Shift+P", "F11", "Numpad8" (Unreal's
 * FInputChord). The plain-text form is what binding files hold:
 *   - modifiers, then the key, joined by '+': Ctrl (or Control), Alt, Shift, Super (Win, Cmd);
 *   - the key by its KeyCode name (InputTypes.h), or a short alias: 0-9, Up / Down / Left /
 *     Right, Esc, Del, Ins, PgUp, PgDn, Return, NumpadAdd, NumpadSubtract;
 *   - case and spaces around '+' do not matter.
 * Keys are positional (scan codes): "W" is the key left of E on every layout.
 */
struct SInputChord
{
	KeyCode Key = KeyCode::Unknown;
	// Shift, Ctrl, Alt and Super only.
	SFlags<EInputModifier> Modifiers = EInputModifier::None;

	bool IsValid () const { return Key != KeyCode::Unknown; }
	bool operator== (const SInputChord& Other) const { return Key == Other.Key && Modifiers.Flags == Other.Modifiers.Flags; }
};

// One chord. False (and OutChord untouched) for anything but exactly one key with modifiers.
FRT_CORE_API bool ParseInputChord (FStringView Text, SInputChord& OutChord);

// A comma-separated list: "W, Up, Numpad8". Empty text or "None" is the empty list. Entries that
// do not parse are skipped, appended to OutError (quoted, comma-separated), and make the result false.
FRT_CORE_API bool ParseInputChords (FStringView Text, std::vector<SInputChord>& OutChords, FString* OutError = nullptr);

// A key alone: its KeyCode name or one of the aliases above.
FRT_CORE_API bool ParseKeyName (FStringView Text, KeyCode& OutKey);

// Text ParseInputChord reads back: modifiers in Unreal's order (Ctrl, Alt, Shift, Super), the
// key by its short alias where it has one ("Ctrl+Shift+P", "Up", "1"). Empty for no chord.
FRT_CORE_API FString FormatInputChord (const SInputChord& Chord);
FRT_CORE_API FStringLiteral GetKeyDisplayName (KeyCode Key);

// The chord's key was pressed this frame with exactly its modifiers held - no fewer, no more
// (Ctrl+P does not fire on Ctrl+Shift+P). Auto-repeat does not count.
FRT_CORE_API bool WasChordPressed (const CInputSystem& Input, const SInputChord& Chord);
}
