#pragma once

#include <filesystem>
#include <string>

#include "CoreTypes.h"


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
 * (Stop must leave the edit world exactly as Play found it),
 * captures the whole window to PNG along the way, and exits with 0 when every check passed (2
 * otherwise). Captures and a log go to [Editor] SmokeTestDir, by default Local/Editor/SmokeTest.
 */
class CEditorSmokeTest
{
public:
	explicit CEditorSmokeTest (const config::CConfig& Config);

	// Every frame, before the editor applies its commands.
	void Update (CEditor& Editor, CEngine& Engine);

private:
	void Check (bool bPassed, const char* What);
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
	bool bFinished = false;
};
}
