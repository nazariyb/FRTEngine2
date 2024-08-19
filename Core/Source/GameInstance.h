#pragma once

#include <memory>

#include "Core.h"
#include "Singleton.h"


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

	virtual void Tick(float DeltaSeconds);

	long long GetFrameCount() const;

protected:
	void CalculateFrameStats() const;

protected:
	Timer* _timer;
	Window* _window;

	long long _frameCount;
};

NAMESPACE_FRT_END
