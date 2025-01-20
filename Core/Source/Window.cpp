#include "Window.h"

#include "Math/Math.h"

#include "imgui.h"

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
	_hWnd = CreateWindow(_params.className.c_str(), _title.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		_params.width, _params.height,
		nullptr /*hWndParent*/, nullptr /*hMenu*/, _params.hInst,
		nullptr /*lpParam*/);

	if (!_hWnd) // TODO: move this if outside
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
	}

	ShowWindow(_hWnd, SW_SHOWDEFAULT);
	UpdateWindow(_hWnd);
}

HWND Window::GetHandle() const
{
	return _hWnd;
}

Vector2f Window::GetWindowSize() const
{
	return { static_cast<float>(_params.width), static_cast<float>(_params.height) };
}

void Window::UpdateTitle(const std::wstring& NewTitleDetails) const
{
	std::wstring newTitle = _title + NewTitleDetails;
	SetWindowText(_hWnd, newTitle.c_str());
}

void Window::RegisterWinAPIClass()
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style          = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wcex.lpfnWndProc    = WindowProcessMessage;
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
				}
				else
				{
					// TODO:
					// mAppPaused = false;
					// mTimer.Start();
				}
				return 0;
			}

		// WM_SIZE is sent when the user resizes the window.
		/* TODO:
		case WM_SIZE:
			// Save the new client area dimensions.
			mClientWidth  = LOWORD(lParam);
			mClientHeight = HIWORD(lParam);
			if( md3dDevice )
			{
				if( wParam == SIZE_MINIMIZED )
				{
					mAppPaused = true;
					mMinimized = true;
					mMaximized = false;
				}
				else if( wParam == SIZE_MAXIMIZED )
				{
					mAppPaused = false;
					mMinimized = false;
					mMaximized = true;
					OnResize();
				}
				else if( wParam == SIZE_RESTORED )
				{
					
					// Restoring from minimized state?
					if( mMinimized )
					{
						mAppPaused = false;
						mMinimized = false;
						OnResize();
					}

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
					{
						OnResize();
					}
				}
			}
			return 0;*/

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
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

NAMESPACE_FRT_END
