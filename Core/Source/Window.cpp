#include "Window.h"

#include <iostream>

#include "CoreUtils.h"
#include "Math/Math.h"

#include "imgui.h"
#include "Graphics/RenderCommonTypes.h"

NAMESPACE_FRT_START
	Window::~Window()
{
}

Window::Window(const WindowParams& Params)
	: _params(Params)
{
	RegisterWinAPIClass();

	const int right = _params.startX + _params.width;
	const int bottom = _params.startY + _params.height;
	RECT wr {.left = _params.startX, .top = _params.startY, .right = right, .bottom = bottom};
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, false);

	_title = L"FRTEngine";
	_hWindow = CreateWindow(_params.className.c_str(), _title.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		_params.width, _params.height,
		nullptr /*hWndParent*/, nullptr /*hMenu*/, _params.hInst,
		this /*lpParam*/);

	if (!_hWindow) // TODO: move this if outside
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
	}

	DisplaySettings = SDisplaySettings
		{
			.MonitorIndex = -1,
			.ResolutionIndex = -1,
			.RefreshRateIndex = -1
		};

	ShowWindow(_hWindow, SW_SHOWDEFAULT);
	UpdateWindow(_hWindow);
}

HWND Window::GetHandle() const
{
	return _hWindow;
}

Vector2f Window::GetWindowSize() const
{
	return { static_cast<float>(_params.width), static_cast<float>(_params.height) };
}

void Window::SetDisplaySettings(const SDisplaySettings& NewSettings, const graphics::SDisplayOptions& Options)
{
	if (NewSettings == DisplaySettings)
	{
		return;
	}

	const bool bMonitorChangedInFullscreen = (NewSettings.MonitorIndex != DisplaySettings.MonitorIndex)
		&& DisplaySettings.IsFullscreen();
	if (DisplaySettings.FullscreenMode != NewSettings.FullscreenMode || bMonitorChangedInFullscreen)
	{
		UpdateFullscreenMode(NewSettings.FullscreenMode, Options.OutputsRects[NewSettings.MonitorIndex]);
	}
	else
	{
		const auto& resolutions = Options.GetResolutionsEncoded((uint32)NewSettings.MonitorIndex);
		Vector2u newSize;
		math::DecodeTwoFromOne(resolutions[NewSettings.ResolutionIndex], newSize.x, newSize.y);
		ResizeWithMove(newSize, Options.OutputsRects[NewSettings.MonitorIndex]);
	}

	DisplaySettings = NewSettings;
}

// TODO: change physical resolution in fullscreen
void SetResolution(int width, int height, int refreshRate = 60)
{
	DEVMODE devMode = {};
	devMode.dmSize = sizeof(DEVMODE);
	devMode.dmPelsWidth = width;
	devMode.dmPelsHeight = height;
	devMode.dmBitsPerPel = 32;
	devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
	devMode.dmDisplayFrequency = refreshRate;

	ChangeDisplaySettings(&devMode, CDS_FULLSCREEN);
}

void Window::ResizeWithMove(const Vector2u& NewSize, const graphics::SRect& MonitorRect) const
{
	const auto [pos, size] = CalcPositionAndSize(NewSize, MonitorRect);
	MoveWindow(_hWindow, pos.x, pos.y, size.x, size.y, true);
}

void Window::ResizeWithSetPos(const Vector2u& NewSize, const graphics::SRect& MonitorRect) const
{
	const auto [pos, size] = CalcPositionAndSize(NewSize, MonitorRect);
	SetWindowPos(_hWindow, HWND_TOP, pos.x, pos.y, size.x, size.y, SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void Window::ResizeWithSetPos(const graphics::SRect& NewRect) const
{
	const auto [pos, size] = CalcPositionAndSize(NewRect);
	SetWindowPos(_hWindow, HWND_TOP, pos.x, pos.y, size.x, size.y, SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void Window::UpdateFullscreenMode(EFullscreenMode NewFullscreenMode, const graphics::SRect& MonitorRect)
{
	if (NewFullscreenMode == EFullscreenMode::Fullscreen)
	{
		{
			// using guard doesn't look right...
			TGuardValue GuardIgnoreNextSizeEvent(bIgnoreNextSizeEvent, true);
			SetWindowLong(_hWindow, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		}
		ResizeWithSetPos(MonitorRect);
		ShowWindow(_hWindow, SW_MAXIMIZE);
	}
	else if (NewFullscreenMode == EFullscreenMode::Borderless)
	{
		{
			TGuardValue GuardIgnoreNextSizeEvent(bIgnoreNextSizeEvent, true);
			SetWindowLong(_hWindow, GWL_STYLE, WS_POPUP);
		}
		ResizeWithSetPos(MonitorRect);
		ShowWindow(_hWindow, SW_MAXIMIZE);
	}
	else
	{
		{
			TGuardValue GuardIgnoreNextSizeEvent(bIgnoreNextSizeEvent, true);
			SetWindowLong(_hWindow, GWL_STYLE, WS_OVERLAPPEDWINDOW);
		}
		ResizeWithSetPos(MonitorRect);
		ShowWindow(_hWindow, SW_NORMAL);
	}
}

Window::SPosSize Window::CalcPositionAndSize(const Vector2u& NewSize, const graphics::SRect& MonitorRect)
{
	const int32 newX = (int32)(MonitorRect.Right - MonitorRect.Left) / 2 - NewSize.x / 2 + (int32)MonitorRect.Left;
	const int32 newY = (int32)(MonitorRect.Bottom - MonitorRect.Top) / 2 - NewSize.y / 2 + (int32)MonitorRect.Top;
	return SPosSize
		{
			.Position = { newX, newY },
			.Size = { (int32)NewSize.x, (int32)NewSize.y }
		};
}

Window::SPosSize Window::CalcPositionAndSize(const graphics::SRect& MonitorRect)
{
	return SPosSize
	{
		.Position = { (int32)MonitorRect.Left, (int32)MonitorRect.Right },
		.Size = { (int32)(MonitorRect.Right - MonitorRect.Left), (int32)(MonitorRect.Bottom - MonitorRect.Top) }
	};
}

void Window::UpdateTitle(const std::wstring& NewTitleDetails) const
{
	std::wstring newTitle = _title + NewTitleDetails;
	SetWindowText(_hWindow, newTitle.c_str());
}

void Window::RegisterWinAPIClass()
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style          = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wcex.lpfnWndProc    = SetupMessageProcessing;
	wcex.cbClsExtra     = 0;
	wcex.cbWndExtra     = 0;
	wcex.hInstance      = _params.hInst;
	wcex.hIcon          = nullptr;//LoadIcon(hInstance, MAKEINTRESOURCE(IDI_DEMO));
	wcex.hCursor        = nullptr;//LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground  = nullptr;//(HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName   = nullptr;//MAKEINTRESOURCEW(IDC_DEMO);
	wcex.lpszClassName  = _params.className.c_str();
	wcex.hIconSm        = nullptr;//LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	RegisterClassEx(&wcex);
}

LRESULT Window::SetupMessageProcessing(HWND hWindow, UINT message, WPARAM wParam, LPARAM lParam)
{
	// use create parameter passed in from CreateWindow() to store window class pointer at WinAPI side
	if (message == WM_NCCREATE)
	{
		// extract ptr to window class from creation data
		const CREATESTRUCTW* const pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
		Window* const pWnd = static_cast<Window*>(pCreate->lpCreateParams);
		// set WinAPI-managed user data to store ptr to window instance
		SetWindowLongPtr(hWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWnd));
		// set message proc to normal (non-setup) handler now that setup is finished
		SetWindowLongPtr(hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::HandleMessageProcessing));
		// forward message to window instance handler
		return pWnd->WindowProcessMessage(hWindow, message, wParam, lParam);
	}
	// if we get a message before the WM_NCCREATE message, handle with default handler
	return DefWindowProc(hWindow, message, wParam, lParam);
}

LRESULT Window::HandleMessageProcessing(HWND hWindow, UINT message, WPARAM wParam, LPARAM lParam)
{
	// retrieve ptr to window instance
	Window* const pWnd = reinterpret_cast<Window*>(GetWindowLongPtr(hWindow, GWLP_USERDATA));
	// forward message to window instance handler
	return pWnd->WindowProcessMessage(hWindow, message, wParam, lParam);
}

LRESULT CALLBACK Window::WindowProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
	{
		return true;
	}

	switch (message)
	{
		// WM_ACTIVATE is sent when the window is activated or deactivated.  
		// We pause the game when the window is deactivated and unpause it 
		// when it becomes active.  
		case WM_ACTIVATE:
			{
				if (LOWORD(wParam) == WA_INACTIVE)
				{
					// TODO:
					// mAppPaused = true;
					// mTimer.Stop();
					if (DisplaySettings.IsFullscreen())
					{
						ShowWindow(_hWindow, SW_MINIMIZE);
					}
					PostLoseFocusEvent.Invoke();
				}
				else
				{
					// TODO:
					// mAppPaused = false;
					// mTimer.Start();
					ShowWindow(_hWindow, SW_RESTORE);
					PostGainFocusEvent.Invoke();
				}
				return 0;
			}

		// WM_SIZE is sent when the user resizes the window.
		case WM_SIZE:
			if (bIgnoreNextSizeEvent)
			{
				return 0;
			}

			// Save the new client area dimensions.
			_params.width  = LOWORD(lParam);
			_params.height = HIWORD(lParam);
			if( wParam == SIZE_MINIMIZED )
			{
				bMinimized = true;
				// mMaximized = false;
				DisplaySettings.FullscreenMode = EFullscreenMode::Minimized;
				PostMinimizeEvent.Invoke();
				return 0;
			}
			else if( wParam == SIZE_MAXIMIZED )
			{
				// mMaximized = true;
				PostResizeEvent.Invoke();
			}
			else if( wParam == SIZE_RESTORED )
			{
				// Restoring from minimized state?
				if (bMinimized)
				{
					bMinimized = false;
					PostRestoreFromMinimizeEvent.Invoke();
					return 0;
				}

				/*
				// Restoring from maximized state?
				else if( mMaximized )
				{
					mAppPaused = false;
					mMaximized = false;
					OnResize();
				}
				else if( mResizing )
				{
					// If user is dragging the resize bars, we do not resize 
					// the buffers here because as the user continuously 
					// drags the resize bars, a stream of WM_SIZE messages are
					// sent to the window, and it would be pointless (and slow)
					// to resize for each WM_SIZE message received from dragging
					// the resize bars.  So instead, we reset after the user is 
					// done resizing the window and releases the resize bars, which 
					// sends a WM_EXITSIZEMOVE message.
				}
				else // API call such as SetWindowPos or mSwapChain->SetFullscreenState.
				*/
				{
					PostResizeEvent.Invoke();
				}
			}
			return 0;

		// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
		/* TODO:
		case WM_ENTERSIZEMOVE:
			mAppPaused = true;
			mResizing  = true;
			mTimer.Stop();
			return 0;
		*/

		// WM_EXITSIZEMOVE is sent when the user releases the resize bars.
		// Here we reset everything based on the new window dimensions.
		/* TODO:
		case WM_EXITSIZEMOVE:
			mAppPaused = false;
			mResizing  = false;
			mTimer.Start();
			OnResize();
			return 0;
		*/

		// WM_DESTROY is sent when the window is being destroyed.
		/* TODO:
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		*/

		// The WM_MENUCHAR message is sent when a menu is active and the user presses 
		// a key that does not correspond to any mnemonic or accelerator key. 
		case WM_MENUCHAR:
			// Don't beep when we alt-enter.
			return MAKELRESULT(0, MNC_CLOSE);

		// Catch this message so to prevent the window from becoming too small.
		case WM_GETMINMAXINFO:
			((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
			((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200; 
			return 0;

		case WM_COMMAND:
		{
			int wmId = LOWORD(wParam);
			// Parse the menu selections:
			switch (wmId)
			{
				//case IDM_ABOUT:
				//    DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
				//    break;
			// case IDM_EXIT:
			// 	DestroyWindow(hWnd);
			// 	break;
			default:
				return DefWindowProc(hWnd, message, wParam, lParam);
			}
		}
		break;
	// case WM_PAINT:
	// 	{
	// 		PAINTSTRUCT ps;
	// 		HDC hdc = BeginPaint(hWnd, &ps);
	// 		// TODO: Add any drawing code that uses hdc here...
	// 		EndPaint(hWnd, &ps);
	// 	}
	// 	break;
	//TODO: case WM_DISPLAYCHANGE :
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

NAMESPACE_FRT_END
