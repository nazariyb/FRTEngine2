#include "Project/Project.h"

#include <optional>
#include <system_error>
#include <vector>

#include "Config/IniFile.h"
#include "Platform/Platform.h"
#include "Project/UserRegistry.h"


namespace frt::project
{
namespace
{
constexpr FStringLiteral ProjectSection = "Project";
constexpr FStringLiteral EngineSection = "Engine";

// Projects the generated solution already has, and names that would read as engine parts.
constexpr FStringLiteral ReservedNames[] =
{
	"Core", "Editor", "Launcher", "Engine", "ImGui", "assimp", "gtest", "googletest", "ThirdParty",
};

FString Get (const config::CIniFile& File, FStringView Section, FStringView Key)
{
	const FString* value = File.Find(Section, Key);
	return value ? *value : FString();
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

// The extensions are ASCII, so ignoring A-Z case is all of what Windows' own rule does for them.
bool HasExtension (const std::filesystem::path& Path, FStringView Extension)
{
	return FString::FromPath(Path.extension()).Equals<ESearchCase::IgnoreCaseAscii>(Extension);
}

// The only file directly in Dir with the extension, or empty.
std::filesystem::path FindOnlyFile (const std::filesystem::path& Dir, FStringView Extension)
{
	std::filesystem::path found;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(Dir, ec), end; !ec && it != end; it.increment(ec))
	{
		if (it->is_regular_file(ec) && HasExtension(it->path(), Extension))
		{
			if (!found.empty())
			{
				return {};   // ambiguous
			}
			found = it->path();
		}
	}
	return found.empty() ? found : Canonical(found);
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

bool HasProjectFileExtension (const std::filesystem::path& Path)
{
	return HasExtension(Path, ProjectFileExtension);
}

bool SProjectDescriptor::Load (const std::filesystem::path& InFile, SProjectDescriptor& Out, FString* Error)
{
	config::CIniFile file;
	if (!file.LoadFromFile(InFile))
	{
		if (Error)
		{
			*Error = "cannot read " + FString::FromPath(InFile, EPathStyle::Preferred);
		}
		return false;
	}
	SProjectDescriptor project;
	project.File = Canonical(InFile);
	project.Name = Get(file, ProjectSection, "Name");
	project.Module = Get(file, ProjectSection, "Module");
	project.Engine = Get(file, ProjectSection, "Engine");
	if (project.Name.IsEmpty())
	{
		if (Error)
		{
			*Error = FString::FromPath(InFile, EPathStyle::Preferred) + " has no [Project] Name";
		}
		return false;
	}
	if (project.Module.IsEmpty())
	{
		project.Module = project.Name;
	}
	// Both name generated projects and files: never an engine project's, or a path.
	for (const FString* name : { &project.Name, &project.Module })
	{
		FString reason;
		if (!IsValidProjectName(*name, &reason))
		{
			if (Error)
			{
				*Error = FString::FromPath(InFile, EPathStyle::Preferred) + ": " + reason;
			}
			return false;
		}
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
	return !Dir.empty() && std::filesystem::is_regular_file(Dir / EngineFileName.ToPath(), ec);
}

bool SEngineDescriptor::Load (const std::filesystem::path& Dir, SEngineDescriptor& Out)
{
	config::CIniFile file;
	if (!file.LoadFromFile(Dir / EngineFileName.ToPath()))
	{
		return false;
	}
	Out.Dir = Canonical(Dir);
	Out.Name = Get(file, EngineSection, "Name");
	Out.Version = Get(file, EngineSection, "Version");
	return !Out.Name.IsEmpty();
}

std::filesystem::path FindProjectFile (const std::filesystem::path& Dir)
{
	// Ambiguous - more than one - is none: say which with -Project=.
	return FindOnlyFile(Dir, ProjectFileExtension);
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

std::filesystem::path ResolveEngineDir (const SProjectDescriptor& Project, FString* Error)
{
	const std::filesystem::path projectDir = Project.GetDir();

	if (const std::filesystem::path overrideDir = platform::GetEnvironmentPath("FRT_ENGINE_DIR"); !overrideDir.empty())
	{
		const std::filesystem::path dir = Canonical(overrideDir);
		if (SEngineDescriptor::IsEngineDir(dir))
		{
			return dir;
		}
		if (Error)
		{
			*Error = "FRT_ENGINE_DIR (" + FString::FromPath(dir, EPathStyle::Preferred) + ") is not an engine directory";
		}
		return {};
	}

	if (!Project.Engine.IsEmpty())
	{
		// Text from a file people edit: what is not UTF-8 is no path.
		const std::filesystem::path asPath = Project.Engine.TryToPath().value_or(std::filesystem::path());
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
		*Error = Project.Engine.IsEmpty()
			? "the project names no engine and is not inside one"
			: FString::Format("no engine '{}': not a directory with {} next to the project, and no engine registered "
				"under that name (run FRT.bat setup in the engine)", Project.Engine, EngineFileName);
	}
	return {};
}

std::filesystem::path FindPackageFile (const std::filesystem::path& Dir)
{
	return FindOnlyFile(Dir, PackageFileExtension);
}

std::filesystem::path FindRunningPackage ()
{
	const std::filesystem::path dir = platform::GetExecutablePath().parent_path();
	return dir.empty() ? dir : FindPackageFile(dir);
}

std::filesystem::path FindProjectToRun (bool bSearchFromExecutable, FString* OutError)
{
	if (bSearchFromExecutable)
	{
		if (std::filesystem::path package = FindRunningPackage(); !package.empty())
		{
			return package;
		}
	}

	// -Project=<file or directory>, else the first bare argument naming a project file.
	const std::vector<FString> arguments = platform::GetCommandLineArguments();
	std::filesystem::path named;
	if (const std::optional<FString> value = platform::FindCommandLineValue(arguments, "Project"); value && !value->IsEmpty())
	{
		named = value->ToPath();
	}
	else
	{
		for (const FString& argument : arguments)
		{
			if (std::filesystem::path file = argument.ToPath(); HasProjectFileExtension(file))
			{
				named = std::move(file);
				break;
			}
		}
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
			*OutError = "no project at " + FString::FromPath(named, EPathStyle::Preferred);
		}
		return {};
	}

	std::error_code ec;
	if (std::filesystem::path file = FindProjectFile(std::filesystem::current_path(ec)); !file.empty())
	{
		return file;
	}
	if (bSearchFromExecutable)
	{
		if (const std::filesystem::path dir = platform::GetExecutablePath().parent_path(); !dir.empty())
		{
			return FindProjectFileAbove(dir);
		}
	}
	return {};
}

bool IsValidProjectName (FStringView Name, FString* Error)
{
	const auto fail = [Error] (FString Reason)
	{
		if (Error)
		{
			*Error = std::move(Reason);
		}
		return false;
	};
	if (Name.IsEmpty() || Name.Len() > 64u)
	{
		return fail("a project name has 1 to 64 characters");
	}
	if (!ascii::IsAlpha(Name.First()))
	{
		return fail("a project name starts with a letter");
	}
	for (const char c : Name)
	{
		if (!ascii::IsIdentifierChar(c))
		{
			return fail("a project name has only letters, digits and underscores");
		}
	}
	for (const FStringLiteral reserved : ReservedNames)
	{
		if (Name.Equals<ESearchCase::IgnoreCaseAscii>(reserved))
		{
			return fail(FString::Format("'{}' is the name of a part of the engine", Name));
		}
	}
	return true;
}
}
