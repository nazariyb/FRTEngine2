#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"


namespace frt::project
{
/**
 * Per-user data shared by every engine checkout and project on the machine, so never inside one:
 * %LOCALAPPDATA%\FRTEngine, or FRT_USER_DATA_DIR when set (tests, CI). It holds only:
 *   Engines.ini    [Engines] <name>=<directory>   engines registered by FRT.bat setup
 *   Projects.ini   [Recent] <project file>=<time> the project manager's list
 * Plain INI, also read and written by the build tools (Premake/project.lua), in UTF-8. What
 * belongs to one checkout is in its Local/ (EnginePaths.h).
 */
FRT_CORE_API std::filesystem::path GetUserDataDir ();

// A packaged game's per-user directory (Project.h: FindRunningPackage), for what it writes while
// running: %LOCALAPPDATA%\<Name>, or <FRT_USER_DATA_DIR>\<Name> when that is set.
FRT_CORE_API std::filesystem::path GetGameUserDataDir (std::string_view Name);

// The directory registered under Name (case-insensitive); empty if none.
FRT_CORE_API std::filesystem::path FindRegisteredEngine (std::string_view Name);
FRT_CORE_API bool RegisterEngine (std::string_view Name, const std::filesystem::path& Dir);
FRT_CORE_API std::vector<std::pair<std::string, std::filesystem::path>> GetRegisteredEngines ();

struct SRecentProject
{
	std::filesystem::path File;
	int64 LastOpened = 0;                // seconds since the Unix epoch
};

// Newest first.
FRT_CORE_API std::vector<SRecentProject> GetRecentProjects ();
FRT_CORE_API void AddRecentProject (const std::filesystem::path& File);
FRT_CORE_API void RemoveRecentProject (const std::filesystem::path& File);
}
