#include "GameInstance.h"

#include "Timer.h"
#include "Window.h"

NAMESPACE_FRT_START

FRT_SINGLETON_DEFINE_INSTANCE(GameInstance)

GameInstance::GameInstance()
	: _frameCount(0)
{
	_timer = new Timer;

	WindowParams windowParams;
	windowParams.startX = 1000;
	windowParams.startY = 1000;
	windowParams.width = 800;
	windowParams.height = 600;
	windowParams.className = L"FrtWindowClass";
	windowParams.hInst = GetModuleHandle(nullptr);
	_window = new Window(windowParams);
}

GameInstance::~GameInstance()
{
	delete _timer;
	delete _window;

	_timer = nullptr;
	_window = nullptr;
}

Timer& GameInstance::GetTime() const
{
	return *_timer;
}

void GameInstance::Tick(float DeltaSeconds)
{
	++_frameCount;
	CalculateFrameStats();
}

long long GameInstance::GetFrameCount() const
{
	return _frameCount;
}

void GameInstance::CalculateFrameStats() const
{
	static int frameCount = 0;
	static float timeElapsed = 0.f;

	++frameCount;

	if (_timer->GetTotalSeconds() - timeElapsed >= 1.f)
	{
		float fps = static_cast<float>(frameCount);
		float msPerFrame = 1000.f / fps;

		std::wstring fpsStr = std::to_wstring(fps);
		std::wstring msPerFrameStr = std::to_wstring(msPerFrame);

		std::wstring windowText =
			L"         FPS: " + fpsStr +
			L"    ms/frame: " + msPerFrameStr;
		_window->UpdateTitle(windowText);

		frameCount = 0;
		timeElapsed += 1.f;
	}
}

NAMESPACE_FRT_END
