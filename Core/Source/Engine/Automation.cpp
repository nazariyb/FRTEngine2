#include "Engine/Automation.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cstdio>

#include "EnginePaths.h"
#include "WorldScene.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "Engine/LaunchConfig.h"

#if !defined(FRT_HEADLESS)
#include "Graphics/Render/Renderer.h"
#include "Graphics/Render/Scene/GpuScene.h"
#endif


namespace frt
{
void CAutomation::Configure (CEngine& Engine, const CWorldScene& World)
{
	const config::CConfig& Config = Engine.GetConfig();
	CaptureFrame = Config.Get("Automation", "CaptureFrame", -1);
	CaptureTimeoutFrames = Config.Get("Automation", "CaptureTimeoutFrames", 600);
	ExitFrame = Config.Get("Automation", "ExitFrame", -1);
	ProfileFrames = Config.Get("Automation", "ProfileFrames", 0);
	bMotion = ParseVector3(Config.Get("Automation", "CameraMove", ""), CameraMove);
	bMotion = ParseVector3(Config.Get("Automation", "CameraTurn", ""), CameraTurn) || bMotion;
	MotionStartFrame = Config.Get("Automation", "MotionStartFrame", 0);
	MotionEndFrame = Config.Get("Automation", "MotionEndFrame", -1);
	TimeOfDayFrame = Config.Get("Automation", "TimeOfDayFrame", -1);
	TimeOfDayValue = Config.Get("Automation", "TimeOfDayValue", World.Environment.TimeOfDay);
	bCaptureHdr = Config.Get("Automation", "CaptureHdr", true);
	const FString capturePath = Config.Get("Automation", "CapturePath", "");
	CapturePath = capturePath.IsEmpty() ? (paths::GetCaptureDir() / "automation") : capturePath.ToPath();
	bActive = CaptureFrame >= 0 || ExitFrame >= 0 || bMotion || TimeOfDayFrame >= 0;

#if !defined(FRT_HEADLESS)
	if (graphics::CRenderer* renderer = Engine.GetRenderer())
	{
		renderer->GetScene().GetTextures().SetDeterministic(bActive && Config.Get("Automation", "DeterministicStreaming", true));
	}
#endif
}

void CAutomation::Update (CEngine& Engine, CWorldScene& World, graphics::CRenderView* View, float RealDeltaSeconds)
{
	const int64 frame = static_cast<int64>(Engine.GetFrameIndex());
	if (frame == TimeOfDayFrame)
	{
		// Picked up by the sky panel (ComputeSkyFromTimeOfDay) on the next UI frame.
		World.Environment.TimeOfDay = TimeOfDayValue;
	}
	if (bMotion && frame >= MotionStartFrame && (MotionEndFrame < 0 || frame < MotionEndFrame))
	{
		using namespace DirectX;
		const Vector3f rot = World.Camera.Transform.GetRotation();
		const XMMATRIX rotM = XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z);
		const XMVECTOR local = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&CameraMove));
		Vector3f worldMove{};
		XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&worldMove), XMVector3TransformNormal(local, rotM));
		World.Camera.Transform.MoveBy(worldMove);
		World.Camera.Transform.SetRotation(rot + CameraTurn);
	}

#if !defined(FRT_HEADLESS)
	graphics::CRenderer* renderer = Engine.GetRenderer();
	if (View && renderer && CaptureFrame >= 0 && !bCaptureRequested && frame >= CaptureFrame)
	{
		// Textures still streaming would be missing from the image: the capture waits for them.
		if (renderer->GetScene().IsStreamingIdle())
		{
			View->RequestCapture(CapturePath, bCaptureHdr);
			bCaptureRequested = true;
			const Vector3f camPos = World.Camera.Transform.GetTranslation();
			const Vector3f camRot = World.Camera.Transform.GetRotation();
			std::printf("[automation] capture requested at frame %llu (%u spp accumulated) camera %.6f,%.6f,%.6f rotation %.6f,%.6f,%.6f\n",
				static_cast<unsigned long long>(frame), View->GetAccumulatedSamples(),
				camPos.x, camPos.y, camPos.z, camRot.x, camRot.y, camRot.z);
			std::fflush(stdout);
		}
		else if (frame >= CaptureFrame + CaptureTimeoutFrames)
		{
			std::fprintf(stderr, "[automation] error: textures still streaming at frame %llu, %lld frames past CaptureFrame; no capture written\n",
				static_cast<unsigned long long>(frame), static_cast<long long>(CaptureTimeoutFrames));
			std::fflush(stderr);
			bCaptureRequested = true;
			ExitFrame = -1;
			Engine.RequestQuit(1);
		}
	}
	if (View && renderer && ProfileFrames > 0 && ExitFrame >= 0 && frame >= ExitFrame - ProfileFrames && frame < ExitFrame)
	{
		for (const profiler::SGpuScopeResult& r : renderer->GetGpuProfiler().GetLastResults())
		{
			// This view's passes and the frame-wide scopes only.
			if (r.Name.IsEmpty() || (!r.View.IsEmpty() && r.View != View->GetName()))
			{
				continue;
			}
			auto it = std::find_if(Profile.begin(), Profile.end(), [&] (const auto& e) { return e.first == r.Name; });
			if (it == Profile.end())
			{
				Profile.push_back({ FString(r.Name), { 0.0, 0u } });
				it = Profile.end() - 1;
			}
			it->second.first += r.DurationMs;
			++it->second.second;
		}
		CpuFrameMs += static_cast<double>(RealDeltaSeconds) * 1000.0;
		++CpuFrames;
	}
	const bool bCaptureWaiting = View && CaptureFrame >= 0 && !bCaptureRequested && frame >= CaptureFrame;
	const bool bCapturesPending = bCaptureWaiting || (renderer && renderer->HasPendingCaptures());
#else
	(void)View;
	(void)RealDeltaSeconds;
	const bool bCapturesPending = false;
#endif

	if (ExitFrame >= 0 && frame >= ExitFrame && !bCapturesPending)
	{
		for (const auto& e : Profile)
		{
			std::printf("[profile] %-28s %8.3f ms\n", e.first.CStr(), e.second.second ? e.second.first / e.second.second : 0.0);
		}
		if (CpuFrames > 0u)
		{
			std::printf("[profile] %-28s %8.3f ms\n", "CPU frame", CpuFrameMs / CpuFrames);
		}
		std::printf("[automation] exit at frame %llu\n", static_cast<unsigned long long>(frame));
		std::fflush(stdout);
		ExitFrame = -1;
		Engine.RequestQuit(0);
	}
}
}
