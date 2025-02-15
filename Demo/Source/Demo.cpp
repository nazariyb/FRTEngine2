// Demo.cpp : Defines the entry point for the application.
//

#include "..\framework.h"
#include "..\Demo.h"

#include <iostream>

#include "DemoGame.h"
#include "Exception.h"
#include "Timer.h"
#include "Graphics/Renderer.h"

using namespace frt;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
					_In_opt_ HINSTANCE hPrevInstance,
					_In_ LPWSTR    lpCmdLine,
					_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	auto game = new DemoGame();

	Timer& time = game->GetTime();
	time.Reset();

	// game->Load();

	// Initialize global strings
	// LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	// LoadStringW(hInstance, IDC_DEMO, szWindowClass, MAX_LOADSTRING);

	MSG msg{};

	try
	{
		while (msg.message != WM_QUIT)
		{
			if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			else
			{
				time.Tick();
				if (!time.IsPaused())
				{
					game->Tick(time.GetDeltaSeconds());
					game->Draw(time.GetDeltaSeconds());
				}
			}
		}
	}
	catch (const Exception& e)
	{
		std::cout << e.What() << " " << e.GetErrorCode() << " : " << e.GetErrorDescription() << std::endl;
		// MessageBox(nullptr, e.What(), e.GetType(), MB_OK | MB_ICONEXCLAMATION);
	}
	catch (const std::exception& e)
	{
		std::cout << e.what() << std::endl;
		// MessageBox(nullptr, e.what(), L"Standard Exception", MB_OK | MB_ICONEXCLAMATION);
	}
	catch (...)
	{
		MessageBox(nullptr, L"No details available", L"Unknown Exception", MB_OK | MB_ICONEXCLAMATION);
	}

	return static_cast<int>(msg.wParam);
}
