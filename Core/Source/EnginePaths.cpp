#include "EnginePaths.h"

#include <system_error>


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


std::filesystem::path GetRepoRoot ()
{
	std::error_code ec;
	const std::filesystem::path start = std::filesystem::current_path(ec);
	if (ec)
	{
		return ".";
	}

	std::filesystem::path dir = start;
	while (true)
	{
		if (std::filesystem::exists(dir / "premake5.lua", ec))
		{
			return dir;
		}

		if (!dir.has_parent_path())
		{
			break;
		}

		// A root path is its own parent; without this the loop never terminates.
		const std::filesystem::path parent = dir.parent_path();
		if (parent == dir)
		{
			break;
		}

		dir = parent;
	}

	return start;
}


std::filesystem::path GetEngineConfigPath ()
{
	return GetRepoRoot() / "Core" / "Config" / "Engine.ini";
}


std::filesystem::path GetProjectConfigPath ()
{
	// Relative to the working directory rather than the root: the working directory IS the
	// project directory (Demo/ via debugdir), and which project is running is exactly what
	// this layer is meant to vary on.
	std::error_code ec;
	const std::filesystem::path current = std::filesystem::current_path(ec);
	if (ec)
	{
		return std::filesystem::path("Config") / "Game.ini";
	}

	return current / "Config" / "Game.ini";
}


std::filesystem::path GetUserConfigPath ()
{
	// Local/ is gitignored, which is the point: this is the only config file the running
	// game writes, and it must never show up in a diff.
	return GetRepoRoot() / "Local" / "UserSettings.ini";
}
}
