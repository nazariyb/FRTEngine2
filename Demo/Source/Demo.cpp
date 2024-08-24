// Demo.cpp : Defines the entry point for the application.
//

#include "..\framework.h"
#include "..\Demo.h"
#include "Core.h"
#include "DemoGame.h"
#include "Timer.h"

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

	// Initialize global strings
	// LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	// LoadStringW(hInstance, IDC_DEMO, szWindowClass, MAX_LOADSTRING);

	MSG msg{};

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
			game->Tick(time.GetDeltaSeconds());
			game->Draw(time.GetDeltaSeconds());
		}
	}

	return static_cast<int>(msg.wParam);
}
