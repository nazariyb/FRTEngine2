#include <filesystem>
#include <optional>

#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Containers/String.h"
#include "Input/InputActions.h"
#include "Input/InputChord.h"
#include "Input/InputSystem.h"
#include "Input/PlatformInput.h"
#include "Platform/Platform.h"

#include <Windows.h>

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

// Under a directory with a non-ASCII name (the files go through UTF-16 paths all the way), of this
// process's own: test runs of several configurations may share the temp directory (PlatformTest.cpp).
std::filesystem::path MakeTempPath (FStringView Name)
{
	return std::filesystem::temp_directory_path() / FString::Format("FrtInputActionsTest Тест {}", GetCurrentProcessId()).ToPath()
		/ Name.ToPath();
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
		const FStringView name = enum_::ToString(key);
		ASSERT_FALSE(name.IsEmpty()) << "KeyCode " << i << " has no name";
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

	FString error;
	EXPECT_FALSE(ParseInputChords("Ctrl+P, Bogus, F5", chords, &error));
	ASSERT_EQ(chords.size(), 2u);
	EXPECT_EQ(chords[1], Chord(KeyCode::F5));
	EXPECT_EQ(error, "\"Bogus\"");

	// Every bad entry is reported, quoted, its text as written: non-ASCII included.
	error.Clear();
	EXPECT_FALSE(ParseInputChords("Ctrl+Ї, W, Ctrl+, Ф", chords, &error));
	ASSERT_EQ(chords.size(), 1u);
	EXPECT_EQ(chords[0], Chord(KeyCode::W));
	EXPECT_EQ(error, "\"Ctrl+Ї\", \"Ctrl+\", \"Ф\"");
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

TEST(InputActionMap, ReadsBindingsAndWritesThemBack)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = MakeTempPath("IAM_Test.frtinputmap");
	ASSERT_TRUE(platform::WriteFileText(path,
		"\xEF\xBB\xBF" "version: 1\n"
		"# comment\n"
		"action: IA_Move\n"
		"type: axis\n"
		"bind: Key W scale=1\n"
		"bind: key s scale=-0.333333 mods=shift|Ctrl\n"
		"bind: GamepadAxis left_y deadzone=0.25 gamepad=1 scale=2x\n"
		"\n"
		"action: \"IA_Стрибок\"\n"
		"bind: MouseButton Right\n"));

	CInputActionMap map;
	ASSERT_TRUE(map.LoadFromFile(path));
	ASSERT_EQ(map.GetActions().Count(), 2u);

	const SInputAction* move = map.FindAction("IA_Move");
	ASSERT_NE(move, nullptr);
	EXPECT_EQ(move->Kind, EInputActionKind::Axis);
	ASSERT_EQ(move->Bindings.Count(), 3u);
	EXPECT_EQ(move->Bindings[1].Key, KeyCode::S);
	EXPECT_EQ(move->Bindings[1].Scale, -0.333333f);
	EXPECT_EQ(move->Bindings[1].Modifiers.Flags, Mods({ EInputModifier::Shift, EInputModifier::Ctrl }).Flags);
	EXPECT_EQ(move->Bindings[2].Type, EInputBindingType::GamepadAxis);
	EXPECT_EQ(move->Bindings[2].GamepadAxis, EGamepadAxis::LeftY);
	EXPECT_EQ(move->Bindings[2].Deadzone, 0.25f);
	EXPECT_EQ(move->Bindings[2].GamepadIndex, 1u);
	EXPECT_EQ(move->Bindings[2].Scale, 1.0f) << "a value that does not parse keeps the default";

	// Names are case-sensitive and kept as written (quotes removed).
	EXPECT_NE(map.FindAction("IA_Стрибок"), nullptr);
	EXPECT_EQ(map.FindAction("ia_move"), nullptr);
	EXPECT_NE(map.FindActionState("IA_Move"), nullptr);

	// Written back and read again: the same actions and bindings.
	const std::filesystem::path copyPath = MakeTempPath("IAM_Copy.frtinputmap");
	ASSERT_TRUE(map.SaveToFile(copyPath));
	const std::optional<FString> written = platform::ReadFileText(copyPath);
	ASSERT_TRUE(written.has_value());
	EXPECT_TRUE(written->Contains("bind: Key S scale=-0.333333 mods=Shift|Ctrl")) << *written;
	EXPECT_TRUE(written->Contains("bind: GamepadAxis LeftY deadzone=0.25 gamepad=1")) << *written;
	EXPECT_TRUE(written->Contains("action: IA_Стрибок")) << *written;

	CInputActionMap copy;
	ASSERT_TRUE(copy.LoadFromFile(copyPath));
	ASSERT_EQ(copy.GetActions().Count(), 2u);
	const SInputAction* moveCopy = copy.FindAction("IA_Move");
	ASSERT_NE(moveCopy, nullptr);
	ASSERT_EQ(moveCopy->Bindings.Count(), 3u);
	EXPECT_EQ(moveCopy->Bindings[1].Scale, -0.333333f);
	EXPECT_EQ(moveCopy->Bindings[1].Modifiers.Flags, move->Bindings[1].Modifiers.Flags);
	EXPECT_EQ(moveCopy->Bindings[2].Deadzone, 0.25f);

	std::error_code ec;
	std::filesystem::remove_all(path.parent_path(), ec);
}
