#include "Input/InputActions.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include "Assets/TextAssetIO.h"
#include "Input/InputSystem.h"
#include "Math/MathUtility.h"


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
	frt::input::KeyCode,
	FRT_ENUM_ENTRY(frt::input::KeyCode, A),
	FRT_ENUM_ENTRY(frt::input::KeyCode, B),
	FRT_ENUM_ENTRY(frt::input::KeyCode, C),
	FRT_ENUM_ENTRY(frt::input::KeyCode, D),
	FRT_ENUM_ENTRY(frt::input::KeyCode, E),
	FRT_ENUM_ENTRY(frt::input::KeyCode, F),
	FRT_ENUM_ENTRY(frt::input::KeyCode, G),
	FRT_ENUM_ENTRY(frt::input::KeyCode, H),
	FRT_ENUM_ENTRY(frt::input::KeyCode, I),
	FRT_ENUM_ENTRY(frt::input::KeyCode, J),
	FRT_ENUM_ENTRY(frt::input::KeyCode, K),
	FRT_ENUM_ENTRY(frt::input::KeyCode, L),
	FRT_ENUM_ENTRY(frt::input::KeyCode, M),
	FRT_ENUM_ENTRY(frt::input::KeyCode, N),
	FRT_ENUM_ENTRY(frt::input::KeyCode, O),
	FRT_ENUM_ENTRY(frt::input::KeyCode, P),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Q),
	FRT_ENUM_ENTRY(frt::input::KeyCode, R),
	FRT_ENUM_ENTRY(frt::input::KeyCode, S),
	FRT_ENUM_ENTRY(frt::input::KeyCode, T),
	FRT_ENUM_ENTRY(frt::input::KeyCode, U),
	FRT_ENUM_ENTRY(frt::input::KeyCode, V),
	FRT_ENUM_ENTRY(frt::input::KeyCode, W),
	FRT_ENUM_ENTRY(frt::input::KeyCode, X),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Y),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Z),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit0),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit1),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit2),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit3),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit4),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit5),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit6),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit7),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit8),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Digit9),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Space),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Tab),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Enter),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Backspace),
	FRT_ENUM_ENTRY(frt::input::KeyCode, Escape),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftShift),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightShift),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftCtrl),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightCtrl),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftAlt),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightAlt),
	FRT_ENUM_ENTRY(frt::input::KeyCode, LeftMeta),
	FRT_ENUM_ENTRY(frt::input::KeyCode, RightMeta),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowUp),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowDown),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowLeft),
	FRT_ENUM_ENTRY(frt::input::KeyCode, ArrowRight)
);

FRT_DECLARE_ENUM_REFLECTION(
	frt::input::EMouseButton,
	FRT_ENUM_ENTRY(frt::input::EMouseButton, Left),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, Right),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, Middle),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, X1),
	FRT_ENUM_ENTRY(frt::input::EMouseButton, X2)
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

	std::string NormalizeEnumToken (std::string_view Value)
	{
		std::string normalized;
		normalized.reserve(Value.size());
		for (char c : Value)
		{
			if (c == '_' || c == '-' || c == ' ')
			{
				continue;
			}
			normalized.push_back(c);
		}
		return normalized;
	}

	bool TryParseKey (const std::string& Value, KeyCode* OutCode)
	{
		std::string normalized = NormalizeEnumToken(Value);
		if (normalized.size() == 1 && normalized[0] >= '0' && normalized[0] <= '9')
		{
			normalized = "Digit" + normalized;
		}
		return enum_::TryParse(normalized, OutCode, true);
	}

	bool TryParseMouseButton (const std::string& Value, EMouseButton* OutButton)
	{
		const std::string normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutButton, true);
	}

	bool TryParseGamepadButton (const std::string& Value, EGamepadButton* OutButton)
	{
		const std::string normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutButton, true);
	}

	bool TryParseGamepadAxis (const std::string& Value, EGamepadAxis* OutAxis)
	{
		const std::string normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutAxis, true);
	}

	std::string KeyToString (KeyCode Code)
	{
		const std::string_view name = enum_::ToString(Code);
		return name.empty() ? "unknown" : std::string(name);
	}

	std::string MouseButtonToString (EMouseButton Button)
	{
		const std::string_view name = enum_::ToString(Button);
		return name.empty() ? "left" : std::string(name);
	}

	std::string GamepadButtonToString (EGamepadButton Button)
	{
		const std::string_view name = enum_::ToString(Button);
		return name.empty() ? "a" : std::string(name);
	}

	std::string GamepadAxisToString (EGamepadAxis Axis)
	{
		const std::string_view name = enum_::ToString(Axis);
		return name.empty() ? "leftx" : std::string(name);
	}

	bool TryParseActionKind (const std::string& Value, EInputActionKind* OutKind)
	{
		const std::string normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutKind, true);
	}

	std::string_view ActionKindToString (EInputActionKind Kind)
	{
		const std::string_view name = enum_::ToString(Kind);
		return name.empty() ? std::string_view("button") : name;
	}

	bool TryParseBindingType (const std::string& Value, EInputBindingType* OutType)
	{
		const std::string normalized = NormalizeEnumToken(Value);
		return enum_::TryParse(normalized, OutType, true);
	}

	std::string_view BindingTypeToString (EInputBindingType Type)
	{
		const std::string_view name = enum_::ToString(Type);
		return name.empty() ? std::string_view("key") : name;
	}

	SFlags<EInputModifier> ParseModifiers (const std::string& Value)
	{
		SFlags<EInputModifier> result = EInputModifier::None;
		std::string token;
		std::istringstream stream(Value);
		while (std::getline(stream, token, '|'))
		{
			const std::string normalized = assets::text::ToLowerCopy(assets::text::TrimCopy(token));
			if (normalized == "shift")
			{
				result += EInputModifier::Shift;
			}
			else if (normalized == "ctrl" || normalized == "control")
			{
				result += EInputModifier::Ctrl;
			}
			else if (normalized == "alt")
			{
				result += EInputModifier::Alt;
			}
			else if (normalized == "super" || normalized == "win")
			{
				result += EInputModifier::Super;
			}
			else if (normalized == "capslock")
			{
				result += EInputModifier::CapsLock;
			}
			else if (normalized == "numlock")
			{
				result += EInputModifier::NumLock;
			}
		}

		return result;
	}

	bool TryParseBinding (const std::string& Value, SInputBinding* OutBinding)
	{
		std::istringstream stream(Value);
		std::string token;
		if (!(stream >> token))
		{
			return false;
		}

		EInputBindingType type = EInputBindingType::Key;
		if (!TryParseBindingType(token, &type))
		{
			return false;
		}

		SInputBinding binding = {};
		binding.Type = type;

		if (!(stream >> token))
		{
			return false;
		}

		bool parsedPrimary = false;
		switch (type)
		{
			case EInputBindingType::Key: parsedPrimary = TryParseKey(token, &binding.Key);
				break;
			case EInputBindingType::MouseButton: parsedPrimary = TryParseMouseButton(token, &binding.MouseButton);
				break;
			case EInputBindingType::GamepadButton: parsedPrimary = TryParseGamepadButton(token, &binding.GamepadButton);
				break;
			case EInputBindingType::GamepadAxis: parsedPrimary = TryParseGamepadAxis(token, &binding.GamepadAxis);
				break;
			default: break;
		}

		if (!parsedPrimary)
		{
			return false;
		}

		while (stream >> token)
		{
			const size_t split = token.find('=');
			if (split == std::string::npos)
			{
				continue;
			}

			std::string key = assets::text::ToLowerCopy(token.substr(0, split));
			std::string value = assets::text::TrimCopy(token.substr(split + 1));
			value = assets::text::StripQuotes(value);

			if (key == "scale")
			{
				(void)assets::text::ParseFloat(value, &binding.Scale);
			}
			else if (key == "deadzone")
			{
				(void)assets::text::ParseFloat(value, &binding.Deadzone);
			}
			else if (key == "mods" || key == "modifiers")
			{
				binding.Modifiers = ParseModifiers(value);
			}
			else if (key == "gamepad")
			{
				uint32 idx = 0;
				if (assets::text::ParseUInt32(value, &idx))
				{
					binding.GamepadIndex = static_cast<uint8>(idx);
				}
			}
		}

		*OutBinding = binding;
		return true;
	}

	std::string BindingToString (const SInputBinding& Binding)
	{
		std::ostringstream stream;
		stream << BindingTypeToString(Binding.Type) << " ";

		switch (Binding.Type)
		{
			case EInputBindingType::Key: stream << KeyToString(Binding.Key);
				break;
			case EInputBindingType::MouseButton: stream << MouseButtonToString(Binding.MouseButton);
				break;
			case EInputBindingType::GamepadButton: stream << GamepadButtonToString(Binding.GamepadButton);
				break;
			case EInputBindingType::GamepadAxis: stream << GamepadAxisToString(Binding.GamepadAxis);
				break;
			default: break;
		}

		if (Binding.Scale != 1.0f)
		{
			stream << " scale=" << Binding.Scale;
		}
		if (Binding.Deadzone > 0.0f)
		{
			stream << " deadzone=" << Binding.Deadzone;
		}
		if (Binding.GamepadIndex != 0u)
		{
			stream << " gamepad=" << static_cast<uint32>(Binding.GamepadIndex);
		}
		if (Binding.Modifiers.HasFlag(EInputModifier::Shift)
			|| Binding.Modifiers.HasFlag(EInputModifier::Ctrl)
			|| Binding.Modifiers.HasFlag(EInputModifier::Alt)
			|| Binding.Modifiers.HasFlag(EInputModifier::Super)
			|| Binding.Modifiers.HasFlag(EInputModifier::CapsLock)
			|| Binding.Modifiers.HasFlag(EInputModifier::NumLock))
		{
			stream << " mods=";
			bool first = true;
			auto append = [&stream, &first] (const char* name)
			{
				if (!first)
				{
					stream << "|";
				}
				first = false;
				stream << name;
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

		return stream.str();
	}
}


bool CInputActionMap::LoadFromFile (const std::filesystem::path& Path)
{
	Clear();

	std::ifstream stream(Path);
	if (!stream.is_open())
	{
		return false;
	}

	std::string line;
	SInputAction* currentAction = nullptr;

	while (std::getline(stream, line))
	{
		std::string key;
		std::string value;
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

	std::ifstream stream(Path);
	if (!stream.is_open())
	{
		return false;
	}

	SInputActionDefinition action;
	action.Name = Path.stem().string();

	std::string line;
	while (std::getline(stream, line))
	{
		std::string key;
		std::string value;
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
	const std::string name = Action.Name.empty() ? "Unnamed" : Action.Name;
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

const SInputAction* CInputActionMap::FindAction (const std::string& Name) const
{
	auto it = ActionIndex.find(Name);
	if (it == ActionIndex.end())
	{
		return nullptr;
	}

	return &Actions[it->second];
}

SInputAction* CInputActionMap::FindAction (const std::string& Name)
{
	return const_cast<SInputAction*>(static_cast<const CInputActionMap&>(*this).FindAction(Name));
}

const SInputActionState* CInputActionMap::FindActionState (const std::string& Name) const
{
	auto it = ActionIndex.find(Name);
	if (it == ActionIndex.end())
	{
		return nullptr;
	}

	return &States[it->second];
}

SInputActionState* CInputActionMap::FindActionState (const std::string& Name)
{
	return const_cast<SInputActionState*>(static_cast<const CInputActionMap&>(*this).FindActionState(Name));
}

void CInputActionMap::ApplyActionKind (const std::string& Name, EInputActionKind Kind)
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
