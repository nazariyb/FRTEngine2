#pragma once

#include <filesystem>
#include <string>

#include "CoreTypes.h"
#include "Math/Math.h"


namespace frt
{
class CEngine;
}

namespace frt::config
{
class CConfig;
}


namespace frt::editor
{
class CEditor;

/**
 * A scripted editor session (-Editor:SmokeTest=1): drives the editor through the same commands
 * the user issues - maximize, fullscreen, play, pause, step, stop - checks the state after each
 * (Stop must leave the edit world exactly as Play found it), presses bound keys through the
 * window's own message queue, rewrites its input overrides to check they are re-read, checks
 * the Unreal camera controller against Unreal's numbers, makes the game module's DLL look
 * rebuilt in edit and in play mode to check the editor reloads it,
 * captures the whole window to PNG along the way, and exits with 0 when every check passed (2
 * otherwise). Captures and a log go to [Editor] SmokeTestDir, by default <project>/Saved/SmokeTest.
 */
class CEditorSmokeTest
{
public:
	explicit CEditorSmokeTest (const config::CConfig& Config);

	// Every frame, after the editor applied its commands.
	void Update (CEditor& Editor, CEngine& Engine);

	// Where captures, the log and the run's own input overrides go.
	const std::filesystem::path& GetOutputDir () const { return OutputDir; }

private:
	void Check (bool bPassed, const char* What);
	void CheckCameraController ();
	void TouchGameBuild (CEditor& Editor);
	void WriteInputOverrides (const char* Text);
	void Capture (CEngine& Engine, const char* Name);
	void Log (const std::string& Line);

	std::filesystem::path OutputDir;
	uint64 StartFrame = 0u;
	uint32 NextStep = 0u;
	uint32 Failures = 0u;
	float LongestFrameMs = 0.0f;
	uint64 EditWorldHash = 0u;
	uint64 PlayWorldHash = 0u;
	uint64 PausedFrames = 0u;
	uint32 InputGeneration = 0u;
	Vector3f CameraStart = Vector3f(0.0f, 0.0f, 0.0f);
	float ReleaseSpeed = 0.0f;
	uint32 GameReloads = 0u;
	uint32 EditEntities = 0u;
	bool bCanReload = false;
	bool bFinished = false;
};
}
