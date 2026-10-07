#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Input/InputChord.h"
#include "Input/InputSystem.h"
#include "Input/PlatformInput.h"

using namespace frt;
using namespace frt::input;

namespace
{
constexpr WindowId TestWindow = 42u;

SInputChord Chord (KeyCode Key, SFlags<EInputModifier> Modifiers = EInputModifier::None)
{
	SInputChord chord;
	chord.Key = Key;
	chord.Modifiers = Modifiers;
	return chord;
}

SFlags<EInputModifier> Mods (std::initializer_list<EInputModifier> List)
{
	SFlags<EInputModifier> flags = EInputModifier::None;
	for (EInputModifier modifier : List)
	{
		flags += modifier;
	}
	return flags;
}

void Key (KeyCode Code, EInputAction Action)
{
	SKeyboardEventData data;
	data.Header.Window = TestWindow;
	data.Key = Code;
	data.Action = Action;
	CPlatformInputQueue::Enqueue(data);
}

void Wheel (float Delta, bool bHorizontal)
{
	SMouseWheelEventData data;
	data.Header.Window = TestWindow;
	data.Delta = Delta;
	data.bHorizontal = bHorizontal;
	CPlatformInputQueue::Enqueue(data);
}
}

TEST(InputChord, ParsesModifiersAndKeysCaseInsensitively)
{
	SInputChord chord;
	ASSERT_TRUE(ParseInputChord("Ctrl+Shift+P", chord));
	EXPECT_EQ(chord, Chord(KeyCode::P, Mods({ EInputModifier::Ctrl, EInputModifier::Shift })));

	ASSERT_TRUE(ParseInputChord("  control + alt +  p ", chord));
	EXPECT_EQ(chord, Chord(KeyCode::P, Mods({ EInputModifier::Ctrl, EInputModifier::Alt })));

	ASSERT_TRUE(ParseInputChord("F11", chord));
	EXPECT_EQ(chord, Chord(KeyCode::F11));

	ASSERT_TRUE(ParseInputChord("shift+space", chord));
	EXPECT_EQ(chord, Chord(KeyCode::Space, EInputModifier::Shift));

	ASSERT_TRUE(ParseInputChord("Win+E", chord));
	EXPECT_EQ(chord, Chord(KeyCode::E, EInputModifier::Super));
}

TEST(InputChord, EveryKeyHasAParseableName)
{
	for (uint16 i = 1u; i < static_cast<uint16>(KeyCode::Count); ++i)
	{
		const KeyCode key = static_cast<KeyCode>(i);
		const std::string_view name = enum_::ToString(key);
		ASSERT_FALSE(name.empty()) << "KeyCode " << i << " has no name";
		KeyCode parsed = KeyCode::Unknown;
		EXPECT_TRUE(ParseKeyName(name, parsed)) << name;
		EXPECT_EQ(parsed, key) << name;
	}
}

TEST(InputChord, AcceptsShortAliases)
{
	KeyCode key = KeyCode::Unknown;
	EXPECT_TRUE(ParseKeyName("1", key));
	EXPECT_EQ(key, KeyCode::Digit1);
	EXPECT_TRUE(ParseKeyName("Up", key));
	EXPECT_EQ(key, KeyCode::ArrowUp);
	EXPECT_TRUE(ParseKeyName("esc", key));
	EXPECT_EQ(key, KeyCode::Escape);
	EXPECT_TRUE(ParseKeyName("PgDn", key));
	EXPECT_EQ(key, KeyCode::PageDown);
	EXPECT_TRUE(ParseKeyName("NumPad8", key));
	EXPECT_EQ(key, KeyCode::Numpad8);
	EXPECT_TRUE(ParseKeyName("NumpadAdd", key));
	EXPECT_EQ(key, KeyCode::NumpadPlus);
}

TEST(InputChord, RejectsMalformedText)
{
	SInputChord chord = Chord(KeyCode::A);
	EXPECT_FALSE(ParseInputChord("", chord));
	EXPECT_FALSE(ParseInputChord("Ctrl+", chord));
	EXPECT_FALSE(ParseInputChord("Ctrl", chord));          // a modifier is not a key
	EXPECT_FALSE(ParseInputChord("Hyper+P", chord));       // unknown modifier
	EXPECT_FALSE(ParseInputChord("P+Ctrl", chord));        // modifiers come first
	EXPECT_FALSE(ParseInputChord("NotAKey", chord));
	EXPECT_EQ(chord, Chord(KeyCode::A)) << "a failed parse leaves the output untouched";
}

TEST(InputChord, ParsesListsAndReportsBadEntries)
{
	std::vector<SInputChord> chords;
	EXPECT_TRUE(ParseInputChords("W, Up, Numpad8", chords));
	ASSERT_EQ(chords.size(), 3u);
	EXPECT_EQ(chords[1], Chord(KeyCode::ArrowUp));

	EXPECT_TRUE(ParseInputChords("", chords));
	EXPECT_TRUE(chords.empty());
	EXPECT_TRUE(ParseInputChords("None", chords));
	EXPECT_TRUE(chords.empty());

	std::string error;
	EXPECT_FALSE(ParseInputChords("Ctrl+P, Bogus, F5", chords, &error));
	ASSERT_EQ(chords.size(), 2u);
	EXPECT_EQ(chords[1], Chord(KeyCode::F5));
	EXPECT_NE(error.find("Bogus"), std::string::npos);
}

TEST(InputChord, FormatsInUnrealOrderAndRoundTrips)
{
	EXPECT_EQ(FormatInputChord(Chord(KeyCode::P, Mods({ EInputModifier::Shift, EInputModifier::Alt, EInputModifier::Ctrl }))), "Ctrl+Alt+Shift+P");
	EXPECT_EQ(FormatInputChord(Chord(KeyCode::ArrowUp)), "Up");
	EXPECT_EQ(FormatInputChord(Chord(KeyCode::Digit7, EInputModifier::Ctrl)), "Ctrl+7");
	EXPECT_EQ(FormatInputChord(SInputChord()), "");

	for (uint16 i = 1u; i < static_cast<uint16>(KeyCode::Count); ++i)
	{
		const SInputChord chord = Chord(static_cast<KeyCode>(i), Mods({ EInputModifier::Ctrl, EInputModifier::Shift }));
		SInputChord parsed;
		ASSERT_TRUE(ParseInputChord(FormatInputChord(chord), parsed)) << FormatInputChord(chord);
		EXPECT_EQ(parsed, chord);
	}
}

TEST(InputChord, MatchesExactModifiersOnly)
{
	FRT_TEST_MEMORY_POOL();
	CInputSystem input;
	input.SetDefaultWindow(TestWindow);

	Key(KeyCode::LeftCtrl, EInputAction::Pressed);
	Key(KeyCode::P, EInputAction::Pressed);
	input.Update(0.016f);
	EXPECT_TRUE(WasChordPressed(input, Chord(KeyCode::P, EInputModifier::Ctrl)));
	EXPECT_FALSE(WasChordPressed(input, Chord(KeyCode::P)));
	EXPECT_FALSE(WasChordPressed(input, Chord(KeyCode::P, Mods({ EInputModifier::Ctrl, EInputModifier::Shift }))));

	// Held: no new press, no new chord.
	Key(KeyCode::P, EInputAction::Repeat);
	input.Update(0.016f);
	EXPECT_FALSE(WasChordPressed(input, Chord(KeyCode::P, EInputModifier::Ctrl)));

	Key(KeyCode::P, EInputAction::Released);
	Key(KeyCode::RightShift, EInputAction::Pressed);
	Key(KeyCode::P, EInputAction::Pressed);
	input.Update(0.016f);
	EXPECT_TRUE(WasChordPressed(input, Chord(KeyCode::P, Mods({ EInputModifier::Ctrl, EInputModifier::Shift }))));
	EXPECT_FALSE(WasChordPressed(input, Chord(KeyCode::P, EInputModifier::Ctrl)));
}

TEST(InputSystem, ATapWithinOneFrameIsStillAPress)
{
	FRT_TEST_MEMORY_POOL();
	CInputSystem input;
	input.SetDefaultWindow(TestWindow);

	Key(KeyCode::F2, EInputAction::Pressed);
	Key(KeyCode::F2, EInputAction::Released);
	input.Update(0.016f);
	EXPECT_TRUE(input.WasKeyPressed(KeyCode::F2));
	EXPECT_TRUE(input.WasKeyReleased(KeyCode::F2));
	EXPECT_FALSE(input.IsKeyDown(KeyCode::F2));

	input.Update(0.016f);
	EXPECT_FALSE(input.WasKeyPressed(KeyCode::F2));
	EXPECT_FALSE(input.WasKeyReleased(KeyCode::F2));
}

TEST(InputSystem, ReportsModifiersFromEitherSide)
{
	FRT_TEST_MEMORY_POOL();
	CInputSystem input;
	input.SetDefaultWindow(TestWindow);

	Key(KeyCode::RightCtrl, EInputAction::Pressed);
	Key(KeyCode::LeftAlt, EInputAction::Pressed);
	input.Update(0.016f);
	EXPECT_EQ(input.GetModifiers().Flags, Mods({ EInputModifier::Ctrl, EInputModifier::Alt }).Flags);

	Key(KeyCode::RightCtrl, EInputAction::Released);
	input.Update(0.016f);
	EXPECT_EQ(input.GetModifiers().Flags, Mods({ EInputModifier::Alt }).Flags);
}

TEST(InputSystem, KeepsTheHorizontalWheelApart)
{
	FRT_TEST_MEMORY_POOL();
	CInputSystem input;
	input.SetDefaultWindow(TestWindow);

	Wheel(1.0f, false);
	Wheel(-2.0f, true);
	input.Update(0.016f);
	EXPECT_FLOAT_EQ(input.GetMouseWheelDelta(), 1.0f);
	EXPECT_FLOAT_EQ(input.GetMouseWheelDeltaX(), -2.0f);

	input.Update(0.016f);
	EXPECT_FLOAT_EQ(input.GetMouseWheelDelta(), 0.0f);
	EXPECT_FLOAT_EQ(input.GetMouseWheelDeltaX(), 0.0f);
}

TEST(InputSystem, ClearForgetsHeldKeysAndQueuedInput)
{
	FRT_TEST_MEMORY_POOL();
	CInputSystem input;
	input.SetDefaultWindow(TestWindow);

	Key(KeyCode::LeftAlt, EInputAction::Pressed);
	input.Update(0.016f);
	ASSERT_TRUE(input.IsKeyDown(KeyCode::LeftAlt));

	// Focus lost with an event still queued: the Alt+Tab's Tab, say.
	Key(KeyCode::Tab, EInputAction::Pressed);
	input.Clear();
	input.Update(0.016f);
	EXPECT_FALSE(input.IsKeyDown(KeyCode::LeftAlt));
	EXPECT_FALSE(input.IsKeyDown(KeyCode::Tab));
	EXPECT_EQ(input.GetDefaultWindow(), TestWindow);

	// Input after the focus change works as before.
	Key(KeyCode::A, EInputAction::Pressed);
	input.Update(0.016f);
	EXPECT_TRUE(input.WasKeyPressed(KeyCode::A));
}
