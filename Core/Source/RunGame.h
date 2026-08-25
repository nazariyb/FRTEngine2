#pragma once

#include <chrono>
#include <iostream>

#include "CoreTypes.h"
#include "Exception.h"
#include "GameInstance.h"
#include "Precision.h"
#include "Timer.h"
#include "ECS/CoreComponents.h"
#include "Graphics/GraphicsComponents.h"


namespace frt
{
template <typename TGame> requires concepts::Derived<TGame, GameInstance>
int RunGame()
{
	// This template is instantiated in the host module, so the macro expands to the
	// host's value and the check compares it against Core's.
	FRT_VERIFY_REAL_PRECISION_ABI();

	// Before the game exists, and that ordering is the whole point: the first
	// GetComponentId<T>() for a type fixes its id permanently, and constructing a CWorld
	// touches the registry. Called any later, this is silently a no-op and ids fall back
	// to whatever order first touch happened to give them.
	RegisterCoreComponents();
	graphics::RegisterGraphicsComponents();

	auto game = new TGame;

	CTimer& time = game->GetTime();
	time.Reset();

	game->Load();
	game->OnLoaded();

	// Initialize global strings
	// LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	// LoadStringW(hInstance, IDC_DEMO, szWindowClass, MAX_LOADSTRING);

	MSG msg{};

	try
	{
#if !defined(FRT_HEADLESS)
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
				// if (!time.IsPaused())
				{
					game->Input(time.GetDeltaSeconds());
					game->Tick(time.GetDeltaSeconds());
					game->Draw(time.GetDeltaSeconds());
				}
			}
		}
#else
		while (true)
		{
			time.Tick();
			if (!time.IsPaused())
			{
				game->Tick(time.GetDeltaSeconds());
			}
		}
#endif
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


namespace platform
{
#if defined(_WINDOWS)

#if !defined(FRT_HEADLESS)
#define FRT_RUN_GAME(TGame)\
		int APIENTRY wWinMain(_In_ HINSTANCE hInstance,\
			_In_opt_ HINSTANCE hPrevInstance,\
			_In_ LPWSTR    lpCmdLine,\
			_In_ int       nCmdShow)\
		{\
			UNREFERENCED_PARAMETER(hPrevInstance);\
			UNREFERENCED_PARAMETER(lpCmdLine);\
			return RunGame<TGame>();\
		}
#else
#define FRT_RUN_GAME(TGame)\
	int main()\
	{\
		return RunGame<TGame>();\
	}
#endif

#elif defined(_LINUX)
#error "Linux platform is not supported yet"
#else
#error "Unsupported platform"
#endif
}
}
