#pragma once

#include <filesystem>

#include "Core.h"
#include "Profiler/ProfilingSession.h"
#include "Profiler/SceneDescriptor.h"


namespace frt::profiler
{
// One session = one named folder under paths::GetProfilingDir(). Per configuration:
//   <index>.txt — that test's settings (config params) + static scene data, key: value
//   <index>.csv — EVERY measured frame, one row per frame (full per-frame history)
//
// Caller owns folder lifecycle (delete-on-rerun). These write into an existing dir,
// called the moment a configuration finishes (before the next begins).

FRT_CORE_API bool WriteConfigTxt (
	const std::filesystem::path& Dir,
	uint32 Index,
	const SProfileConfig& Config,
	const SSceneDescriptor& Scene);

FRT_CORE_API bool WriteConfigFramesCsv (
	const std::filesystem::path& Dir,
	uint32 Index,
	const SProfileResult& Result);
}
