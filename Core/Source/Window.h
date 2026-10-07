#pragma once

#include <string>
#include <Windows.h>

#include "Core.h"
#include "Event.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Math/Math.h"
#include "User/UserSettings.h"


NAMESPACE_FRT_START
namespace graphics
{
	struct SDisplayOptions;
}


struct FRT_CORE_API SWindowParams
{
	HINSTANCE hInst = nullptr;
	int StartX = 0;
	int StartY = 0;
	int Width = 0;
	int Height = 0;

#pragma warning(push)
#pragma warning(disable: 4251)
	std::wstring ClassName;
	std::wstring Title = L"FRTEngine";
#pragma warning(pop)
};


class FRT_CORE_API CWindow
{
public:
	CWindow () = delete;
	~CWindow ();

	CWindow (const CWindow&) = delete;
	CWindow (CWindow&&) = delete;
	CWindow& operator= (const CWindow&) = delete;
	CWindow& operator= (CWindow&&) = delete;

	CWindow (const SWindowParams& Params);

	HWND GetHandle () const;
	Vector2f GetWindowSize () const;

	// TODO: probably options should be saved in Window?
	void SetDisplaySettings (const SDisplaySettings& NewSettings, const graphics::SDisplayOptions& Options);
	void UpdateTitle (const std::wstring& NewTitleDetails) const;

	// The window is the foreground (active) window: input reaches it.
	bool IsForeground () const;

	// Borderless fullscreen over the window's monitor, and back to the exact previous placement
	// (Unreal's immersive mode, the editor's F11). Never DXGI exclusive: tools must survive focus
	// changes. The swap chain follows through the regular WM_SIZE path.
	void SetImmersive (bool bImmersive);
	bool IsImmersive () const { return bImmersive; }

	// ---- cursor capture (a viewport flying its camera) ----
	// Client-area rectangle, in client pixels; the cursor cannot leave it until released.
	void ClipCursorToClientRect (int32 Left, int32 Top, int32 Right, int32 Bottom) const;
	void ReleaseCursorClip () const;
	Vector2i GetCursorClientPosition () const;
	void SetCursorClientPosition (const Vector2i& Position) const;

protected:
	void RegisterWinAPIClass ();

	static LRESULT CALLBACK SetupMessageProcessing (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK HandleMessageProcessing (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT CALLBACK WindowProcessMessage (HWND hWnd, UINT Message, WPARAM wParam, LPARAM lParam);

	void ResizeWithMove (const Vector2u& NewSize, const graphics::SRect& MonitorRect) const;
	void ResizeWithSetPos (const Vector2u& NewSize, const graphics::SRect& MonitorRect) const;
	void ResizeWithSetPos (const graphics::SRect& NewRect) const;
	void UpdateFullscreenMode (EFullscreenMode NewFullscreenMode, const graphics::SRect& MonitorRect);


	struct SPosSize
	{
		Vector2i Position;
		Vector2i Size;
	};


	static SPosSize CalcPositionAndSize (const Vector2u& NewSize, const graphics::SRect& MonitorRect);
	static SPosSize CalcPositionAndSize (const graphics::SRect& MonitorRect);

public:
	CEvent<> PostResizeEvent;
	CEvent<> PostLoseFocusEvent;
	CEvent<> PostGainFocusEvent;
	CEvent<> PostMinimizeEvent;
	CEvent<> PostRestoreFromMinimizeEvent;

protected:
	SWindowParams Params;
	HWND hWindow;
	SDisplaySettings DisplaySettings;
	bool bMinimized = false;
	bool bIgnoreNextSizeEvent = false;
	bool bImmersive = false;
	WINDOWPLACEMENT PlacementBeforeImmersive = { sizeof(WINDOWPLACEMENT) };
	LONG_PTR StyleBeforeImmersive = 0;

#pragma warning(push)
#pragma warning(disable: 4251)
	std::wstring Title;
#pragma warning(pop)
};


NAMESPACE_FRT_END
