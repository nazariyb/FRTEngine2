#pragma once

#include <string>
#include <Windows.h>

#include "Core.h"


NAMESPACE_FRT_START

struct FRT_CORE_API WindowParams
{
	HINSTANCE hInst = nullptr;
	int startX = 0;
	int startY = 0;
	int width = 0;
	int height = 0;
	std::wstring className;
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

	void UpdateTitle(const std::wstring& NewTitleDetails) const;

protected:
	void RegisterWinAPIClass();

	//
	//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
	//
	//  PURPOSE: Processes messages for the main window.
	//
	//  WM_COMMAND  - process the application menu
	//  WM_PAINT    - Paint the main window
	//  WM_DESTROY  - post a quit message and return
	//
	//
	static LRESULT CALLBACK WindowProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

protected:
	WindowParams _params;
	HWND _hWnd;
	std::wstring _title;
};

NAMESPACE_FRT_END
