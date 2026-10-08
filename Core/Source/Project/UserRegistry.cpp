#include "Project/UserRegistry.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <system_error>

#include "Enum.h"
#include "Config/IniFile.h"
#include "Project/Project.h"


namespace frt::project
{
namespace
{
constexpr std::string_view EnginesSection = "Engines";
constexpr std::string_view RecentSection = "Recent";

std::filesystem::path EnginesFile ()
{
	return GetUserDataDir() / "Engines.ini";
}

std::filesystem::path ProjectsFile ()
{
	return GetUserDataDir() / "Projects.ini";
}

// One spelling per path, so a project opened through another relative path is one entry: absolute,
// normalized, with the platform's separators - as the build tools write them (Premake/project.lua),
// and as a batch file can use them. Compared case-insensitively.
std::string Key (const std::filesystem::path& Path)
{
	std::error_code ec;
	std::filesystem::path normal = std::filesystem::weakly_canonical(Path, ec);
	if (ec)
	{
		normal = std::filesystem::absolute(Path, ec).lexically_normal();
	}
	return PathToUtf8(normal);
}

int64 Now ()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}

std::filesystem::path GetUserDataDir ()
{
	if (std::filesystem::path dir = GetEnvironmentPath(L"FRT_USER_DATA_DIR"); !dir.empty())
	{
		return dir;
	}
	if (const std::filesystem::path local = GetEnvironmentPath(L"LOCALAPPDATA"); !local.empty())
	{
		return local / "FRTEngine";
	}
	std::error_code ec;
	return std::filesystem::temp_directory_path(ec) / "FRTEngine";
}

std::filesystem::path FindRegisteredEngine (std::string_view Name)
{
	config::CIniFile file;
	file.LoadFromFile(EnginesFile());
	const std::string* dir = file.Find(EnginesSection, Name);
	return dir && !dir->empty() ? PathFromUtf8(*dir) : std::filesystem::path();
}

bool RegisterEngine (std::string_view Name, const std::filesystem::path& Dir)
{
	config::CIniFile file;
	file.LoadFromFile(EnginesFile());
	file.Set(EnginesSection, Name, Key(Dir));
	return file.SaveToFile(EnginesFile());
}

std::vector<std::pair<std::string, std::filesystem::path>> GetRegisteredEngines ()
{
	std::vector<std::pair<std::string, std::filesystem::path>> engines;
	config::CIniFile file;
	file.LoadFromFile(EnginesFile());
	for (const config::CIniFile::SSection& section : file.GetSections())
	{
		if (enum_::EqualsIgnoreCase(section.Name, EnginesSection))
		{
			for (const config::CIniFile::SEntry& entry : section.Entries)
			{
				engines.emplace_back(entry.Key, PathFromUtf8(entry.Value));
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
		if (!enum_::EqualsIgnoreCase(section.Name, RecentSection))
		{
			continue;
		}
		for (const config::CIniFile::SEntry& entry : section.Entries)
		{
			SRecentProject project;
			project.File = PathFromUtf8(entry.Key);
			std::from_chars(entry.Value.data(), entry.Value.data() + entry.Value.size(), project.LastOpened);
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
	file.Set(RecentSection, Key(File), std::to_string(Now()));
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
