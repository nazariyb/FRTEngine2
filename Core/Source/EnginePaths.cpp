#include "EnginePaths.h"

#include <string>
#include <system_error>
#include <vector>

#if defined(_WINDOWS)
#include <Windows.h>
#endif

#include "Project/Project.h"
#include "Project/UserRegistry.h"


namespace frt::paths
{
namespace
{
project::SProjectDescriptor Project;
bool bHasProject = false;

std::filesystem::path CurrentDir ()
{
	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::current_path(ec);
	return ec ? std::filesystem::path(".") : dir;
}

// The file Core's code was loaded from: Core.dll in the engine's Binaries, or - monolithic - the
// executable itself.
std::filesystem::path GetCoreModuleFile ()
{
#if defined(_WINDOWS)
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&GetCoreModuleFile), &module))
	{
		std::vector<wchar_t> buffer(MAX_PATH);
		while (true)
		{
			const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0u)
			{
				break;
			}
			if (length < buffer.size())
			{
				return std::filesystem::path(std::wstring(buffer.data(), length));
			}
			buffer.resize(buffer.size() * 2u);   // a long path: retry with room for it
		}
	}
#endif
	return {};
}

std::filesystem::path ResolveEngineDir ()
{
	// Core's own location: the checkout it was built in.
	if (const std::filesystem::path core = GetCoreModuleFile(); !core.empty())
	{
		if (std::filesystem::path dir = project::FindEngineDirAbove(core.parent_path()); !dir.empty())
		{
			return dir;
		}
	}
	// A monolithic executable in the project's Binaries: the project's engine.
	if (bHasProject)
	{
		if (std::filesystem::path dir = project::ResolveEngineDir(Project); !dir.empty())
		{
			return dir;
		}
	}
	if (std::filesystem::path dir = project::FindEngineDirAbove(CurrentDir()); !dir.empty())
	{
		return dir;
	}
	// No engine anywhere: lookups fail where they are made, naming the path they tried.
	return CurrentDir();
}
}

const std::filesystem::path& GetEngineDir ()
{
	static const std::filesystem::path dir = ResolveEngineDir();
	return dir;
}

bool SetProject (const project::SProjectDescriptor& InProject)
{
	Project = InProject;
	bHasProject = !Project.File.empty();
	return bHasProject;
}

bool HasProject ()
{
	return bHasProject;
}

const project::SProjectDescriptor& GetProject ()
{
	return Project;
}

std::filesystem::path GetProjectDir ()
{
	return bHasProject ? Project.GetDir() : std::filesystem::path();
}

std::filesystem::path GetEngineContentDir ()
{
	return GetEngineDir() / "Core" / "Content";
}

std::filesystem::path GetShaderRootDir ()
{
	return GetEngineContentDir() / "Shaders";
}

std::filesystem::path GetProjectContentDir ()
{
	return bHasProject ? GetProjectDir() / "Content" : std::filesystem::path();
}

std::filesystem::path GetProjectBinariesDir ()
{
	return bHasProject ? GetProjectDir() / "Binaries" / FRT_PLATFORM_NAME : std::filesystem::path();
}

std::filesystem::path GetSavedDir ()
{
	return bHasProject ? GetProjectDir() / "Saved" : project::GetUserDataDir() / "Saved";
}

std::filesystem::path GetCaptureDir ()
{
	return GetSavedDir() / "Captures";
}

std::filesystem::path GetProfilingDir ()
{
	return GetSavedDir() / "Profiling";
}

std::filesystem::path GetEngineConfigPath ()
{
	return GetEngineDir() / "Core" / "Config" / "Engine.ini";
}

std::filesystem::path GetProjectConfigPath ()
{
	return bHasProject ? GetProjectDir() / "Config" / "Game.ini" : std::filesystem::path();
}

std::filesystem::path GetUserConfigPath ()
{
	// Saved/ is never tracked: this is the only config file the running game writes.
	return GetSavedDir() / "Config" / "UserSettings.ini";
}

std::filesystem::path GetEditorUserDir ()
{
	return project::GetUserDataDir() / "Editor";
}
}
