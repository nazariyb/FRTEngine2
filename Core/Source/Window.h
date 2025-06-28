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

#pragma warning(push)
#pragma warning(disable: 4251)
	std::wstring Title;
#pragma warning(pop)
};


NAMESPACE_FRT_END
