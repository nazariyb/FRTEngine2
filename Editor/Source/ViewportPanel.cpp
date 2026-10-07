#include "ViewportPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui_internal.h"

#include "Asserts.h"
#include "Graphics/Render/Renderer.h"
#include "Graphics/Render/RenderView.h"


namespace frt::editor
{
namespace
{
// Frames a new panel size must hold, while a mouse drag goes on, before the view follows it.
constexpr uint32 ResizeSettleFrames = 4u;
// How long the "F11 exits fullscreen" hint stays.
constexpr double FullscreenHintSeconds = 3.0;

constexpr ImGuiWindowFlags ViewportWindowFlags =
	ImGuiWindowFlags_MenuBar |
	ImGuiWindowFlags_NoScrollbar |
	ImGuiWindowFlags_NoScrollWithMouse |
	ImGuiWindowFlags_NoCollapse |
	// Keyboard navigation would take Space, arrows and Enter from the camera and the game.
	ImGuiWindowFlags_NoNavInputs;

constexpr ImU32 LetterboxColor = IM_COL32(12, 12, 12, 255);
constexpr ImU32 OverlayText = IM_COL32(235, 235, 235, 255);
constexpr ImU32 PlayFrameColor = IM_COL32(64, 156, 255, 255);
constexpr ImU32 PausedColor = IM_COL32(255, 196, 64, 255);

float GetAspectValue (EViewportAspect Aspect)
{
	switch (Aspect)
	{
		case EViewportAspect::Aspect16x9:  return 16.0f / 9.0f;
		case EViewportAspect::Aspect16x10: return 16.0f / 10.0f;
		case EViewportAspect::Aspect4x3:   return 4.0f / 3.0f;
		case EViewportAspect::Aspect21x9:  return 21.0f / 9.0f;
		default:                           return 0.0f;
	}
}

// The largest Aspect rectangle inside Area (all of it for a free aspect).
ImVec2 FitAspect (const ImVec2& Area, float Aspect)
{
	if (Aspect <= 0.0f)
	{
		return Area;
	}
	return Area.x > Area.y * Aspect ? ImVec2(Area.y * Aspect, Area.y) : ImVec2(Area.x, Area.x / Aspect);
}

// Text on a dark plate, the way viewport readouts are drawn over the image.
void DrawLabel (ImDrawList& DrawList, const ImVec2& Pos, const char* Text, ImU32 Color, float Alpha = 1.0f)
{
	const ImGuiStyle& style = ImGui::GetStyle();
	const ImVec2 size = ImGui::CalcTextSize(Text);
	const ImVec2 pad = style.FramePadding;
	const ImU32 background = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, (150.0f / 255.0f) * Alpha));
	const ImU32 text = (Color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((Color >> IM_COL32_A_SHIFT) & 0xFF) * Alpha) << IM_COL32_A_SHIFT);
	DrawList.AddRectFilled(Pos, ImVec2(Pos.x + size.x + pad.x * 2.0f, Pos.y + size.y + pad.y * 2.0f), background, style.FrameRounding);
	DrawList.AddText(ImVec2(Pos.x + pad.x, Pos.y + pad.y), text, Text);
}
}

const char* GetAspectName (EViewportAspect Aspect)
{
	switch (Aspect)
	{
		case EViewportAspect::Aspect16x9:  return "16:9";
		case EViewportAspect::Aspect16x10: return "16:10";
		case EViewportAspect::Aspect4x3:   return "4:3";
		case EViewportAspect::Aspect21x9:  return "21:9";
		default:                           return "Free Aspect";
	}
}

CViewportPanel::CViewportPanel (EViewportKind InKind, const char* InName)
	: Kind(InKind)
	, Name(InName)
{
	std::snprintf(OverlayName, sizeof(OverlayName), "##%s.Overlay", Name);
}

void CViewportPanel::Create (graphics::CRenderer& Renderer)
{
	frt_assert(View == nullptr);
	// Name is a literal: the GPU profiler keeps the pointer as the view's scope tag.
	View = Renderer.CreateView({ Name, graphics::EViewTarget::Texture });
}

void CViewportPanel::Destroy (graphics::CRenderer& Renderer)
{
	if (View)
	{
		Renderer.DestroyView(View);
		View = nullptr;
	}
}

float CViewportPanel::GetAspect () const
{
	return CommittedWidth > 0u && CommittedHeight > 0u ? static_cast<float>(CommittedWidth) / static_cast<float>(CommittedHeight) : 1.0f;
}

const SViewportFrame& CViewportPanel::Draw (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args)
{
	Frame = {};
	if (Args.Presentation != LastPresentation)
	{
		LastPresentation = Args.Presentation;
		PresentationChangeTime = ImGui::GetTime();
	}
	if (!bOpen)
	{
		return Frame;
	}

	const bool bDocked = Args.Presentation == EViewportPresentation::Docked;
	const bool bMaximized = Args.Presentation == EViewportPresentation::Maximized;
	const bool bFullscreen = Args.Presentation == EViewportPresentation::Fullscreen;

	// The panel's own window is submitted every frame, also while covered or shown over the
	// editor: that keeps its dock slot and its tab.
	ImGui::SetNextWindowSize(ImVec2(960.0f, 540.0f), ImGuiCond_FirstUseEver);
	if (Args.bTakeFocus && bDocked)
	{
		ImGui::SetNextWindowFocus();
	}
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool bShown = ImGui::Begin(Name, &bOpen, ViewportWindowFlags);
	ImGui::PopStyleVar();
	// Right after Begin the last item is the window's tab (docked) or title bar (floating).
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		Frame.bToggleMaximize = true;
	}
	if (ImGui::BeginPopupContextItem("##TabMenu"))
	{
		if (ImGui::MenuItem(bMaximized ? "Restore" : "Maximize", "Shift+Space"))
		{
			Frame.bToggleMaximize = true;
		}
		ImGui::EndPopup();
	}
	if (bShown && bDocked)
	{
		DrawContents(Renderer, Args, true);
	}
	ImGui::End();

	if (bMaximized || bFullscreen)
	{
		// A plain window over the dock space (Unity's maximized view): the layout underneath
		// stays exactly as it was.
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(bFullscreen ? viewport->Pos : viewport->WorkPos);
		ImGui::SetNextWindowSize(bFullscreen ? viewport->Size : viewport->WorkSize);
		if (Args.bTakeFocus)
		{
			ImGui::SetNextWindowFocus();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		const ImGuiWindowFlags flags = (ViewportWindowFlags & ~(bFullscreen ? ImGuiWindowFlags_MenuBar : 0)) |
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
		ImGui::Begin(OverlayName, nullptr, flags);
		ImGui::PopStyleVar(3);
		DrawContents(Renderer, Args, !bFullscreen);
		ImGui::End();
	}
	return Frame;
}

void CViewportPanel::DrawContents (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args, bool bToolbar)
{
	if (bToolbar)
	{
		DrawToolbar(Renderer, Args);
	}
	DrawSurface(Renderer, Args);
	Frame.bFocused = ImGui::IsWindowFocused();
}

void CViewportPanel::DrawToolbar (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args)
{
	if (!ImGui::BeginMenuBar())
	{
		return;
	}

	// Over the editor nothing else says which view this is.
	const bool bMaximized = Args.Presentation == EViewportPresentation::Maximized;
	if (bMaximized)
	{
		ImGui::TextUnformatted(Name);
		ImGui::Separator();
	}

	const bool bRaster = View->GetRenderMode() == graphics::ERenderMode::Raster;
	if (ImGui::BeginMenu(bRaster ? "Raster" : "Path Traced"))
	{
		if (ImGui::MenuItem("Path Traced", "F2", !bRaster, Renderer.CanTrace()))
		{
			View->SetRenderMode(graphics::ERenderMode::Raytracing);
		}
		if (ImGui::MenuItem("Raster", "F2", bRaster))
		{
			View->SetRenderMode(graphics::ERenderMode::Raster);
		}
		ImGui::EndMenu();
	}

	if (Kind == EViewportKind::Game && ImGui::BeginMenu(GetAspectName(Aspect)))
	{
		for (uint8 i = 0u; i < static_cast<uint8>(EViewportAspect::Count); ++i)
		{
			const EViewportAspect aspect = static_cast<EViewportAspect>(i);
			if (ImGui::MenuItem(GetAspectName(aspect), nullptr, Aspect == aspect))
			{
				Aspect = aspect;
			}
		}
		ImGui::EndMenu();
	}

	// Right-aligned: the stats toggle and maximize / restore.
	const char* maximizeLabel = bMaximized ? "Restore" : "Maximize";
	const ImGuiStyle& style = ImGui::GetStyle();
	const float rightWidth = ImGui::CalcTextSize("Stats").x + ImGui::CalcTextSize(maximizeLabel).x + style.ItemSpacing.x * 4.0f;
	const float rightX = ImGui::GetWindowContentRegionMax().x - rightWidth;
	if (rightX > ImGui::GetCursorPosX())
	{
		ImGui::SetCursorPosX(rightX);
	}
	if (ImGui::MenuItem("Stats", nullptr, bShowStats))
	{
		bShowStats = !bShowStats;
	}
	if (ImGui::MenuItem(maximizeLabel))
	{
		Frame.bToggleMaximize = true;
	}
	ImGui::EndMenuBar();
}

void CViewportPanel::UpdateSize (graphics::CRenderer& Renderer, uint32 Width, uint32 Height)
{
	if (Width == CommittedWidth && Height == CommittedHeight)
	{
		PendingWidth = Width;
		PendingHeight = Height;
		PendingFrames = 0u;
		return;
	}
	if (Width != PendingWidth || Height != PendingHeight)
	{
		PendingWidth = Width;
		PendingHeight = Height;
		PendingFrames = 0u;
	}
	else
	{
		++PendingFrames;
	}

	// Follow at once unless a drag is going on (a splitter, a floating window's edge); then
	// once the size has held for a few frames, or the drag ends.
	const bool bFirst = CommittedWidth == 0u;
	const bool bDragging = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	if (bFirst || !bDragging || PendingFrames >= ResizeSettleFrames)
	{
		Renderer.SetViewSize(*View, Width, Height);
		CommittedWidth = Width;
		CommittedHeight = Height;
		PendingFrames = 0u;
	}
}

void CViewportPanel::DrawSurface (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args)
{
	const ImVec2 areaMin = ImGui::GetCursorScreenPos();
	const ImVec2 area = ImGui::GetContentRegionAvail();
	if (area.x < 1.0f || area.y < 1.0f)
	{
		return;
	}
	const ImVec2 areaMax(areaMin.x + area.x, areaMin.y + area.y);

	// Physical pixels: the Win32 backend keeps ImGui coordinates in pixels, so the view's
	// resolution is the panel's, and the image is drawn 1:1.
	const ImVec2 wanted = FitAspect(area, GetAspectValue(Kind == EViewportKind::Game ? Aspect : EViewportAspect::Free));
	UpdateSize(Renderer,
		(std::max)(1u, static_cast<uint32>(wanted.x)),
		(std::max)(1u, static_cast<uint32>(wanted.y)));

	// The whole panel is one surface for every mouse button: a press anywhere on it - the
	// letterbox included - belongs to the viewport, never to a window move or a dock drag.
	ImGui::InvisibleButton("##Surface", area,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
	Frame.bVisible = true;
	Frame.bHovered = ImGui::IsItemHovered();
	Frame.bSurfaceActive = ImGui::IsItemActive();
	Frame.bRightHeld = Frame.bSurfaceActive && ImGui::GetCurrentContext()->ActiveIdMouseButton == ImGuiMouseButton_Right;
	if (ImGui::IsItemActivated())
	{
		// Any button focuses the viewport, as in Unity - ImGui alone focuses on the left one.
		ImGui::SetWindowFocus();
	}

	// The view's image, centred; scaled down only while a resize is pending.
	const float width = static_cast<float>(CommittedWidth);
	const float height = static_cast<float>(CommittedHeight);
	const float scale = (std::min)(1.0f, (std::min)(area.x / width, area.y / height));
	const ImVec2 size(std::floor(width * scale), std::floor(height * scale));
	const ImVec2 imageMin(std::floor(areaMin.x + (area.x - size.x) * 0.5f), std::floor(areaMin.y + (area.y - size.y) * 0.5f));
	const ImVec2 imageMax(imageMin.x + size.x, imageMin.y + size.y);
	Frame.ImageMin = imageMin;
	Frame.ImageMax = imageMax;

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(areaMin, areaMax, LetterboxColor);
	if (View->GetTextureId() != 0u)
	{
		// 1:1 on whole pixels: point sampling reproduces the view exactly.
		const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
		const bool bExact = scale >= 1.0f && platform.DrawCallback_SetSamplerNearest && platform.DrawCallback_SetSamplerLinear;
		if (bExact)
		{
			drawList->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
		}
		drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(View->GetTextureId())), imageMin, imageMax);
		if (bExact)
		{
			drawList->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
		}
	}

	DrawOverlays(Renderer, Args, imageMin, imageMax);
}

void CViewportPanel::DrawOverlays (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args, const ImVec2& Min, const ImVec2& Max)
{
	ImDrawList& drawList = *ImGui::GetWindowDrawList();
	const ImGuiStyle& style = ImGui::GetStyle();
	const float unit = ImGui::GetFontSize();

	// Play mode: the Game view wears a frame, so it is never mistaken for the edited world.
	if (Args.bPlaying && Kind == EViewportKind::Game)
	{
		const float thickness = (std::max)(2.0f, std::round(unit * 0.15f));
		const ImU32 color = Args.bPaused ? PausedColor : PlayFrameColor;
		drawList.AddRect(
			ImVec2(Min.x + thickness * 0.5f, Min.y + thickness * 0.5f),
			ImVec2(Max.x - thickness * 0.5f, Max.y - thickness * 0.5f),
			color, 0.0f, 0, thickness);
		if (Args.bPaused)
		{
			const char* text = "PAUSED";
			const float textWidth = ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f;
			DrawLabel(drawList, ImVec2(std::floor((Min.x + Max.x - textWidth) * 0.5f), Min.y + unit * 0.75f), text, PausedColor);
		}
	}

	if (bShowStats)
	{
		const bool bTracing = Renderer.IsTracing(*View);
		// The size asked for this frame: the view itself only takes it when rendered, after the UI.
		char text[160];
		int length = std::snprintf(text, sizeof(text), "%u x %u  %s\n%.2f ms",
			CommittedWidth, CommittedHeight,
			bTracing ? "path traced" : "raster",
			Args.FrameMilliseconds);
		if (bTracing && Renderer.Profile.Frame.Mode == graphics::EPathTracingMode::Reference && length > 0)
		{
			std::snprintf(text + length, sizeof(text) - length, "  %u spp", View->GetAccumulatedSamples());
		}
		DrawLabel(drawList, ImVec2(Min.x + unit * 0.5f, Min.y + unit * 0.5f), text, OverlayText);
	}

	if (Args.Presentation == EViewportPresentation::Fullscreen)
	{
		const double shown = ImGui::GetTime() - PresentationChangeTime;
		if (shown < FullscreenHintSeconds)
		{
			const char* text = "F11  exit fullscreen";
			const float fade = static_cast<float>((std::min)(1.0, (FullscreenHintSeconds - shown) / 0.5));
			const float textWidth = ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f;
			DrawLabel(drawList, ImVec2(std::floor((Min.x + Max.x - textWidth) * 0.5f), Max.y - unit * 3.0f), text, OverlayText, fade);
		}
	}
}
}
