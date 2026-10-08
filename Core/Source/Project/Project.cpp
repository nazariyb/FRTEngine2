#include "Project/Project.h"

#include <cstdlib>
#include <cwchar>
#include <system_error>

#if defined(_WINDOWS)
#include <Windows.h>
#include <shellapi.h>
#endif

#include "Enum.h"
#include "Config/IniFile.h"
#include "Project/UserRegistry.h"


namespace frt::project
{
namespace
{
constexpr std::string_view ProjectSection = "Project";
constexpr std::string_view EngineSection = "Engine";

// Projects the generated solution already has, and names that would read as engine parts.
constexpr std::string_view ReservedNames[] =
{
	"Core", "Editor", "Launcher", "Engine", "ImGui", "assimp", "gtest", "googletest", "ThirdParty",
};

std::string Get (const config::CIniFile& File, std::string_view Section, std::string_view Key)
{
	const std::string* value = File.Find(Section, Key);
	return value ? *value : std::string();
}

bool IsDir (const std::filesystem::path& Path)
{
	std::error_code ec;
	return std::filesystem::is_directory(Path, ec);
}

std::filesystem::path Canonical (const std::filesystem::path& Path)
{
	std::error_code ec;
	std::filesystem::path result = std::filesystem::weakly_canonical(Path, ec);
	return ec ? std::filesystem::absolute(Path, ec).lexically_normal() : result;
}

template <typename TPredicate>
std::filesystem::path FindAbove (const std::filesystem::path& Start, TPredicate&& Found)
{
	std::filesystem::path dir = Canonical(Start);
	while (!dir.empty())
	{
		if (Found(dir))
		{
			return dir;
		}
		const std::filesystem::path parent = dir.parent_path();
		if (parent == dir)
		{
			break;
		}
		dir = parent;
	}
	return {};
}
}

std::filesystem::path PathFromUtf8 (std::string_view Text)
{
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(Text.data()), Text.size()));
}

std::filesystem::path GetEnvironmentPath (const wchar_t* Name)
{
	wchar_t* value = nullptr;
	size_t length = 0u;
	std::filesystem::path result;
	if (_wdupenv_s(&value, &length, Name) == 0 && value)
	{
		result = value;
	}
	std::free(value);
	return result;
}

std::string PathToUtf8 (const std::filesystem::path& Path)
{
	const std::u8string text = std::filesystem::path(Path).make_preferred().u8string();
	return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool SProjectDescriptor::Load (const std::filesystem::path& InFile, SProjectDescriptor& Out, std::string* Error)
{
	config::CIniFile file;
	if (!file.LoadFromFile(InFile))
	{
		if (Error)
		{
			*Error = "cannot read " + PathToUtf8(InFile);
		}
		return false;
	}
	SProjectDescriptor project;
	project.File = Canonical(InFile);
	project.Name = Get(file, ProjectSection, "Name");
	project.Module = Get(file, ProjectSection, "Module");
	project.Engine = Get(file, ProjectSection, "Engine");
	if (project.Name.empty())
	{
		if (Error)
		{
			*Error = PathToUtf8(InFile) + " has no [Project] Name";
		}
		return false;
	}
	if (project.Module.empty())
	{
		project.Module = project.Name;
	}
	Out = std::move(project);
	return true;
}

bool SProjectDescriptor::Save () const
{
	config::CIniFile file;
	file.LoadFromFile(File);   // keeps whatever else the file holds
	file.Set(ProjectSection, "Name", Name);
	file.Set(ProjectSection, "Module", Module);
	file.Set(ProjectSection, "Engine", Engine);
	return file.SaveToFile(File);
}

bool SEngineDescriptor::IsEngineDir (const std::filesystem::path& Dir)
{
	std::error_code ec;
	return !Dir.empty() && std::filesystem::is_regular_file(Dir / EngineFileName, ec);
}

bool SEngineDescriptor::Load (const std::filesystem::path& Dir, SEngineDescriptor& Out)
{
	config::CIniFile file;
	if (!file.LoadFromFile(Dir / EngineFileName))
	{
		return false;
	}
	Out.Dir = Canonical(Dir);
	Out.Name = Get(file, EngineSection, "Name");
	Out.Version = Get(file, EngineSection, "Version");
	return !Out.Name.empty();
}

std::filesystem::path FindProjectFile (const std::filesystem::path& Dir)
{
	std::filesystem::path found;
	std::error_code ec;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(Dir, ec))
	{
		if (entry.is_regular_file(ec) && entry.path().extension() == ProjectFileExtension)
		{
			if (!found.empty())
			{
				return {};   // ambiguous: say which with -Project=
			}
			found = entry.path();
		}
	}
	return found.empty() ? found : Canonical(found);
}

std::filesystem::path FindProjectFileAbove (const std::filesystem::path& Dir)
{
	const std::filesystem::path dir = FindAbove(Dir, [] (const std::filesystem::path& Candidate) { return !FindProjectFile(Candidate).empty(); });
	return dir.empty() ? dir : FindProjectFile(dir);
}

std::filesystem::path FindEngineDirAbove (const std::filesystem::path& Dir)
{
	return FindAbove(Dir, [] (const std::filesystem::path& Candidate) { return SEngineDescriptor::IsEngineDir(Candidate); });
}

std::filesystem::path ResolveEngineDir (const SProjectDescriptor& Project, std::string* Error)
{
	const std::filesystem::path projectDir = Project.GetDir();

	if (const std::filesystem::path overrideDir = GetEnvironmentPath(L"FRT_ENGINE_DIR"); !overrideDir.empty())
	{
		const std::filesystem::path dir = Canonical(overrideDir);
		if (SEngineDescriptor::IsEngineDir(dir))
		{
			return dir;
		}
		if (Error)
		{
			*Error = "FRT_ENGINE_DIR (" + PathToUtf8(dir) + ") is not an engine directory";
		}
		return {};
	}

	if (!Project.Engine.empty())
	{
		const std::filesystem::path asPath = PathFromUtf8(Project.Engine);
		const std::filesystem::path dir = Canonical(asPath.is_absolute() ? asPath : projectDir / asPath);
		if (SEngineDescriptor::IsEngineDir(dir))
		{
			return dir;
		}
		const std::filesystem::path registered = FindRegisteredEngine(Project.Engine);
		if (SEngineDescriptor::IsEngineDir(registered))
		{
			return Canonical(registered);
		}
	}

	if (const std::filesystem::path above = FindEngineDirAbove(projectDir); !above.empty())
	{
		return above;
	}

	if (Error)
	{
		*Error = Project.Engine.empty()
			? "the project names no engine and is not inside one"
			: "no engine '" + Project.Engine + "': not a directory with " + std::string(EngineFileName) +
			  " next to the project, and no engine registered under that name (run FRT.bat setup in the engine)";
	}
	return {};
}

std::filesystem::path FindProjectToRun (bool bSearchFromExecutable, std::string* OutError)
{
#if defined(_WINDOWS)
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::filesystem::path named;
	for (int i = 1; argv && i < argc; ++i)
	{
		const std::wstring_view argument = argv[i];
		for (const std::wstring_view prefix : { std::wstring_view(L"-Project="), std::wstring_view(L"--Project="), std::wstring_view(L"/Project=") })
		{
			if (argument.size() > prefix.size() && _wcsnicmp(argument.data(), prefix.data(), prefix.size()) == 0)
			{
				named = std::filesystem::path(argument.substr(prefix.size()));
			}
		}
		if (named.empty() && std::filesystem::path(argument).extension() == ProjectFileExtension)
		{
			named = std::filesystem::path(argument);
		}
	}
	if (argv)
	{
		LocalFree(argv);
	}
	if (!named.empty())
	{
		const std::filesystem::path file = IsDir(named) ? FindProjectFile(named) : named;
		std::error_code ec;
		if (!file.empty() && std::filesystem::is_regular_file(file, ec))
		{
			return Canonical(file);
		}
		if (OutError)
		{
			*OutError = "no project at " + PathToUtf8(named);
		}
		return {};
	}
#endif

	std::error_code ec;
	if (std::filesystem::path file = FindProjectFile(std::filesystem::current_path(ec)); !file.empty())
	{
		return file;
	}
#if defined(_WINDOWS)
	if (bSearchFromExecutable)
	{
		wchar_t buffer[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length > 0u && length < MAX_PATH)
		{
			return FindProjectFileAbove(std::filesystem::path(buffer).parent_path());
		}
	}
#endif
	return {};
}

bool IsValidProjectName (std::string_view Name, std::string* Error)
{
	const auto fail = [Error] (std::string Reason)
	{
		if (Error)
		{
			*Error = std::move(Reason);
		}
		return false;
	};
	if (Name.empty() || Name.size() > 64u)
	{
		return fail("a project name has 1 to 64 characters");
	}
	const auto isLetter = [] (char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	if (!isLetter(Name.front()))
	{
		return fail("a project name starts with a letter");
	}
	for (const char c : Name)
	{
		if (!isLetter(c) && !(c >= '0' && c <= '9') && c != '_')
		{
			return fail("a project name has only letters, digits and underscores");
		}
	}
	for (const std::string_view reserved : ReservedNames)
	{
		if (enum_::EqualsIgnoreCase(Name, reserved))
		{
			return fail("'" + std::string(Name) + "' is the name of a part of the engine");
		}
	}
	return true;
}
}
