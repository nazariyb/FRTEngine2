#pragma once

#include "Core.h"
#include "Singleton.h"
#include "Window.h"
#include "World.h"
#include "Graphics/RenderCommonTypes.h"
#include "Memory/MemoryPool.h"
#include "Memory/Ref.h"
#include "User/UserSettings.h"


namespace frt::graphics
{
class CCamera;
}


namespace frt::graphics
{
class CRenderer;
}


namespace frt
{
class CTimer;
class CWindow;
}


NAMESPACE_FRT_START
class FRT_CORE_API GameInstance : public Singleton<GameInstance>
{
public:
	GameInstance ();
	virtual ~GameInstance ();

	FRT_SINGLETON_GETTERS(GameInstance)

	CTimer& GetTime () const;

	bool HasGraphics () const;
	memory::TRefWeak<graphics::CRenderer> GetGraphics () const;

	// temp
	CWindow& GetWindow () const { return *Window; }
	memory::TRefWeak<graphics::CCamera> GetCamera () const { return Camera.GetWeak(); }
	// ~temp

	virtual void Load ();

	// Update
	// virtual void Input(float DeltaSeconds);
	virtual void Tick (float DeltaSeconds);
	virtual void Draw (float DeltaSeconds);
	// ~Update

	long long GetFrameCount () const;

protected:
	void CalculateFrameStats () const;

	virtual void OnWindowResize ();
	virtual void OnLoseFocus ();
	virtual void OnGainFocus ();
	virtual void OnMinimize ();
	virtual void OnRestoreFromMinimize ();

	virtual void DisplayUserSettings ();

protected:
	memory::CMemoryPool MemoryPool;
	CTimer* Timer;
	CWindow* Window;
	memory::TRefUnique<graphics::CRenderer> Renderer;

	memory::TRefUnique<CWorld> World;

	memory::TRefShared<graphics::CCamera> Camera;

	graphics::SDisplayOptions DisplayOptions;
	SUserSettings UserSettings;

	long long FrameCount;

	bool bLoaded = false; // temp
};


NAMESPACE_FRT_END
