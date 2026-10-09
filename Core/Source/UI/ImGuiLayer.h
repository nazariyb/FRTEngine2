#pragma once

#include <filesystem>

#include "Core.h"
#include "CoreTypes.h"
#include "UI/ImGuiFonts.h"


namespace frt
{
class CWindow;
}

namespace frt::graphics
{
class CRenderer;
}

namespace frt::input
{
class CInputSystem;
}


namespace frt
{
/**
 * The Dear ImGui context and its Win32 + DX12 backends: the engine's immediate-mode UI layer.
 *
 * UI scale is user zoom (Ctrl+Wheel) times the monitor DPI scale, applied to fonts
 * (style.FontScaleMain - 1.92 rasterises on demand, so it stays crisp) and to metrics
 * (ScaleAllSizes). ScaleAllSizes is not idempotent, so every change rebuilds the live style from
 * an unscaled copy - which is also where a tint (the editor's play-mode cue) is applied.
 * Font atlas and user textures live in the renderer's bindless heap, so view textures are
 * sampled straight from their bindless SRVs.
 */
class FRT_CORE_API CImGuiLayer
{
public:
	CImGuiLayer () = default;
	CImGuiLayer (const CImGuiLayer&) = delete;
	CImGuiLayer& operator= (const CImGuiLayer&) = delete;

	// IniPath: where the layout is kept; empty = imgui.ini in the working directory.
	void StartUp (CWindow& Window, graphics::CRenderer& Renderer, const std::filesystem::path& IniPath);
	void ShutDown ();

	// No layout file from now on: none is written, and none is read if the first frame has not
	// begun yet (the file is read then, as ImGui does). For scripted runs and screens without a
	// layout: they start from the default and leave the user's alone.
	void DisableIniFile ();

	// Ctrl+Wheel zoom from this frame's input, then NewFrame.
	void BeginFrame (const input::CInputSystem& Input);
	// Draws the frame's UI into the bound back buffer.
	void Render (graphics::CRenderer& Renderer);
	// Closes a frame that will not be rendered (nothing to present to).
	void EndFrameWithoutRender ();

	// Multiplies the window, title, menu and tab colours (1,1,1 = none). Applied from the next
	// frame on.
	void SetTint (float R, float G, float B);

	float GetScale () const { return ScaleApplied; }

private:
	void ApplyStyle (float DpiScale);
	void SaveIniFile ();

	CWindow* Window = nullptr;
	// The layout goes through memory (LoadIniSettingsFromMemory / SaveIniSettingsToMemory), not
	// ImGui's own file functions: they take the path as UTF-8 text, which cannot spell every name
	// Windows allows (an unpaired surrogate becomes U+FFFD).
	std::filesystem::path IniPath;
	bool bIniLoaded = false;                   // the first frame has read the layout (or there was none)
	ui::CFontSet Fonts;                        // the atlas reads the font files from here until ShutDown
	float Zoom = 1.0f;
	float ScaleApplied = 0.0f;
	float Tint[3] = { 1.0f, 1.0f, 1.0f };
	float TintApplied[3] = { 1.0f, 1.0f, 1.0f };
	bool bStarted = false;
	struct SImpl;
	SImpl* Impl = nullptr;                     // backend init info + unscaled style (keeps imgui.h out of this header)
};
}
