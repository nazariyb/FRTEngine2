#include "Input/InputActions.h"

#include <cmath>
#include <fstream>
#include <optional>

#include "Assets/TextAssetIO.h"
#include "Containers/String.h"
#include "Input/InputSystem.h"
#include "Math/MathUtility.h"
#include "Platform/Platform.h"


FRT_DECLARE_ENUM_REFLECTION(
	frt::input::EInputActionKind,
	FRT_ENUM_ENTRY(frt::input::EInputActionKind, Button),
	FRT_ENUM_ENTRY(frt::input::EInputActionKind, Axis)
);

FRT_DECLARE_ENUM_REFLECTION(
	frt::input::EInputBindingType,
	FRT_ENUM_ENTRY(frt::input::EInputBindingType, Key),
	FRT_ENUM_ENTRY(frt::input::EInputBindingType, MouseButton),
	FRT_ENUM_ENTRY(frt::input::EInputBindingType, GamepadButton),
	FRT_ENUM_ENTRY(frt::input::EInputBindingType, GamepadAxis)
);

FRT_DECLARE_ENUM_REFLECTION(
	frt::input::EGamepadButton,
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, A),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, B),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, X),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, Y),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, LeftBumper),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, RightBumper),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, Back),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, Start),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, Guide),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, LeftStick),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, RightStick),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, DpadUp),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, DpadDown),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, DpadLeft),
	FRT_ENUM_ENTRY(frt::input::EGamepadButton, DpadRight)
);

FRT_DECLARE_ENUM_REFLECTION(
	frt::input::EGamepadAxis,
	FRT_ENUM_ENTRY(frt::input::EGamepadAxis, LeftX),
	FRT_ENUM_ENTRY(frt::input::EGamepadAxis, LeftY),
	FRT_ENUM_ENTRY(frt::input::EGamepadAxis, RightX),
	FRT_ENUM_ENTRY(frt::input::EGamepadAxis, RightY),
	FRT_ENUM_ENTRY(frt::input::EGamepadAxis, LeftTrigger),
	FRT_ENUM_ENTRY(frt::input::EGamepadAxis, RightTrigger)
);


namespace frt::input
{
namespace
{
	static constexpr int32 InputActionsFileVersion = 1;
	static constexpr int32 InputActionDefinitionVersion = 1;

	// Enum names as people write them: "Left_Shift", "left-shift" and "Left Shift" are LeftShift.
	FString NormalizeEnumToken (FStringView Value)
	{
		FString normalized(Value);
		normalized.RemoveAll('_');
		normalized.RemoveAll('-');
		normalized.RemoveAll(' ');
		return normalized;
	}

	bool TryParseKey (FStringView Value, KeyCode* OutCode)
	{
		FString normalized = NormalizeEnumToken(Value);
		if (normalized.Len() == 1u && ascii::IsDigit(normalized[0]))
		{
			normalized.InsertAt(0, "Digit");
		}
		return enum_::TryParse(normalized, OutCode, true);
	}

	bool TryParseMouseButton (FStringView Value, EMouseButton* OutButton)
	{
		const FString normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutButton, true);
	}

	bool TryParseGamepadButton (FStringView Value, EGamepadButton* OutButton)
	{
		const FString normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutButton, true);
	}

	bool TryParseGamepadAxis (FStringView Value, EGamepadAxis* OutAxis)
	{
		const FString normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutAxis, true);
	}

	// A reflected enum's name, or Fallback for a value outside the reflected set.
	template <enum_::ReflectedEnum E>
	FStringLiteral ToStringOr (E Value, FStringLiteral Fallback)
	{
		const FStringLiteral name = enum_::ToString(Value);
		return name.IsEmpty() ? Fallback : name;
	}

	FStringLiteral KeyToString (KeyCode Code)
	{
		return ToStringOr(Code, "unknown");
	}

	FStringLiteral MouseButtonToString (EMouseButton Button)
	{
		return ToStringOr(Button, "left");
	}

	FStringLiteral GamepadButtonToString (EGamepadButton Button)
	{
		return ToStringOr(Button, "a");
	}

	FStringLiteral GamepadAxisToString (EGamepadAxis Axis)
	{
		return ToStringOr(Axis, "leftx");
	}

	bool TryParseActionKind (FStringView Value, EInputActionKind* OutKind)
	{
		const FString normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutKind, true);
	}

	FStringLiteral ActionKindToString (EInputActionKind Kind)
	{
		return ToStringOr(Kind, "button");
	}

	bool TryParseBindingType (FStringView Value, EInputBindingType* OutType)
	{
		const FString normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutType, true);
	}

	FStringLiteral BindingTypeToString (EInputBindingType Type)
	{
		return ToStringOr(Type, "key");
	}

	// "Shift|Ctrl": names ignore A-Z case, as enum names do; unknown ones are skipped.
	SFlags<EInputModifier> ParseModifiers (FStringView Value)
	{
		SFlags<EInputModifier> result = EInputModifier::None;
		for (const FStringView token : Value.Split('|', EStringSplit::Trim))
		{
			const auto matches = [token] (FStringView Name) { return token.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
			if (matches("shift"))
			{
				result += EInputModifier::Shift;
			}
			else if (matches("ctrl") || matches("control"))
			{
				result += EInputModifier::Ctrl;
			}
			else if (matches("alt"))
			{
				result += EInputModifier::Alt;
			}
			else if (matches("super") || matches("win"))
			{
				result += EInputModifier::Super;
			}
			else if (matches("capslock"))
			{
				result += EInputModifier::CapsLock;
			}
			else if (matches("numlock"))
			{
				result += EInputModifier::NumLock;
			}
		}

		return result;
	}

	// "<type> <key, button or axis> [option=value ...]", separated by whitespace.
	bool TryParseBinding (FStringView Value, SInputBinding* OutBinding)
	{
		const FStringSplitRange tokens = Value.SplitWhitespace();
		auto token = tokens.begin();
		EInputBindingType type = EInputBindingType::Key;
		if (token == tokens.end() || !TryParseBindingType(*token, &type))
		{
			return false;
		}

		SInputBinding binding = {};
		binding.Type = type;

		if (++token == tokens.end())
		{
			return false;
		}

		bool parsedPrimary = false;
		switch (type)
		{
			case EInputBindingType::Key: parsedPrimary = TryParseKey(*token, &binding.Key);
				break;
			case EInputBindingType::MouseButton: parsedPrimary = TryParseMouseButton(*token, &binding.MouseButton);
				break;
			case EInputBindingType::GamepadButton: parsedPrimary = TryParseGamepadButton(*token, &binding.GamepadButton);
				break;
			case EInputBindingType::GamepadAxis: parsedPrimary = TryParseGamepadAxis(*token, &binding.GamepadAxis);
				break;
			default: break;
		}

		if (!parsedPrimary)
		{
			return false;
		}

		// Options: a token without '=', or with a value that does not parse, is skipped.
		while (++token != tokens.end())
		{
			const FStringView option = *token;
			const SStringSplit keyValue = option.SplitFirst('=');
			if (!keyValue)
			{
				continue;
			}

			const auto matches = [&keyValue] (FStringView Name) { return keyValue.Left.Equals<ESearchCase::IgnoreCaseAscii>(Name); };
			const FStringView value = keyValue.Right.TrimQuotes("\"'");

			if (matches("scale"))
			{
				binding.Scale = value.ToFloat().value_or(binding.Scale);
			}
			else if (matches("deadzone"))
			{
				binding.Deadzone = value.ToFloat().value_or(binding.Deadzone);
			}
			else if (matches("mods") || matches("modifiers"))
			{
				binding.Modifiers = ParseModifiers(value);
			}
			else if (matches("gamepad"))
			{
				binding.GamepadIndex = value.TryParse<uint8>().value_or(binding.GamepadIndex);
			}
		}

		*OutBinding = binding;
		return true;
	}

	// Floats are written shortest-round-trip, so they read back exactly.
	FString BindingToString (const SInputBinding& Binding)
	{
		FString text;
		text << BindingTypeToString(Binding.Type) << ' ';

		switch (Binding.Type)
		{
			case EInputBindingType::Key: text << KeyToString(Binding.Key);
				break;
			case EInputBindingType::MouseButton: text << MouseButtonToString(Binding.MouseButton);
				break;
			case EInputBindingType::GamepadButton: text << GamepadButtonToString(Binding.GamepadButton);
				break;
			case EInputBindingType::GamepadAxis: text << GamepadAxisToString(Binding.GamepadAxis);
				break;
			default: break;
		}

		if (Binding.Scale != 1.0f)
		{
			text << " scale=" << Binding.Scale;
		}
		if (Binding.Deadzone > 0.0f)
		{
			text << " deadzone=" << Binding.Deadzone;
		}
		if (Binding.GamepadIndex != 0u)
		{
			text << " gamepad=" << static_cast<uint32>(Binding.GamepadIndex);
		}
		if (Binding.Modifiers.HasFlag(EInputModifier::Shift)
			|| Binding.Modifiers.HasFlag(EInputModifier::Ctrl)
			|| Binding.Modifiers.HasFlag(EInputModifier::Alt)
			|| Binding.Modifiers.HasFlag(EInputModifier::Super)
			|| Binding.Modifiers.HasFlag(EInputModifier::CapsLock)
			|| Binding.Modifiers.HasFlag(EInputModifier::NumLock))
		{
			text << " mods=";
			bool first = true;
			auto append = [&text, &first] (const char* name)
			{
				if (!first)
				{
					text << '|';
				}
				first = false;
				text << name;
			};

			if (Binding.Modifiers.HasFlag(EInputModifier::Shift))
			{
				append("Shift");
			}
			if (Binding.Modifiers.HasFlag(EInputModifier::Ctrl))
			{
				append("Ctrl");
			}
			if (Binding.Modifiers.HasFlag(EInputModifier::Alt))
			{
				append("Alt");
			}
			if (Binding.Modifiers.HasFlag(EInputModifier::Super))
			{
				append("Super");
			}
			if (Binding.Modifiers.HasFlag(EInputModifier::CapsLock))
			{
				append("CapsLock");
			}
			if (Binding.Modifiers.HasFlag(EInputModifier::NumLock))
			{
				append("NumLock");
			}
		}

		return text;
	}
}


bool CInputActionMap::LoadFromFile (const std::filesystem::path& Path)
{
	Clear();

	const std::optional<FString> text = platform::ReadFileText(Path);
	if (!text)
	{
		return false;
	}

	SInputAction* currentAction = nullptr;

	for (const FStringView line : text->Lines())
	{
		FStringView key;
		FStringView value;
		if (!assets::text::TryParseKeyValue(line, &key, &value))
		{
			continue;
		}

		if (key == "version")
		{
			continue;
		}

		if (key == "action")
		{
			SInputAction& action = Actions.Add();
			action.Name = value;
			action.Kind = EInputActionKind::Button;
			currentAction = &action;
			continue;
		}

		if (!currentAction)
		{
			continue;
		}

		if (key == "type")
		{
			EInputActionKind kind = currentAction->Kind;
			if (TryParseActionKind(value, &kind))
			{
				currentAction->Kind = kind;
			}
			continue;
		}

		if (key == "bind")
		{
			SInputBinding binding;
			if (TryParseBinding(value, &binding))
			{
				currentAction->Bindings.Add(binding);
			}
		}
	}

	States.SetSize(Actions.Count());
	RebuildIndex();
	return true;
}

bool CInputActionMap::SaveToFile (const std::filesystem::path& Path) const
{
	std::error_code ec;
	std::filesystem::create_directories(Path.parent_path(), ec);

	std::ofstream stream(Path);
	if (!stream.is_open())
	{
		return false;
	}

	return SaveToStream(stream);
}

bool CInputActionMap::SaveToStream (std::ostream& Stream) const
{
	Stream << "version: " << InputActionsFileVersion << "\n";
	Stream << "# FRT input actions\n\n";

	for (const SInputAction& action : Actions)
	{
		Stream << "action: " << action.Name << "\n";
		Stream << "type: " << ActionKindToString(action.Kind) << "\n";
		for (const SInputBinding& binding : action.Bindings)
		{
			Stream << "bind: " << BindingToString(binding) << "\n";
		}
		Stream << "\n";
	}

	return Stream.good();
}

bool LoadInputActionDefinition (const std::filesystem::path& Path, SInputActionDefinition* OutAction)
{
	if (!OutAction)
	{
		return false;
	}

	const std::optional<FString> text = platform::ReadFileText(Path);
	if (!text)
	{
		return false;
	}

	SInputActionDefinition action;
	action.Name = FString::FromPath(Path.stem());

	for (const FStringView line : text->Lines())
	{
		FStringView key;
		FStringView value;
		if (!assets::text::TryParseKeyValue(line, &key, &value))
		{
			continue;
		}

		if (key == "version")
		{
			continue;
		}

		if (key == "action")
		{
			action.Name = value;
			continue;
		}

		if (key == "type")
		{
			EInputActionKind kind = action.Kind;
			if (TryParseActionKind(value, &kind))
			{
				action.Kind = kind;
			}
			continue;
		}
	}

	*OutAction = action;
	return true;
}

bool SaveInputActionDefinition (const std::filesystem::path& Path, const SInputActionDefinition& Action)
{
	std::error_code ec;
	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ec);
	}

	std::ofstream stream(Path);
	if (!stream.is_open())
	{
		return false;
	}

	return SaveInputActionDefinitionToStream(stream, Action);
}

bool SaveInputActionDefinitionToStream (std::ostream& Stream, const SInputActionDefinition& Action)
{
	const FStringView name = Action.Name.IsEmpty() ? "Unnamed" : Action.Name.View();
	Stream << "version: " << InputActionDefinitionVersion << "\n";
	Stream << "# FRT input action\n\n";
	Stream << "action: " << name << "\n";
	Stream << "type: " << ActionKindToString(Action.Kind) << "\n";
	return Stream.good();
}

void CInputActionMap::Clear ()
{
	Actions.Clear();
	States.Clear();
	ActionIndex.clear();
}

void CInputActionMap::Evaluate (const CInputSystem& Input, WindowId Window)
{
	if (States.Count() != Actions.Count())
	{
		States.SetSize(Actions.Count());
	}

	for (uint32 index = 0; index < Actions.Count(); ++index)
	{
		const SInputAction& action = Actions[index];
		SInputActionState& state = States[index];

		const bool wasDown = state.bDown;
		const float previousValue = state.Value;
		float value = 0.0f;
		bool down = false;

		if (action.Kind == EInputActionKind::Button)
		{
			for (const SInputBinding& binding : action.Bindings)
			{
				const float bindingValue = EvaluateBindingValue(binding, Input, Window);
				if (bindingValue != 0.0f)
				{
					down = true;
					break;
				}
			}

			value = down ? 1.0f : 0.0f;
		}
		else if (action.Kind == EInputActionKind::Axis)
		{
			for (const SInputBinding& binding : action.Bindings)
			{
				value += EvaluateBindingValue(binding, Input, Window);
			}

			value = math::Clamp(value, -1.0f, 1.0f);
			down = std::abs(value) > 0.0f;
		}

		state.bDown = down;
		state.bPressed = (!wasDown && down);
		state.bReleased = (wasDown && !down);
		state.Value = value;

		if (state.bPressed)
		{
			OnActionEvent.Invoke(SInputActionEventData{
				.Name = action.Name,
				.Kind = action.Kind,
				.Type = EInputActionEventType::Pressed,
				.Value = state.Value,
				.bDown = state.bDown
			});
		}

		if (state.bReleased)
		{
			OnActionEvent.Invoke(SInputActionEventData{
				.Name = action.Name,
				.Kind = action.Kind,
				.Type = EInputActionEventType::Released,
				.Value = state.Value,
				.bDown = state.bDown
			});
		}

		if (state.bDown && wasDown)
		{
			OnActionEvent.Invoke(SInputActionEventData{
				.Name = action.Name,
				.Kind = action.Kind,
				.Type = EInputActionEventType::Held,
				.Value = state.Value,
				.bDown = state.bDown
			});
		}

		if (action.Kind == EInputActionKind::Axis && previousValue != state.Value)
		{
			OnActionEvent.Invoke(SInputActionEventData{
				.Name = action.Name,
				.Kind = action.Kind,
				.Type = EInputActionEventType::ValueChanged,
				.Value = state.Value,
				.bDown = state.bDown
			});
		}
	}
}

const SInputAction* CInputActionMap::FindAction (FStringView Name) const
{
	auto it = ActionIndex.find(Name);
	if (it == ActionIndex.end())
	{
		return nullptr;
	}

	return &Actions[it->second];
}

SInputAction* CInputActionMap::FindAction (FStringView Name)
{
	return const_cast<SInputAction*>(static_cast<const CInputActionMap&>(*this).FindAction(Name));
}

const SInputActionState* CInputActionMap::FindActionState (FStringView Name) const
{
	auto it = ActionIndex.find(Name);
	if (it == ActionIndex.end())
	{
		return nullptr;
	}

	return &States[it->second];
}

SInputActionState* CInputActionMap::FindActionState (FStringView Name)
{
	return const_cast<SInputActionState*>(static_cast<const CInputActionMap&>(*this).FindActionState(Name));
}

void CInputActionMap::ApplyActionKind (FStringView Name, EInputActionKind Kind)
{
	SInputAction* action = FindAction(Name);
	if (!action)
	{
		return;
	}

	action->Kind = Kind;
}

void CInputActionMap::RebuildIndex ()
{
	ActionIndex.clear();
	ActionIndex.reserve(Actions.Count());
	for (uint32 index = 0; index < Actions.Count(); ++index)
	{
		ActionIndex.emplace(Actions[index].Name, index);
	}
}

bool CInputActionMap::AreModifiersActive (
	SFlags<EInputModifier> Required,
	const CInputSystem& Input,
	WindowId Window) const
{
	if (Required.HasFlag(EInputModifier::Shift))
	{
		if (!Input.IsKeyDown(KeyCode::LeftShift, Window) && !Input.IsKeyDown(KeyCode::RightShift, Window))
		{
			return false;
		}
	}
	if (Required.HasFlag(EInputModifier::Ctrl))
	{
		if (!Input.IsKeyDown(KeyCode::LeftCtrl, Window) && !Input.IsKeyDown(KeyCode::RightCtrl, Window))
		{
			return false;
		}
	}
	if (Required.HasFlag(EInputModifier::Alt))
	{
		if (!Input.IsKeyDown(KeyCode::LeftAlt, Window) && !Input.IsKeyDown(KeyCode::RightAlt, Window))
		{
			return false;
		}
	}
	if (Required.HasFlag(EInputModifier::Super))
	{
		if (!Input.IsKeyDown(KeyCode::LeftMeta, Window) && !Input.IsKeyDown(KeyCode::RightMeta, Window))
		{
			return false;
		}
	}
	if (Required.HasFlag(EInputModifier::CapsLock))
	{
		if (!Input.IsKeyDown(KeyCode::CapsLock, Window))
		{
			return false;
		}
	}
	if (Required.HasFlag(EInputModifier::NumLock))
	{
		if (!Input.IsKeyDown(KeyCode::NumLock, Window))
		{
			return false;
		}
	}

	return true;
}

float CInputActionMap::EvaluateBindingValue (
	const SInputBinding& Binding,
	const CInputSystem& Input,
	WindowId Window) const
{
	if (!AreModifiersActive(Binding.Modifiers, Input, Window))
	{
		return 0.0f;
	}

	switch (Binding.Type)
	{
		case EInputBindingType::Key: return Input.IsKeyDown(Binding.Key, Window) ? Binding.Scale : 0.0f;
		case EInputBindingType::MouseButton: return Input.IsMouseButtonDown(Binding.MouseButton, Window)
														? Binding.Scale
														: 0.0f;
		case EInputBindingType::GamepadButton: return Input.IsGamepadButtonDown(
														Binding.GamepadIndex, Binding.GamepadButton, Window)
														? Binding.Scale
														: 0.0f;
		case EInputBindingType::GamepadAxis:
		{
			if (!Input.IsGamepadConnected(Binding.GamepadIndex, Window))
			{
				return 0.0f;
			}

			float value = Input.GetGamepadAxis(Binding.GamepadIndex, Binding.GamepadAxis, Window);
			if (std::abs(value) < Binding.Deadzone)
			{
				value = 0.0f;
			}
			return value * Binding.Scale;
		}
		default: break;
	}

	return 0.0f;
}
}
