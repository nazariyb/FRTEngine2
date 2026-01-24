#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Core.h"
#include "Containers/Array.h"
#include "Input/InputTypes.h"


namespace frt::input
{
class CInputSystem;

enum class EInputActionKind : uint8
{
	Button = 0u,
	Axis
};

enum class EInputBindingType : uint8
{
	Key = 0u,
	MouseButton,
	GamepadButton,
	GamepadAxis
};

enum class EInputActionEventType : uint8
{
	Pressed = 0u,
	Released,
	Held,
	ValueChanged
};


struct SInputBinding
{
	EInputBindingType Type = EInputBindingType::Key;
	KeyCode Key = KeyCode::Unknown;
	EMouseButton MouseButton = EMouseButton::Left;
	EGamepadButton GamepadButton = EGamepadButton::A;
	EGamepadAxis GamepadAxis = EGamepadAxis::LeftX;
	uint8 GamepadIndex = 0u;
	SFlags<EInputModifier> Modifiers = EInputModifier::None;
	float Scale = 1.0f;
	float Deadzone = 0.0f;
};


struct SInputAction
{
	std::string Name;
	EInputActionKind Kind = EInputActionKind::Button;
	TArray<SInputBinding> Bindings;
};

struct SInputActionEventData
{
	std::string_view Name;
	EInputActionKind Kind = EInputActionKind::Button;
	EInputActionEventType Type = EInputActionEventType::Pressed;
	float Value = 0.0f;
	bool bDown = false;
};

using InputEventAction = CEvent<SInputActionEventData>;

struct SInputActionDefinition
{
	std::string Name;
	EInputActionKind Kind = EInputActionKind::Button;
};

FRT_CORE_API bool LoadInputActionDefinition (
	const std::filesystem::path& Path,
	SInputActionDefinition* OutAction);
FRT_CORE_API bool SaveInputActionDefinition (
	const std::filesystem::path& Path,
	const SInputActionDefinition& Action);
FRT_CORE_API bool SaveInputActionDefinitionToStream (
	std::ostream& Stream,
	const SInputActionDefinition& Action);


struct SInputActionState
{
	bool bDown = false;
	bool bPressed = false;
	bool bReleased = false;
	float Value = 0.0f;
};


class FRT_CORE_API CInputActionMap
{
public:
	bool LoadFromFile (const std::filesystem::path& Path);
	bool SaveToFile (const std::filesystem::path& Path) const;
	bool SaveToStream (std::ostream& Stream) const;

	void Clear ();
	void Evaluate (const CInputSystem& Input, WindowId Window = InvalidWindowId);

	const TArray<SInputAction>& GetActions () const { return Actions; }
	const TArray<SInputActionState>& GetActionStates () const { return States; }

	const SInputAction* FindAction (const std::string& Name) const;
	SInputAction* FindAction (const std::string& Name);

	const SInputActionState* FindActionState (const std::string& Name) const;
	SInputActionState* FindActionState (const std::string& Name);

	void ApplyActionKind (const std::string& Name, EInputActionKind Kind);

	InputEventAction OnActionEvent;

private:
	void RebuildIndex ();
	bool AreModifiersActive (SFlags<EInputModifier> Required, const CInputSystem& Input, WindowId Window) const;
	float EvaluateBindingValue (const SInputBinding& Binding, const CInputSystem& Input, WindowId Window) const;

private:
	TArray<SInputAction> Actions;
	TArray<SInputActionState> States;
	std::unordered_map<std::string, uint32> ActionIndex;
};
}
