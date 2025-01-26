#pragma once

#include <string>
#include <Windows.h>

#include "Core.h"
#include "CoreTypes.h"
#include "Event.h"
#include "Math/Math.h"


NAMESPACE_FRT_START
	namespace graphics
	{
		struct SRect;
	}

	struct FRT_CORE_API WindowParams
{
	HINSTANCE hInst = nullptr;
	int startX = 0;
	int startY = 0;
	int width = 0;
	int height = 0;

#pragma warning(push)
#pragma warning(disable: 4251)
	std::wstring className;
#pragma warning(pop)
};

class FRT_CORE_API Window
{
public:
	Window() = delete;
	~Window();

	Window(const Window&) = delete;
	Window(Window&&) = delete;
	Window& operator=(const Window&) = delete;
	Window& operator=(Window&&) = delete;

	Window(const WindowParams& Params);

	HWND GetHandle() const;
	Vector2f GetWindowSize() const;

	void Move(const Vector2u& NewSize, const graphics::SRect& MonitorRect);
	void UpdateTitle(const std::wstring& NewTitleDetails) const;

protected:
	void RegisterWinAPIClass();

	static LRESULT CALLBACK SetupMessageProcessing(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK HandleMessageProcessing(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT CALLBACK WindowProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

public:
	CEvent<> PostResizeEvent;

protected:
	WindowParams _params;
	HWND _hWindow;

#pragma warning(push)
#pragma warning(disable: 4251)
	std::wstring _title;
#pragma warning(pop)
};

NAMESPACE_FRT_END
