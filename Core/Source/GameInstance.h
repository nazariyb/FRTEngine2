#pragma once

#include <memory>

#include "Core.h"
#include "Singleton.h"
#include "World.h"
#include "Graphics/RenderCommonTypes.h"
#include "Memory/Memory.h"
#include "User/UserSettings.h"


namespace frt::graphics
{
	class CCamera;
}

namespace frt::graphics
{
	class Renderer;
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

	bool HasGraphics() const;
	graphics::Renderer& GetGraphics() const;

	virtual void Load();

	// Update
	// virtual void Input(float DeltaSeconds);
	virtual void Tick(float DeltaSeconds);
	virtual void Draw(float DeltaSeconds);
	// ~Update

	long long GetFrameCount() const;

protected:
	void CalculateFrameStats() const;

	virtual void DisplayUserSettings();

protected:
	Timer* _timer;
	Window* _window;
	graphics::Renderer* _renderer;

	memory::TMemoryHandle<CWorld> World;

	memory::TMemoryHandle<graphics::CCamera> Camera;

	graphics::SDisplayOptions DisplayOptions;
	SUserSettings UserSettings;

	long long _frameCount;
};

NAMESPACE_FRT_END
