#include "MainToolbar.h"

#include <cstdio>

#include "imgui.h"
#include "imgui_internal.h"


namespace frt::editor
{
namespace
{
enum class EIcon : uint8
{
	Play,
	Pause,
	Step,
};

// Drawn rather than taken from a font: the UI font has no media glyphs, and shapes stay crisp
// at every UI scale.
void DrawIcon (ImDrawList& DrawList, EIcon Icon, const ImVec2& Min, const ImVec2& Max, ImU32 Color)
{
	const ImVec2 centre((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
	const float h = (Max.y - Min.y) * 0.25f;   // half the glyph's height
	switch (Icon)
	{
		case EIcon::Play:
		{
			DrawList.AddTriangleFilled(
				ImVec2(centre.x - h * 0.8f, centre.y - h),
				ImVec2(centre.x + h * 0.9f, centre.y),
				ImVec2(centre.x - h * 0.8f, centre.y + h), Color);
			break;
		}
		case EIcon::Pause:
		{
			const float bar = h * 0.55f;
			const float gap = h * 0.35f;
			DrawList.AddRectFilled(ImVec2(centre.x - gap - bar, centre.y - h), ImVec2(centre.x - gap, centre.y + h), Color);
			DrawList.AddRectFilled(ImVec2(centre.x + gap, centre.y - h), ImVec2(centre.x + gap + bar, centre.y + h), Color);
			break;
		}
		case EIcon::Step:
		{
			const float left = centre.x - h * 0.9f;
			DrawList.AddTriangleFilled(
				ImVec2(left, centre.y - h),
				ImVec2(left + h * 1.3f, centre.y),
				ImVec2(left, centre.y + h), Color);
			DrawList.AddRectFilled(ImVec2(left + h * 1.4f, centre.y - h), ImVec2(left + h * 1.9f, centre.y + h), Color);
			break;
		}
	}
}

bool IconButton (const char* Id, EIcon Icon, bool bToggled, bool bEnabled, const char* Tooltip)
{
	const float height = ImGui::GetFrameHeight();
	ImGui::BeginDisabled(!bEnabled);
	// Neutral unless toggled, so the one that is on stands out.
	ImGui::PushStyleColor(ImGuiCol_Button, bToggled ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0.5f, 0.5f, 0.5f, 0.18f));
	const bool bPressed = ImGui::Button(Id, ImVec2(height * 1.6f, height));
	ImGui::PopStyleColor();
	// GetColorU32 applies the disabled alpha.
	DrawIcon(*ImGui::GetWindowDrawList(), Icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text));
	ImGui::SetItemTooltip("%s", Tooltip);
	ImGui::EndDisabled();
	return bPressed;
}
}

std::optional<EEditorCommand> DrawMainToolbar (const SToolbarState& State)
{
	std::optional<EEditorCommand> command;
	const ImGuiStyle& style = ImGui::GetStyle();
	const float height = ImGui::GetFrameHeight() + style.WindowPadding.y;
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, style.WindowPadding.y * 0.5f));
	const bool bOpen = ImGui::BeginViewportSideBar("##MainToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags);
	ImGui::PopStyleVar();
	if (bOpen)
	{
		// Centred play controls.
		const float button = ImGui::GetFrameHeight() * 1.6f;
		const float controls = button * 3.0f + style.ItemSpacing.x * 2.0f;
		ImGui::SetCursorPosX((ImGui::GetWindowWidth() - controls) * 0.5f);
		if (IconButton("##Play", EIcon::Play, State.bPlaying, true, State.bPlaying ? "Stop (Ctrl+P)" : "Play (Ctrl+P)"))
		{
			command = EEditorCommand::TogglePlay;
		}
		ImGui::SameLine();
		if (IconButton("##Pause", EIcon::Pause, State.bPaused, true, State.bPlaying ? "Pause (Ctrl+Shift+P)" : "Start paused (Ctrl+Shift+P)"))
		{
			command = EEditorCommand::TogglePause;
		}
		ImGui::SameLine();
		if (IconButton("##Step", EIcon::Step, false, State.bPlaying, "Step one frame (Ctrl+Alt+P)"))
		{
			command = EEditorCommand::Step;
		}

		// The play state, right-aligned.
		char status[128];
		if (State.Message)
		{
			std::snprintf(status, sizeof(status), "%s", State.Message);
		}
		else if (State.bPlaying)
		{
			std::snprintf(status, sizeof(status), "%s  %.2f s  frame %llu", State.bPaused ? "Paused" : "Playing",
				State.PlaySeconds, static_cast<unsigned long long>(State.PlayFrames));
		}
		else
		{
			std::snprintf(status, sizeof(status), "Edit mode");
		}
		const float statusWidth = ImGui::CalcTextSize(status).x;
		ImGui::SameLine(ImGui::GetWindowWidth() - statusWidth - style.WindowPadding.x * 2.0f);
		ImGui::AlignTextToFramePadding();
		if (State.Message)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", status);
		}
		else
		{
			ImGui::TextDisabled("%s", status);
		}
	}
	ImGui::End();
	return command;
}
}
