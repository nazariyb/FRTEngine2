#include "Project/UserRegistry.h"

#include <algorithm>
#include <chrono>
#include <system_error>

#include "Config/IniFile.h"
#include "Platform/Platform.h"


namespace frt::project
{
namespace
{
constexpr FStringLiteral EnginesSection = "Engines";
constexpr FStringLiteral RecentSection = "Recent";

std::filesystem::path EnginesFile ()
{
	return GetUserDataDir() / "Engines.ini";
}

std::filesystem::path ProjectsFile ()
{
	return GetUserDataDir() / "Projects.ini";
}

// One spelling per path, so a project opened through another relative path is one entry: absolute
// and normalized, with the platform's separators - exactly as the build tools write them
// (Premake/project.lua: canonicalPath), and as a batch file can use them. Links and substituted
// drives are left as they are, since the build tools cannot resolve them. Compared as every INI
// key is: A-Z in any case.
FString Key (const std::filesystem::path& Path)
{
	std::error_code ec;
	const std::filesystem::path absolute = std::filesystem::absolute(Path, ec);
	return FString::FromPath((ec ? Path : absolute).lexically_normal(), EPathStyle::Preferred);
}

// A path the registry holds. The files are edited by hand too: text that is not UTF-8 is no path.
std::filesystem::path ToRegisteredPath (FStringView Text)
{
	return Text.TryToPath().value_or(std::filesystem::path());
}

int64 Now ()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Where per-user application data goes: %LOCALAPPDATA%, else the temporary directory.
std::filesystem::path GetLocalAppDataDir ()
{
	if (std::filesystem::path local = platform::GetEnvironmentPath("LOCALAPPDATA"); !local.empty())
	{
		return local;
	}
	std::error_code ec;
	return std::filesystem::temp_directory_path(ec);
}
}

std::filesystem::path GetUserDataDir ()
{
	if (std::filesystem::path dir = platform::GetEnvironmentPath("FRT_USER_DATA_DIR"); !dir.empty())
	{
		return dir;
	}
	return GetLocalAppDataDir() / "FRTEngine";
}

std::filesystem::path GetGameUserDataDir (FStringView Name)
{
	std::filesystem::path root = platform::GetEnvironmentPath("FRT_USER_DATA_DIR");
	if (root.empty())
	{
		root = GetLocalAppDataDir();
	}
	return root / Name.ToPath();
}

std::filesystem::path FindRegisteredEngine (FStringView Name)
{
	config::CIniFile file;
	file.LoadFromFile(EnginesFile());
	const FString* dir = file.Find(EnginesSection, Name);
	return dir ? ToRegisteredPath(*dir) : std::filesystem::path();
}

bool RegisterEngine (FStringView Name, const std::filesystem::path& Dir)
{
	config::CIniFile file;
	file.LoadFromFile(EnginesFile());
	file.Set(EnginesSection, Name, Key(Dir));
	return file.SaveToFile(EnginesFile());
}

std::vector<std::pair<FString, std::filesystem::path>> GetRegisteredEngines ()
{
	std::vector<std::pair<FString, std::filesystem::path>> engines;
	config::CIniFile file;
	file.LoadFromFile(EnginesFile());
	for (const config::CIniFile::SSection& section : file.GetSections())
	{
		if (section.Name.Equals<ESearchCase::IgnoreCaseAscii>(EnginesSection))
		{
			for (const config::CIniFile::SEntry& entry : section.Entries)
			{
				engines.emplace_back(entry.Key, ToRegisteredPath(entry.Value));
			}
		}
	}
	return engines;
}

std::vector<SRecentProject> GetRecentProjects ()
{
	std::vector<SRecentProject> projects;
	config::CIniFile file;
	file.LoadFromFile(ProjectsFile());
	for (const config::CIniFile::SSection& section : file.GetSections())
	{
		if (!section.Name.Equals<ESearchCase::IgnoreCaseAscii>(RecentSection))
		{
			continue;
		}
		for (const config::CIniFile::SEntry& entry : section.Entries)
		{
			SRecentProject project;
			project.File = ToRegisteredPath(entry.Key);
			project.LastOpened = entry.Value.ToInt64().value_or(0);
			projects.push_back(std::move(project));
		}
	}
	std::stable_sort(projects.begin(), projects.end(),
		[] (const SRecentProject& A, const SRecentProject& B) { return A.LastOpened > B.LastOpened; });
	return projects;
}

void AddRecentProject (const std::filesystem::path& File)
{
	config::CIniFile file;
	file.LoadFromFile(ProjectsFile());
	file.Set(RecentSection, Key(File), FString::FromNumber(Now()));
	file.SaveToFile(ProjectsFile());
}

void RemoveRecentProject (const std::filesystem::path& File)
{
	config::CIniFile file;
	file.LoadFromFile(ProjectsFile());
	file.Remove(RecentSection, Key(File));
	file.SaveToFile(ProjectsFile());
}
}
