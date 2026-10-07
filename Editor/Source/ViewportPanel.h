#pragma once

#include "imgui.h"

#include "CoreTypes.h"


namespace frt::graphics
{
class CRenderer;
class CRenderView;
}


namespace frt::editor
{
// Unity's two views: the Scene view looks at the world through the editor's own camera, the
// Game view through the world's - what the player sees.
enum class EViewportKind : uint8
{
	Scene,
	Game,
};

// Game view aspect ratios (Unity's Game view aspect menu): the image is letterboxed to it.
enum class EViewportAspect : uint8
{
	Free,
	Aspect16x9,
	Aspect16x10,
	Aspect4x3,
	Aspect21x9,
	Count
};

// How a viewport is shown this frame.
enum class EViewportPresentation : uint8
{
	Docked,        // in its own window: docked, tabbed or floating
	Covered,       // another viewport fills the editor: stays docked, renders nothing
	Maximized,     // fills the editor's work area, below the menu and toolbar
	Fullscreen,    // fills the window, which fills the screen; image only
};

struct SViewportDrawArgs
{
	EViewportPresentation Presentation = EViewportPresentation::Docked;
	bool bTakeFocus = false;       // focus the panel this frame
	bool bPlaying = false;         // a play session runs: the Game view wears the play frame
	bool bPaused = false;
	float FrameMilliseconds = 0.0f;
	// Shortcuts the panel's menus show, from the editor's bindings ("" when unbound).
	const char* MaximizeShortcut = "";
	const char* RenderModeShortcut = "";
	const char* FullscreenExitHint = "";     // shown briefly over a fullscreen viewport
	// The viewport camera's speed, editable from the toolbar; null for a view without one.
	float* CameraSpeed = nullptr;
	float DefaultCameraSpeed = 1.0f;
};

// What happened in a viewport this frame. Rebuilt every frame (immediate mode): the panel
// observes, the editor decides what follows - camera control, game input, commands.
struct SViewportFrame
{
	bool bVisible = false;         // on screen: its view is rendered this frame
	bool bHovered = false;         // the cursor is over the viewport's surface
	bool bFocused = false;         // the panel has keyboard focus
	// A mouse drag that began on the surface: it lasts while any of the left, right and middle
	// buttons is held, whichever started it - Unreal's L+R pans however they were pressed.
	bool bNavigating = false;
	uint8 NavigationButtons = 0u;  // held during the drag: viewport_button bits (ViewportCamera.h)
	bool bToggleMaximize = false;  // the user asked to maximize or restore it
	ImVec2 ImageMin = {};          // the image, in client pixels
	ImVec2 ImageMax = {};
};

/**
 * A 3D viewport: an ImGui window showing one render view (Unity's Scene and Game views,
 * Unreal's level viewports). Docks, tabs, floats and resizes like any panel; maximizes over
 * the editor (or double-clicking its tab) or over the whole screen without touching the dock
 * layout.
 *
 * Its view is a texture view the size of the panel in physical pixels, drawn 1:1. Resizes are
 * debounced: while a splitter is dragged the last image is scaled into the panel, and the
 * view is reallocated once the size settles, so the path tracer starts its histories over
 * once per resize instead of on every frame of the drag. A viewport that is not on screen -
 * a background tab, covered, collapsed, closed - is not rendered at all.
 */
class CViewportPanel
{
public:
	CViewportPanel (EViewportKind Kind, const char* Name);

	void Create (graphics::CRenderer& Renderer);
	void Destroy (graphics::CRenderer& Renderer);

	const SViewportFrame& Draw (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args);
	const SViewportFrame& GetFrame () const { return Frame; }

	graphics::CRenderView& GetView () const { return *View; }
	EViewportKind GetKind () const { return Kind; }
	const char* GetName () const { return Name; }
	// Width over height of the view (1 before it has a size).
	float GetAspect () const;

	bool bOpen = true;
	bool bShowStats = true;
	EViewportAspect Aspect = EViewportAspect::Free;

private:
	void DrawContents (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args, bool bToolbar);
	void DrawToolbar (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args);
	void DrawSurface (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args);
	void DrawOverlays (graphics::CRenderer& Renderer, const SViewportDrawArgs& Args, const ImVec2& AreaMin, const ImVec2& AreaMax);
	void UpdateSize (graphics::CRenderer& Renderer, uint32 Width, uint32 Height);

	EViewportKind Kind;
	const char* Name;
	char OverlayName[48] = {};
	graphics::CRenderView* View = nullptr;

	// Debounced resize: Committed is the view's size, Pending the size the panel last asked for.
	uint32 CommittedWidth = 0u;
	uint32 CommittedHeight = 0u;
	uint32 PendingWidth = 0u;
	uint32 PendingHeight = 0u;
	uint32 PendingFrames = 0u;

	EViewportPresentation LastPresentation = EViewportPresentation::Docked;
	double PresentationChangeTime = 0.0;

	// The surface drag, carried from frame to frame (see SViewportFrame::bNavigating).
	bool bNavigating = false;
	bool bSurfaceDrawn = false;

	SViewportFrame Frame;
};

const char* GetAspectName (EViewportAspect Aspect);
}
