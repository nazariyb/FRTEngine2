#include "EnginePaths.h"


namespace frt::paths
{
// All paths are resolved relative to the working directory, and the leading ".." assumes
// that directory is ONE level below the repository root - i.e. a project directory such
// as Demo/, which is what Visual Studio uses by default and what demo.lua now pins via
// debugdir.
//
// It is deliberately NOT the binary output directory: that is Binaries/<platform>/<config>,
// three levels down, where ".." resolves to the wrong place and content lookup fails with
// an assert inside ReadFileBytes. Launching a binary by double-clicking it will do exactly
// that. Resolving against the executable location instead is the obvious fix and belongs
// with the AssetManager / VFS work these functions are a placeholder for.
//
// FRT_PLATFORM_NAME and FRT_CONFIG_NAME come from the Premake workspace defines
// (see Premake/workspace.lua). They mirror the on-disk Intermediate/ layout.

std::filesystem::path GetContentDir ()
{
	return std::filesystem::path("..") / "Core" / "Content";
}

std::filesystem::path GetIntermediateDir ()
{
	return std::filesystem::path("..") / "Intermediate" / FRT_PLATFORM_NAME / FRT_CONFIG_NAME;
}

std::filesystem::path GetDxrShaderSourceDir ()
{
	return GetContentDir() / "Shaders" / "DXR";
}

std::filesystem::path GetDxrShaderCacheDir ()
{
	return GetIntermediateDir() / "Shaders" / "DXR";
}

std::filesystem::path GetProfilingDir ()
{
	return std::filesystem::path("..") / "Local" / "Profiling";
}
}
