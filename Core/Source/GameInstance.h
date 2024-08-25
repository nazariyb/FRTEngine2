#pragma once

#include <memory>

#include "Core.h"
#include "Singleton.h"


namespace frt::graphics
{
	class Graphics;
}

namespace frt
{
	class Timer;
	class Window;
}

NAMESPACE_FRT_START

class FRT_CORE_API GameInstance : public Singleton<GameInstance>
{
public:
	GameInstance();
	virtual ~GameInstance();

	FRT_SINGLETON_GETTERS(GameInstance)

	Timer& GetTime() const;

	// Update
	// virtual void Input(float DeltaSeconds);
	virtual void Tick(float DeltaSeconds);
	virtual void Draw(float DeltaSeconds);
	// ~Update

	long long GetFrameCount() const;

protected:
	void CalculateFrameStats() const;

protected:
	Timer* _timer;
	Window* _window;
	graphics::Graphics* _graphics;

	long long _frameCount;
};

NAMESPACE_FRT_END
