#include "Window.h"

#include <iostream>

#include "CoreUtils.h"
#include "Math/Math.h"

#include "imgui.h"
#include "Graphics/RenderCommonTypes.h"

NAMESPACE_FRT_START
CWindow::~CWindow ()
{}

CWindow::CWindow (const SWindowParams& Params)
	: Params(Params)
{
	RegisterWinAPIClass();

	const int right = Params.StartX + Params.Width;
	const int bottom = Params.StartY + Params.Height;
	RECT wr{ .left = Params.StartX, .top = Params.StartY, .right = right, .bottom = bottom };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, false);

	Title = L"FRTEngine";
	hWindow = CreateWindow(
		Params.ClassName.c_str(), Title.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		Params.Width, Params.Height,
		nullptr /*hWndParent*/, nullptr /*hMenu*/, Params.hInst,
		this /*lpParam*/);

	if (!hWindow) // TODO: move this if outside
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
	}

	DisplaySettings = SDisplaySettings
	{
		.MonitorIndex = -1,
		.ResolutionIndex = -1,
		.RefreshRateIndex = -1
	};

	ShowWindow(hWindow, SW_SHOWDEFAULT);
	UpdateWindow(hWindow);
}

HWND CWindow::GetHandle () const
{
	return hWindow;
}

Vector2f CWindow::GetWindowSize () const
{
	return { static_cast<float>(Params.Width), static_cast<float>(Params.Height) };
}

void CWindow::SetDisplaySettings (const SDisplaySettings& NewSettings, const graphics::SDisplayOptions& Options)
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
void SetResolution (int width, int height, int refreshRate = 60)
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

void CWindow::ResizeWithMove (const Vector2u& NewSize, const graphics::SRect& MonitorRect) const
{
	const auto [pos, size] = CalcPositionAndSize(NewSize, MonitorRect);
	MoveWindow(hWindow, pos.x, pos.y, size.x, size.y, true);
}

void CWindow::ResizeWithSetPos (const Vector2u& NewSize, const graphics::SRect& MonitorRect) const
{
	const auto [pos, size] = CalcPositionAndSize(NewSize, MonitorRect);
	SetWindowPos(hWindow, HWND_TOP, pos.x, pos.y, size.x, size.y, SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void CWindow::ResizeWithSetPos (const graphics::SRect& NewRect) const
{
	const auto [pos, size] = CalcPositionAndSize(NewRect);
	SetWindowPos(hWindow, HWND_TOP, pos.x, pos.y, size.x, size.y, SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void CWindow::UpdateFullscreenMode (EFullscreenMode NewFullscreenMode, const graphics::SRect& MonitorRect)
{
	if (NewFullscreenMode == EFullscreenMode::Fullscreen)
	{
		{
			// using guard doesn't look right...
			TGuardValue GuardIgnoreNextSizeEvent(bIgnoreNextSizeEvent, true);
			SetWindowLong(hWindow, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		}
		ResizeWithSetPos(MonitorRect);
		ShowWindow(hWindow, SW_MAXIMIZE);
	}
	else if (NewFullscreenMode == EFullscreenMode::Borderless)
	{
		{
			TGuardValue GuardIgnoreNextSizeEvent(bIgnoreNextSizeEvent, true);
			SetWindowLong(hWindow, GWL_STYLE, WS_POPUP);
		}
		ResizeWithSetPos(MonitorRect);
		ShowWindow(hWindow, SW_MAXIMIZE);
	}
	else
	{
		{
			TGuardValue GuardIgnoreNextSizeEvent(bIgnoreNextSizeEvent, true);
			SetWindowLong(hWindow, GWL_STYLE, WS_OVERLAPPEDWINDOW);
		}
		ResizeWithSetPos(MonitorRect);
		ShowWindow(hWindow, SW_NORMAL);
	}
}

CWindow::SPosSize CWindow::CalcPositionAndSize (const Vector2u& NewSize, const graphics::SRect& MonitorRect)
{
	const int32 newX = (int32)(MonitorRect.Right - MonitorRect.Left) / 2 - NewSize.x / 2 + (int32)MonitorRect.Left;
	const int32 newY = (int32)(MonitorRect.Bottom - MonitorRect.Top) / 2 - NewSize.y / 2 + (int32)MonitorRect.Top;
	return SPosSize
	{
		.Position = { newX, newY },
		.Size = { (int32)NewSize.x, (int32)NewSize.y }
	};
}

CWindow::SPosSize CWindow::CalcPositionAndSize (const graphics::SRect& MonitorRect)
{
	return SPosSize
	{
		.Position = { (int32)MonitorRect.Left, (int32)MonitorRect.Right },
		.Size = { (int32)(MonitorRect.Right - MonitorRect.Left), (int32)(MonitorRect.Bottom - MonitorRect.Top) }
	};
}

void CWindow::UpdateTitle (const std::wstring& NewTitleDetails) const
{
	std::wstring newTitle = Title + NewTitleDetails;
	SetWindowText(hWindow, newTitle.c_str());
}

void CWindow::RegisterWinAPIClass ()
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wcex.lpfnWndProc = SetupMessageProcessing;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = Params.hInst;
	wcex.hIcon = nullptr;         //LoadIcon(hInstance, MAKEINTRESOURCE(IDI_DEMO));
	wcex.hCursor = nullptr;       //LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = nullptr; //(HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName = nullptr;  //MAKEINTRESOURCEW(IDC_DEMO);
	wcex.lpszClassName = Params.ClassName.c_str();
	wcex.hIconSm = nullptr; //LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	RegisterClassEx(&wcex);
}

LRESULT CWindow::SetupMessageProcessing (HWND hWindow, UINT message, WPARAM wParam, LPARAM lParam)
{
	// use create parameter passed in from CreateWindow() to store window class pointer at WinAPI side
	if (message == WM_NCCREATE)
	{
		// extract ptr to window class from creation data
		const CREATESTRUCTW* const pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
		const auto pWnd = static_cast<CWindow*>(pCreate->lpCreateParams);
		// set WinAPI-managed user data to store ptr to window instance
		SetWindowLongPtr(hWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWnd));
		// set message proc to normal (non-setup) handler now that setup is finished
		SetWindowLongPtr(hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CWindow::HandleMessageProcessing));
		// forward message to window instance handler
		return pWnd->WindowProcessMessage(hWindow, message, wParam, lParam);
	}
	// if we get a message before the WM_NCCREATE message, handle with default handler
	return DefWindowProc(hWindow, message, wParam, lParam);
}

LRESULT CWindow::HandleMessageProcessing (HWND hWindow, UINT message, WPARAM wParam, LPARAM lParam)
{
	// retrieve ptr to window instance
	const auto pWnd = reinterpret_cast<CWindow*>(GetWindowLongPtr(hWindow, GWLP_USERDATA));
	// forward message to window instance handler
	return pWnd->WindowProcessMessage(hWindow, message, wParam, lParam);
}

LRESULT CALLBACK CWindow::WindowProcessMessage (HWND hWnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
	extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler (HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	if (ImGui_ImplWin32_WndProcHandler(hWnd, Message, wParam, lParam))
	{
		return true;
	}

	switch (Message)
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
					ShowWindow(hWindow, SW_MINIMIZE);
				}
				PostLoseFocusEvent.Invoke();
			}
			else
			{
				// TODO:
				// mAppPaused = false;
				// mTimer.Start();
				ShowWindow(hWindow, SW_RESTORE);
				PostGainFocusEvent.Invoke();
			}
			return 0;
		}

		// WM_SIZE is sent when the user resizes the window.
		case WM_SIZE: if (bIgnoreNextSizeEvent)
			{
				return 0;
			}

			// Save the new client area dimensions.
			Params.Width = LOWORD(lParam);
			Params.Height = HIWORD(lParam);
			if (wParam == SIZE_MINIMIZED)
			{
				bMinimized = true;
				// mMaximized = false;
				DisplaySettings.FullscreenMode = EFullscreenMode::Minimized;
				PostMinimizeEvent.Invoke();
				return 0;
			}
			else if (wParam == SIZE_MAXIMIZED)
			{
				// mMaximized = true;
				PostResizeEvent.Invoke();
			}
			else if (wParam == SIZE_RESTORED)
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
		case WM_GETMINMAXINFO: ((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
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
				default: return DefWindowProc(hWnd, Message, wParam, lParam);
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
		case WM_DESTROY: PostQuitMessage(0);
			break;
		default: return DefWindowProc(hWnd, Message, wParam, lParam);
	}
	return 0;
}

NAMESPACE_FRT_END
