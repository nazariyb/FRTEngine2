#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "Core.h"


namespace frt::project
{
// <Name>.frtproject at a project's root.
inline constexpr std::string_view ProjectFileExtension = ".frtproject";
// At an engine checkout's root; also how an engine directory is recognised.
inline constexpr std::string_view EngineFileName = "Engine.frtengine";

/**
 * A project: a game in its own directory - its own repository - with the engine elsewhere
 * (registered once and shared by many projects) or inside it (a git submodule, pinned per
 * project). Its descriptor, <Dir>/<Name>.frtproject, is an INI file committed with the game:
 *
 *     [Project]
 *     Name=MyGame
 *     Module=MyGame          the game module, Source/ built as <Module>[-<Configuration>].dll
 *     Engine=FRTEngine2      a registered engine's name (FRT.bat setup), or a path relative
 *                            to this file (an engine submodule: Engine=Engine)
 *
 * Never an absolute path or anything machine-specific: the file is shared by everyone working
 * on the game. Reserved for code shared between projects (a module directory several projects
 * reference), not read yet: SharedModules=<paths relative to this file>.
 *
 * Layout under <Dir>: Source/, Content/ and Config/ (tracked); Binaries/, Intermediate/, Saved/
 * and the generated solution (not tracked).
 */
struct FRT_CORE_API SProjectDescriptor
{
	std::filesystem::path File;          // absolute
	std::string Name;
	std::string Module;                  // defaults to Name
	std::string Engine;

	std::filesystem::path GetDir () const { return File.parent_path(); }

	// False, with a reason, when the file cannot be read or names no project.
	static bool Load (const std::filesystem::path& File, SProjectDescriptor& Out, std::string* Error = nullptr);
	bool Save () const;
};

// An engine checkout: <Dir>/Engine.frtengine, [Engine] Name and Version. The name is what
// projects using a shared engine refer to; two checkouts registered side by side (a fork, a
// branch) need different names.
struct FRT_CORE_API SEngineDescriptor
{
	std::filesystem::path Dir;
	std::string Name;
	std::string Version;

	static bool Load (const std::filesystem::path& Dir, SEngineDescriptor& Out);
	static bool IsEngineDir (const std::filesystem::path& Dir);
};

// The project file directly in Dir - the only *.frtproject there - or empty.
FRT_CORE_API std::filesystem::path FindProjectFile (const std::filesystem::path& Dir);
// The nearest directory at or above Dir that holds a project file / is an engine; empty if none.
FRT_CORE_API std::filesystem::path FindProjectFileAbove (const std::filesystem::path& Dir);
FRT_CORE_API std::filesystem::path FindEngineDirAbove (const std::filesystem::path& Dir);

/**
 * The engine a project builds and runs with. The same rules, in the same order, as the
 * project's FRT.bat and Premake/project.lua:
 *   1. FRT_ENGINE_DIR, when set: an explicit override for CI and scripts;
 *   2. the project's Engine value: a path relative to the project file, if it leads to an
 *      engine, else the name of a registered engine (UserRegistry.h);
 *   3. the nearest engine above the project: a project kept inside an engine checkout.
 * Empty, with the reason, when none applies.
 */
FRT_CORE_API std::filesystem::path ResolveEngineDir (const SProjectDescriptor& Project, std::string* Error = nullptr);

/**
 * The project a host was started for:
 *   - -Project=<file or directory> on the command line, or a bare *.frtproject argument (what
 *     the IDE's debugger and the project manager pass);
 *   - else the working directory's project;
 *   - else, with bSearchFromExecutable, the nearest project above the executable: a game built
 *     into its project's Binaries and started from there.
 * Empty if none; OutError says why when a project was named but is not there.
 */
FRT_CORE_API std::filesystem::path FindProjectToRun (bool bSearchFromExecutable, std::string* OutError = nullptr);

// A project's name is also its module's, its files' and its C++ identifiers' name: a letter,
// then letters, digits or underscores, at most 64 - and not an engine project's name, which the
// generated solution already uses.
FRT_CORE_API bool IsValidProjectName (std::string_view Name, std::string* Error = nullptr);

// UTF-8 text in config files and messages <-> paths, which on Windows would otherwise read narrow
// strings in the ANSI code page. Written with the platform's separators.
FRT_CORE_API std::filesystem::path PathFromUtf8 (std::string_view Text);
FRT_CORE_API std::string PathToUtf8 (const std::filesystem::path& Path);
// An environment variable holding a path, read in UTF-16 (any user name, any folder); empty
// when unset.
FRT_CORE_API std::filesystem::path GetEnvironmentPath (const wchar_t* Name);
}
