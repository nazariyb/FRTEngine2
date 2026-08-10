#pragma once

#include <filesystem>

#include "Core.h"


namespace frt::paths
{
// Central path resolution. Currently hardcoded relative paths.
// Designed to be the single replacement point when an AssetManager / VFS lands —
// the rest of the engine should not concatenate engine-relative paths inline.

FRT_CORE_API std::filesystem::path GetContentDir ();
FRT_CORE_API std::filesystem::path GetIntermediateDir ();

FRT_CORE_API std::filesystem::path GetDxrShaderSourceDir ();
FRT_CORE_API std::filesystem::path GetDxrShaderCacheDir ();

// Profiling-session output (CSV + scene .txt). Local/, not tracked.
FRT_CORE_API std::filesystem::path GetProfilingDir ();

// Repository root, found by walking up from the working directory for premake5.lua, which
// sits there and nowhere else. Falls back to the working directory when the marker is not
// found - a deployed build has no repository, and config is optional anyway.
//
// Unlike the functions above this does not assume the working directory is exactly one
// level below the root, so it also resolves correctly from Binaries/<platform>.
FRT_CORE_API std::filesystem::path GetRepoRoot ();

// Config is not Content: it is read before the asset system exists and must not be
// reachable only through it. See Config/IniFile.h for why that separation matters.
//
// Layers, weakest to strongest.
FRT_CORE_API std::filesystem::path GetEngineConfigPath ();  // <root>/Core/Config/Engine.ini
FRT_CORE_API std::filesystem::path GetProjectConfigPath (); // <workdir>/Config/Game.ini
FRT_CORE_API std::filesystem::path GetUserConfigPath ();    // <root>/Local/UserSettings.ini
}
