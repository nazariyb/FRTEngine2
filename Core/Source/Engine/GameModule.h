#pragma once

#include <memory>
#include <string>

#include "Core.h"
#include "CoreTypes.h"
#include "Precision.h"


namespace frt
{
class GameInstance;
}

namespace frt::config
{
class CConfig;
}


// The contract between the hosts and a game module: GameInstance's interface and the types it
// passes. Bump on any change to them.
#define FRT_GAME_API_VERSION 1u

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
 *     FRT_IMPLEMENT_GAME_MODULE(DemoGame)
 * The game is a DLL the hosts (Launcher, Editor) load, finding these two exports by name - or,
 * in a monolithic build, a static library linked into the launcher, where Core calls them
 * directly and that reference is what pulls the game into the link. A constructor that throws
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
 * per game, and the game's code stays a unit that can be rebuilt while running. The exception
 * is the monolithic (Release) launcher, built with the default game linked in.
 *
 * Everything whose code lives in the module - game instances, and worlds the game populated
 * (their component pools are instantiated in it) - must be destroyed before Unload().
 */
class FRT_CORE_API CGameModule
{
public:
	CGameModule () = default;
	~CGameModule ();
	CGameModule (const CGameModule&) = delete;
	CGameModule& operator= (const CGameModule&) = delete;

	// Loads <executable dir>/<Name>-<Configuration>.dll and checks it was built against this
	// engine's ABI. With FRT_WITH_GAME_HOT_RELOAD the DLL is loaded from a copy, so the build
	// can overwrite it while it runs (see HasNewerBuild). Monolithic builds: the game linked
	// into the executable, whose name GetName() then returns; asking for another one only logs
	// that it is not there. Logs and returns false otherwise.
	bool Load (const std::string& Name);
	void Unload ();
	bool IsLoaded () const { return CreateFunction != nullptr; }
	const std::string& GetName () const { return Name; }

	// FRT_WITH_GAME_HOT_RELOAD: the module's DLL was rebuilt since Load and the linker is done
	// with it. Reloading is Load(GetName()) once everything holding the module's code is gone.
	// Always false otherwise.
	bool HasNewerBuild () const;

	// A new game instance; null if the game's constructor failed.
	std::unique_ptr<GameInstance> CreateGame () const;

	// [Game] Module from the config (-Game:Module=<Name> on the command line); by default the
	// working directory's name, which is the project directory.
	static std::string GetConfiguredName (const config::CConfig& Config);

private:
	void* Handle = nullptr;
	GameInstance* (*CreateFunction) () = nullptr;
	std::string Name;
	// FRT_WITH_GAME_HOT_RELOAD: the build output, the copy actually loaded, and the output's
	// write time when it was copied. In every build, so the layout does not vary.
	std::wstring SourcePath;
	std::wstring LoadedPath;
	int64 SourceWriteTime = 0;
};
}
