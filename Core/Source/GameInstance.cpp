#include "GameInstance.h"

#include "Window.h"

NAMESPACE_FRT_START

GameInstance* Singleton<GameInstance>::_instance(nullptr);

GameInstance::GameInstance()
{
	WindowParams windowParams;
	windowParams.startX = 1000;
	windowParams.startY = 1000;
	windowParams.width = 800;
	windowParams.height = 600;
	windowParams.className = L"FrtWindowClass";
	windowParams.hInst = GetModuleHandle(nullptr);
	_window = std::make_unique<Window>(windowParams);
}

GameInstance::~GameInstance()
{
}

NAMESPACE_FRT_END
