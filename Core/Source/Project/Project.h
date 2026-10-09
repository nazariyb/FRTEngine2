#pragma once

#include <filesystem>

#include "Core.h"
#include "Containers/String.h"


namespace frt::project
{
// <Name>.frtproject at a project's root.
inline constexpr FStringLiteral ProjectFileExtension = ".frtproject";
// At an engine checkout's root; also how an engine directory is recognised.
inline constexpr FStringLiteral EngineFileName = "Engine.frtengine";
// <Name>.frtpackage beside a packaged game's executable (FindRunningPackage).
inline constexpr FStringLiteral PackageFileExtension = ".frtpackage";

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
 * Layout under <Dir>: Source/, Content/ and Config/ (tracked); Binaries/, Intermediate/, Local/
 * and the generated solution (not tracked).
 */
struct FRT_CORE_API SProjectDescriptor
{
	std::filesystem::path File;          // absolute
	FString Name;
	FString Module;                      // defaults to Name
	FString Engine;

	std::filesystem::path GetDir () const { return File.parent_path(); }

	// False, with a reason, when the file cannot be read or names no project.
	static bool Load (const std::filesystem::path& File, SProjectDescriptor& Out, FString* Error = nullptr);
	bool Save () const;
};

// An engine checkout: <Dir>/Engine.frtengine, [Engine] Name and Version. The name is what
// projects using a shared engine refer to; two checkouts registered side by side (a fork, a
// branch) need different names.
struct FRT_CORE_API SEngineDescriptor
{
	std::filesystem::path Dir;
	FString Name;
	FString Version;

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
FRT_CORE_API std::filesystem::path ResolveEngineDir (const SProjectDescriptor& Project, FString* Error = nullptr);

/**
 * A packaged game (FRT.bat package): a directory with everything a Release game runs with, which
 * needs neither the engine checkout nor the project's sources:
 *
 *     <Module>.exe, dxcompiler.dll, dxil.dll, D3D12/    the game and its runtime libraries
 *     <Name>.frtpackage                                 the marker: the project's descriptor
 *     Content/, Config/                                 the project's
 *     Engine/Content/Shaders, Engine/Content/Fonts,     the engine's
 *     Engine/Config/Engine.ini
 *
 * The marker holds the project's [Project] Name and Module, so it loads as an
 * SProjectDescriptor whose directory is the package's; its [Package] section says what built it
 * and is not read. A process is a packaged game when the marker is beside its executable: the
 * only *.frtpackage file there. EnginePaths.h then finds the engine's files in the package, and
 * what the game writes goes to the user's directory instead of the package's, which may be
 * read-only and is shared by every user. Empty for any other process.
 */
FRT_CORE_API std::filesystem::path FindRunningPackage ();
// The package marker directly in Dir - the only *.frtpackage there - or empty.
FRT_CORE_API std::filesystem::path FindPackageFile (const std::filesystem::path& Dir);

/**
 * The project a host was started for:
 *   - with bSearchFromExecutable, the package the executable is in (FindRunningPackage): a
 *     packaged game runs its own package, whatever it is started with;
 *   - -Project=<file or directory> on the command line, or a bare *.frtproject argument (what
 *     the IDE's debugger and the project manager pass);
 *   - else the working directory's project;
 *   - else, with bSearchFromExecutable, the nearest project above the executable: a game built
 *     into its project's Binaries and started from there.
 * Empty if none; OutError says why when a project was named but is not there.
 */
FRT_CORE_API std::filesystem::path FindProjectToRun (bool bSearchFromExecutable, FString* OutError = nullptr);

// A project's name is also its module's, its files' and its C++ identifiers' name: a letter,
// then letters, digits or underscores, at most 64 - and not an engine project's name, which the
// generated solution already uses. Descriptors with another Name or Module do not load.
FRT_CORE_API bool IsValidProjectName (FStringView Name, FString* Error = nullptr);

// Ends in .frtproject, in any case.
FRT_CORE_API bool HasProjectFileExtension (const std::filesystem::path& Path);
}
