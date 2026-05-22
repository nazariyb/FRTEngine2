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
}
