#pragma once

#include <filesystem>

#include "Core.h"


namespace frt::project
{
struct SProjectDescriptor;
}


namespace frt::paths
{
/**
 * Where things are: the single place that knows the layout. Two roots, never confused:
 *
 *   the engine   the checkout holding Engine.frtengine: engine code, shaders, fonts, engine
 *                config and the editor's shipped defaults. Found from where Core is loaded
 *                (<engine>/Binaries/<platform>/Core*.dll), else from the project.
 *   the project  the game being run or edited (<project>/<Name>.frtproject): its content,
 *                config, binaries. Set by the host (SetProject) before the engine starts; none
 *                in the editor's project manager.
 *
 * Each has a Local/ for what is written while running and never tracked: the project's for its
 * game (settings, captures, profiling), the engine's for the editor's own files and for runs
 * without a project. The machine-wide registry of engines and recent projects is outside both
 * (Project/UserRegistry.h).
 *
 * A packaged game (Project/Project.h: FindRunningPackage) has both in its package: the project
 * is the package's directory, the engine its Engine/ - which holds Content/ and Config/ where a
 * checkout has Core/Content and Core/Config. What it writes goes to GetGameUserDataDir(<Name>)
 * rather than a Local/: a package may be read-only, and every user shares it. Every other
 * process resolves as above. Every function returns an absolute path.
 */

// The engine checkout, resolved on first use: from where Core is loaded, else - a monolithic
// executable, built into its project - from the project, which such a host sets first. A
// packaged game's Engine/.
FRT_CORE_API const std::filesystem::path& GetEngineDir ();
// This process is a packaged game: a package marker is beside its executable.
FRT_CORE_API bool IsPackaged ();

// The project this process runs: hosts call it once, before CEngine::StartUp, which reads its
// config.
FRT_CORE_API bool SetProject (const project::SProjectDescriptor& Project);
FRT_CORE_API bool HasProject ();
FRT_CORE_API const project::SProjectDescriptor& GetProject ();
// Empty without a project.
FRT_CORE_API std::filesystem::path GetProjectDir ();

// ---- engine ----
FRT_CORE_API std::filesystem::path GetEngineContentDir ();           // <engine>/Core/Content
// Root of every HLSL source and of Shared/*.h.
FRT_CORE_API std::filesystem::path GetShaderRootDir ();              // <engine>/Core/Content/Shaders

// ---- project ----
FRT_CORE_API std::filesystem::path GetProjectContentDir ();          // <project>/Content
// The game module's directory: <project>/Binaries/<platform>; a package's own.
FRT_CORE_API std::filesystem::path GetProjectBinariesDir ();
// What a running game or editor writes, untracked: <project>/Local, or <engine>/Local without a
// project; a packaged game's GetGameUserDataDir(<Name>).
FRT_CORE_API std::filesystem::path GetLocalDir ();
FRT_CORE_API std::filesystem::path GetCaptureDir ();                 // <local>/Captures
FRT_CORE_API std::filesystem::path GetProfilingDir ();               // <local>/Profiling

// ---- config layers, weakest to strongest ----
// Config is not Content: it is read before any asset system exists and must not depend on it.
// See Config/IniFile.h for why that separation matters.
FRT_CORE_API std::filesystem::path GetEngineConfigPath ();           // <engine>/Core/Config/Engine.ini
FRT_CORE_API std::filesystem::path GetProjectConfigPath ();          // <project>/Config/Game.ini; empty without a project
FRT_CORE_API std::filesystem::path GetUserConfigPath ();             // <local>/UserSettings.ini

// ---- the editor's ----
// Its own files, whatever the project (layout, input overrides, project manager): <engine>/Local/Editor.
FRT_CORE_API std::filesystem::path GetEditorUserDir ();
}
