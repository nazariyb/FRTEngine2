#pragma once

#include "Core.h"
#include "Singleton.h"
#include "Window.h"
#include "Sys_MeshRenderer.h"
#include "WorldScene.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "Graphics/Render/RenderResourceAllocators.h"
#include "Input/InputActionLibrary.h"
#include "Input/InputSystem.h"
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
#if !defined(FRT_HEADLESS)
	memory::TRefWeak<graphics::CRenderer> GetRenderer () const;

	// temp
	CWindow& GetWindow () const { return *Window; }
	memory::TRefWeak<graphics::CCamera> GetCamera () const { return Camera.GetWeak(); }
	// ~temp
#endif
	input::CInputSystem& GetInputSystem () { return InputSystem; }
	const input::CInputSystem& GetInputSystem () const { return InputSystem; }
	input::CInputActionLibrary& GetInputActionLibrary () { return InputActionLibrary; }
	const input::CInputActionLibrary& GetInputActionLibrary () const { return InputActionLibrary; }
	input::CInputActionMap* GetActiveInputActionMap ();
	const input::CInputActionMap* GetActiveInputActionMap () const;

	virtual void Load ();

	// Update
	virtual void Input (float DeltaSeconds);
	virtual void Tick (float DeltaSeconds);
#if !defined(FRT_HEADLESS)
	virtual void Draw (float DeltaSeconds);
#endif
	// ~Update

	uint64 GetFrameCount () const;

	CWorldScene& GetWorldScene () { return World; }
	const CWorldScene& GetWorldScene () const { return World; }

protected:
	void CalculateFrameStats () const;

#ifndef FRT_HEADLESS
	virtual void OnWindowResize ();
	virtual void OnLoseFocus ();
	virtual void OnGainFocus ();
	virtual void OnMinimize ();
	virtual void OnRestoreFromMinimize ();

	virtual void DisplayUserSettings ();
#endif

protected:
	memory::CMemoryPool MemoryPool;
	CTimer* Timer;
#ifndef FRT_HEADLESS
	CWindow* Window;
	memory::TRefUnique<graphics::CRenderer> Renderer;
	memory::TRefShared<graphics::CCamera> Camera;

	// Dedicated descriptor heap for ImGui (font + a few spares).
	// Kept separate from Renderer's ShaderDescriptorHeap so engine-side heap rebuilds
	// on material/texture growth never invalidate ImGui's font slot or trash its state.
	graphics::DX12_DescriptorHeap ImGuiDescriptorHeap;
#endif
	CWorldScene World;
	memory::TRefWeak<Sys_MeshRenderer> MeshRenderer;
	input::CInputSystem InputSystem;
	input::CInputActionLibrary InputActionLibrary;
	input::SInputActionMapAsset* ActiveActionMap = nullptr;

	// temp
	memory::TRefShared<CEntity> Cube;
	memory::TRefShared<CEntity> Cylinder;
	memory::TRefShared<CEntity> Sphere;
	void UpdateEntities (float DeltaSeconds);
	// ~temp

	graphics::SDisplayOptions DisplayOptions;
	SUserSettings UserSettings;

	// Live raytracing knobs — written by ImGui sliders, read by Sys_MeshRenderer when
	// filling SPassConstants. Tweak at runtime without shader recompile.
	struct SRtSettings
	{
		uint32 SampleCount = 32u;
		uint32 MaxBounces = 4u;
		uint32 RussianRouletteDepth = 2u;
	};
public:
	SRtSettings& GetRtSettings () { return RtSettings; }
	const SRtSettings& GetRtSettings () const { return RtSettings; }
protected:
	SRtSettings RtSettings;

	uint64 FrameCount;

	bool bCameraMovementEnabled = false;
};


NAMESPACE_FRT_END
