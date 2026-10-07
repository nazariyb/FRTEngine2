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
 * Makes a DLL a game module: one line in the game's sources, e.g.
 *     FRT_IMPLEMENT_GAME_MODULE(DemoGame)
 * The hosts (Launcher, Editor) find these two exports by name.
 */
#define FRT_IMPLEMENT_GAME_MODULE(TGame)\
	extern "C" __declspec(dllexport) ::frt::GameInstance* FrtCreateGame () { return new TGame(); }\
	extern "C" __declspec(dllexport) unsigned int FrtGameModuleAbi () { return FRT_GAME_MODULE_ABI; }


namespace frt
{
/**
 * A game module: a DLL holding a game (Unreal's game modules; Handmade Hero's game DLL). The
 * launcher and the editor are generic executables that load one by name, so neither is built
 * per game, and the game's code stays a unit that could later be reloaded while running.
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

	// Loads <executable dir>/<Name>.dll - with the build's -<Configuration> suffix outside
	// Release - and checks it was built against this engine's ABI. Logs and returns false
	// otherwise.
	bool Load (const std::string& Name);
	void Unload ();
	bool IsLoaded () const { return Handle != nullptr; }
	const std::string& GetName () const { return Name; }

	// A new game instance.
	std::unique_ptr<GameInstance> CreateGame () const;

	// [Game] Module from the config (-Game:Module=<Name> on the command line); by default the
	// working directory's name, which is the project directory.
	static std::string GetConfiguredName (const config::CConfig& Config);

private:
	void* Handle = nullptr;
	GameInstance* (*CreateFunction) () = nullptr;
	std::string Name;
};
}
