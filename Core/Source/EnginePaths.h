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
 *                config, binaries, and Saved/ for what it writes while running. Set by the host
 *                (SetProject) before the engine starts; none in the editor's project manager.
 *
 * Plus the per-user directory (Project/UserRegistry.h) for what belongs to neither: the
 * engine registry, recent projects, the editor's layout and input overrides.
 * Every function returns an absolute path.
 */

// The engine checkout, resolved on first use: from where Core is loaded, else - a monolithic
// executable, built into its project - from the project, which such a host sets first.
FRT_CORE_API const std::filesystem::path& GetEngineDir ();

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
// The game module's directory: <project>/Binaries/<platform>.
FRT_CORE_API std::filesystem::path GetProjectBinariesDir ();
// What a running game or editor writes for the project, untracked: <project>/Saved, or
// <user data>/Saved without a project.
FRT_CORE_API std::filesystem::path GetSavedDir ();
FRT_CORE_API std::filesystem::path GetCaptureDir ();                 // <saved>/Captures
FRT_CORE_API std::filesystem::path GetProfilingDir ();               // <saved>/Profiling

// ---- config layers, weakest to strongest ----
// Config is not Content: it is read before any asset system exists and must not depend on it.
// See Config/IniFile.h for why that separation matters.
FRT_CORE_API std::filesystem::path GetEngineConfigPath ();           // <engine>/Core/Config/Engine.ini
FRT_CORE_API std::filesystem::path GetProjectConfigPath ();          // <project>/Config/Game.ini; empty without a project
FRT_CORE_API std::filesystem::path GetUserConfigPath ();             // <saved>/Config/UserSettings.ini

// ---- per user ----
// The editor's own files (layout, input overrides): <user data>/Editor.
FRT_CORE_API std::filesystem::path GetEditorUserDir ();
}
