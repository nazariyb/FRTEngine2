#include "UI/ImGuiString.h"

#if !defined(FRT_HEADLESS)

#include <limits>


namespace frt::ui
{
namespace
{
// What the resize callback needs, and the caller's own callback it stands in front of.
struct SResizeUserData
{
	FString* Value;
	ImGuiInputTextCallback Callback;
	void* UserData;
};

int ResizeCallback (ImGuiInputTextCallbackData* Data)
{
	SResizeUserData& resize = *static_cast<SResizeUserData*>(Data->UserData);
	if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		// ImGui is about to copy BufTextLen bytes and a NUL into Buf. ResizeUninitialized makes room
		// (the terminator slot included) without filling bytes ImGui overwrites next anyway.
		FString& value = *resize.Value;
		frt_assert(Data->Buf == value.GetData());
		value.ResizeUninitialized(static_cast<uint32>(Data->BufTextLen));
		Data->Buf = value.GetData();
		Data->BufSize = static_cast<int>(value.GetCapacity()) + 1;
		return 0;
	}

	if (resize.Callback == nullptr)
	{
		return 0;
	}
	Data->UserData = resize.UserData;
	return resize.Callback(Data);
}

// One implementation for the three InputText variants. Input calls the ImGui function with
// (buffer, buffer size, flags, callback, user data).
template <typename TInput>
bool EditString (FString& Value, ImGuiInputTextFlags Flags, ImGuiInputTextCallback Callback, void* UserData, uint32 MaxBytes,
	TInput&& Input)
{
	frt_assert((Flags & ImGuiInputTextFlags_CallbackResize) == 0);   // the wrapper resizes, not the caller

	if (MaxBytes > 0u)
	{
		frt_assert(MaxBytes < static_cast<uint32>((std::numeric_limits<int>::max)()));
		// A fixed buffer: ImGui edits within MaxBytes + 1 bytes and stops at a whole code point. The
		// text is cut to the limit first (label-safe, never inside a character), then padded with NULs
		// to the buffer ImGui gets; the NUL it leaves after the text says where the text ends.
		Value.TruncateInline(MaxBytes);
		Value.Resize(MaxBytes, '\0');
		const bool bEdited = Input(Value.GetData(), static_cast<size_t>(MaxBytes) + 1u, Flags, Callback, UserData);
		Value.TrimToNullTerminator();
		return bEdited;
	}

	SResizeUserData resize = { &Value, Callback, UserData };
	return Input(Value.GetData(), static_cast<size_t>(Value.GetCapacity()) + 1u, Flags | ImGuiInputTextFlags_CallbackResize,
		&ResizeCallback, &resize);
}

const char* GetStringItem (void* Items, int Index)
{
	return static_cast<const FString*>(Items)[Index].CStr();
}

const char* GetLiteralItem (void* Items, int Index)
{
	return static_cast<const FStringLiteral*>(Items)[Index].CStr();
}

int GetItemCount (size_t Count)
{
	frt_assert(Count <= static_cast<size_t>((std::numeric_limits<int>::max)()));
	return static_cast<int>(Count);
}
}

bool InputText (const char* Label, FString& Value, ImGuiInputTextFlags Flags, ImGuiInputTextCallback Callback,
	void* UserData, uint32 MaxBytes)
{
	return EditString(Value, Flags, Callback, UserData, MaxBytes,
		[Label] (char* Buffer, size_t BufferSize, ImGuiInputTextFlags InFlags, ImGuiInputTextCallback InCallback, void* InUserData)
		{
			return ImGui::InputText(Label, Buffer, BufferSize, InFlags, InCallback, InUserData);
		});
}

bool InputTextWithHint (const char* Label, const char* Hint, FString& Value, ImGuiInputTextFlags Flags,
	ImGuiInputTextCallback Callback, void* UserData, uint32 MaxBytes)
{
	return EditString(Value, Flags, Callback, UserData, MaxBytes,
		[Label, Hint] (char* Buffer, size_t BufferSize, ImGuiInputTextFlags InFlags, ImGuiInputTextCallback InCallback, void* InUserData)
		{
			return ImGui::InputTextWithHint(Label, Hint, Buffer, BufferSize, InFlags, InCallback, InUserData);
		});
}

bool InputTextMultiline (const char* Label, FString& Value, const ImVec2& Size, ImGuiInputTextFlags Flags,
	ImGuiInputTextCallback Callback, void* UserData, uint32 MaxBytes)
{
	return EditString(Value, Flags, Callback, UserData, MaxBytes,
		[Label, &Size] (char* Buffer, size_t BufferSize, ImGuiInputTextFlags InFlags, ImGuiInputTextCallback InCallback, void* InUserData)
		{
			return ImGui::InputTextMultiline(Label, Buffer, BufferSize, Size, InFlags, InCallback, InUserData);
		});
}

// ImGui's getter overload takes the items as void*; the cast back lives here, typed, once.
bool Combo (const char* Label, int* Index, std::span<const FString> Items, int PopupMaxHeightInItems)
{
	return ImGui::Combo(Label, Index, &GetStringItem, const_cast<FString*>(Items.data()), GetItemCount(Items.size()),
		PopupMaxHeightInItems);
}

bool Combo (const char* Label, int* Index, std::span<const FStringLiteral> Items, int PopupMaxHeightInItems)
{
	return ImGui::Combo(Label, Index, &GetLiteralItem, const_cast<FStringLiteral*>(Items.data()), GetItemCount(Items.size()),
		PopupMaxHeightInItems);
}
}

#endif // !FRT_HEADLESS
