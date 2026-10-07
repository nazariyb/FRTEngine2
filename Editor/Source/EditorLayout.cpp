#include "EditorLayout.h"

#include "imgui_internal.h"


namespace frt::editor
{
namespace
{
// Fixed rather than derived from a window: saved layouts must keep finding it.
const char* const DockSpaceName = "FRT.Editor.DockSpace";

// Panels the default layout places, by window name: the viewports (CViewportPanel) and the
// debug tools' panels (CDebugTools).
const char* const ScenePanel = "Scene";
const char* const GamePanel = "Game";
const char* const RendererPanel = "Renderer";
const char* const SkyPanel = "Sky / Sun";
const char* const EditorPanel = "Editor";
const char* const StatsPanel = "Stats";
}

bool CEditorLayout::Draw ()
{
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImGuiID dockSpace = ImHashStr(DockSpaceName);

	// Before the dock space is submitted: DockBuilder edits the tree it is about to lay out.
	bool bBuilt = false;
	if (bResetRequested || ImGui::DockBuilderGetNode(dockSpace) == nullptr)
	{
		BuildDefault(dockSpace);
		bResetRequested = false;
		bBuilt = true;
	}

	ImGui::DockSpaceOverViewport(dockSpace, viewport, ImGuiDockNodeFlags_None);
	return bBuilt;
}

void CEditorLayout::BuildDefault (ImGuiID DockSpace)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();

	ImGui::DockBuilderRemoveNode(DockSpace);
	ImGui::DockBuilderAddNode(DockSpace, ImGuiDockNodeFlags_DockSpace);
	// Sized before splitting, or the split ratios apply to an empty node.
	ImGui::DockBuilderSetNodePos(DockSpace, viewport->WorkPos);
	ImGui::DockBuilderSetNodeSize(DockSpace, viewport->WorkSize);

	ImGuiID centre = DockSpace;
	ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f, nullptr, &centre);
	const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
	const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.24f, nullptr, &centre);

	ImGui::DockBuilderDockWindow(ScenePanel, centre);
	ImGui::DockBuilderDockWindow(GamePanel, centre);
	ImGui::DockBuilderDockWindow(RendererPanel, right);
	ImGui::DockBuilderDockWindow(SkyPanel, rightBottom);
	ImGui::DockBuilderDockWindow(EditorPanel, rightBottom);
	ImGui::DockBuilderDockWindow(StatsPanel, bottom);
	ImGui::DockBuilderFinish(DockSpace);
}
}
