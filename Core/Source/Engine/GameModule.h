#pragma once

#include <filesystem>
#include <memory>

#include "Core.h"
#include "CoreTypes.h"
#include "Precision.h"
#include "Containers/String.h"


namespace frt
{
class GameInstance;
}

namespace frt::config
{
class CConfig;
}


// The contract between the hosts and a game module: GameInstance's interface and the types it
// passes, including those whose inline code the module compiles itself - the string types
// (Containers/String.h) and ImGui's, as UI/ImGuiConfig.h configures them. Bump on any change to them.
// 2: ImWchar became 32 bits (IMGUI_USE_WCHAR32), which moves members of ImFontConfig, ImFont and
// ImGuiInputTextCallbackData and renames every ImGui export taking an ImWchar.
// 3: the config the game reads (CConfig, CIniFile) and the project and host types take and hold
// FString, and reflected enum names are FStringLiteral (FRT_DECLARE_ENUM_REFLECTION).
// 4: entity, component and input-action names, render view and GPU scope names and asset archive
// keys are FString / FStringView / FStringLiteral (CEntity::Name, SpawnEntity, Comp_Name,
// FRT_DECLARE_COMPONENT, SRenderViewDesc, CGpuScope, IReadArchive / IWriteArchive).
#define FRT_GAME_API_VERSION 4u

#if defined(FRT_HEADLESS)
#define FRT_GAME_MODULE_HEADLESS 1u
#else
#define FRT_GAME_MODULE_HEADLESS 0u
#endif

// What a module and the host loading it must agree on to share types: API version, headless
// layouts, real-number precision.
#define FRT_GAME_MODULE_ABI ((FRT_GAME_API_VERSION << 16u) | (FRT_GAME_MODULE_HEADLESS << 8u) | FRT_REAL_PRECISION)

/**
 * Makes a project a game module: one line in the game's sources, e.g.
 *     FRT_IMPLEMENT_GAME_MODULE(MyGame)
 * The game is a DLL the hosts (Launcher, Editor) load, finding these exports by name - or, in a
 * monolithic build, compiled into the game's own executable (with the launcher's code), where
 * Core calls them directly. A constructor that throws
 * yields null: no exception may cross the C boundary.
 */
#if FRT_MONOLITHIC
#define FRT_GAME_MODULE_EXPORT extern "C"
#else
#define FRT_GAME_MODULE_EXPORT extern "C" __declspec(dllexport)
#endif

// The game project's name (Premake: gameProject), so a monolithic executable can say which game
// it was built with.
#ifndef FRT_GAME_MODULE_NAME
#define FRT_GAME_MODULE_NAME ""
#endif

#define FRT_IMPLEMENT_GAME_MODULE(TGame)\
	FRT_GAME_MODULE_EXPORT ::frt::GameInstance* FrtCreateGame () { try { return new TGame(); } catch (...) { return nullptr; } }\
	FRT_GAME_MODULE_EXPORT unsigned int FrtGameModuleAbi () { return FRT_GAME_MODULE_ABI; }\
	FRT_GAME_MODULE_EXPORT const char* FrtGameModuleName () { return FRT_GAME_MODULE_NAME; }


namespace frt
{
/**
 * A game module: a DLL holding a game (Unreal's game modules; Handmade Hero's game DLL). The
 * launcher and the editor are generic executables that load one by name, so neither is built
 * per game, and the game's code stays a unit that can be rebuilt while running. In a monolithic
 * (Release) build there is no module: the game is its own executable, the engine linked in.
 *
 * Everything whose code lives in the module - game instances, and worlds the game populated
 * (their component pools are instantiated in it) - must be destroyed before Unload(). Unload
 * itself releases the component types the module registered first (CComponentRegistry).
 *
 * Hot reload (FRT_WITH_GAME_HOT_RELOAD): a host polls HasNewerBuild, destroys all of the above
 * and calls Load(GetName()) again.
 */
class FRT_CORE_API CGameModule
{
public:
	CGameModule () = default;
	~CGameModule ();
	CGameModule (const CGameModule&) = delete;
	CGameModule& operator= (const CGameModule&) = delete;

	// Loads <project>/Binaries/<platform>/<Name>-<Configuration>.dll (without a project: from the
	// executable's directory) and checks it was built against this engine's ABI. With FRT_WITH_GAME_HOT_RELOAD the DLL is loaded from a copy, so the build
	// can overwrite it while it runs (see HasNewerBuild). Monolithic builds: the game linked
	// into the executable, whose name GetName() then returns; asking for another one only logs
	// that it is not there. Logs and returns false otherwise - keeping the name, and with hot
	// reload watching the build still, so that a fixed build can be loaded the same way.
	bool Load (FStringView Name);
	void Unload ();
	bool IsLoaded () const { return CreateFunction != nullptr; }
	// The module last asked for, loaded or not.
	const FString& GetName () const { return Name; }

	// FRT_WITH_GAME_HOT_RELOAD: the module's DLL was rebuilt since the last Load - which may
	// have failed - and the linker is done with it. Reloading is Load(GetName()) once
	// everything holding the module's code is gone. Always false otherwise.
	bool HasNewerBuild () const;
	// FRT_WITH_GAME_HOT_RELOAD: the DLL the build writes, which HasNewerBuild watches. Empty
	// otherwise.
	const std::filesystem::path& GetBuildPath () const { return SourcePath; }

	// A new game instance; null if the game's constructor failed.
	std::unique_ptr<GameInstance> CreateGame () const;

	// [Game] Module from the config (-Game:Module=<Name> on the command line); by default the
	// project's module (paths::GetProject).
	static FString GetConfiguredName (const config::CConfig& Config);

private:
	void* Handle = nullptr;
	GameInstance* (*CreateFunction) () = nullptr;
	FString Name;
	// FRT_WITH_GAME_HOT_RELOAD: the build output, the copy actually loaded, and the output's
	// write time when it was copied. In every build, so the layout does not vary.
	std::filesystem::path SourcePath;
	std::filesystem::path LoadedPath;
	int64 SourceWriteTime = 0;
};
}
