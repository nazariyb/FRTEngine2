#include "EnginePaths.h"


namespace frt::paths
{
// All paths are resolved relative to the working directory used at engine startup,
// which is the binary output dir (e.g. Binaries/Win64). Hence the leading "..".
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
}
