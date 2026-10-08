#include "Engine/GameModule.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(_WINDOWS)
#include <Windows.h>
#endif

#include "BuildConfig.h"
#include "GameInstance.h"
#include "Config/Config.h"


#if FRT_MONOLITHIC
// The game module is a static library linked into the executable (Premake: hostProject). These
// references are what pull its objects into the link. A link without a game - Core-Test, if it
// ever reaches this file - binds them to the fallbacks instead of failing, and Load() reports
// that no game is linked in. x64 names: C symbols carry no leading underscore.
extern "C" ::frt::GameInstance* FrtCreateGame ();
extern "C" unsigned int FrtGameModuleAbi ();
extern "C" ::frt::GameInstance* FrtCreateGameNotLinked () { return nullptr; }
extern "C" unsigned int FrtGameModuleAbiNotLinked () { return 0u; }
#pragma comment(linker, "/ALTERNATENAME:FrtCreateGame=FrtCreateGameNotLinked")
#pragma comment(linker, "/ALTERNATENAME:FrtGameModuleAbi=FrtGameModuleAbiNotLinked")
#endif


namespace frt
{
namespace
{
#if !FRT_MONOLITHIC && defined(_WINDOWS)
std::filesystem::path GetExecutableDir ()
{
	wchar_t buffer[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	if (length > 0u && length < MAX_PATH)
	{
		return std::filesystem::path(buffer).parent_path();
	}
	std::error_code ec;
	return std::filesystem::current_path(ec);
}

// Module DLLs exist only where builds are not monolithic, and those name their binaries
// <Project>-<Configuration> (Premake/common.lua).
std::string MakeModuleFileName (const std::string& Name)
{
	return Name + "-" + FRT_CONFIG_NAME + ".dll";
}
#endif
}

CGameModule::~CGameModule ()
{
	Unload();
}

bool CGameModule::Load (const std::string& InName)
{
	Unload();
#if FRT_MONOLITHIC
	const unsigned int abi = FrtGameModuleAbi();
	if (abi == 0u)
	{
		std::fprintf(stderr, "[game] no game module is linked into this executable\n");
		return false;
	}
	if (abi != FRT_GAME_MODULE_ABI)
	{
		std::fprintf(stderr, "[game] the linked game module was built for another engine ABI (0x%08x, expected 0x%08x)\n",
			abi, FRT_GAME_MODULE_ABI);
		return false;
	}
	CreateFunction = &FrtCreateGame;
	Name = InName;
	std::printf("[game] module '%s' linked in\n", Name.c_str());
	return true;
#elif defined(_WINDOWS)
	const std::filesystem::path path = GetExecutableDir() / MakeModuleFileName(InName);
	HMODULE module = LoadLibraryW(path.c_str());
	if (!module)
	{
		std::fprintf(stderr, "[game] cannot load game module '%s' (%ls), error %lu\n", InName.c_str(), path.c_str(), GetLastError());
		return false;
	}

	using AbiFunction = unsigned int (*) ();
	const AbiFunction abi = reinterpret_cast<AbiFunction>(GetProcAddress(module, "FrtGameModuleAbi"));
	const auto create = reinterpret_cast<GameInstance* (*) ()>(GetProcAddress(module, "FrtCreateGame"));
	if (!abi || !create)
	{
		std::fprintf(stderr, "[game] '%ls' is not a game module (no FRT_IMPLEMENT_GAME_MODULE)\n", path.c_str());
		FreeLibrary(module);
		return false;
	}
	if (abi() != FRT_GAME_MODULE_ABI)
	{
		std::fprintf(stderr, "[game] '%ls' was built for another engine ABI (0x%08x, expected 0x%08x): rebuild it\n",
			path.c_str(), abi(), FRT_GAME_MODULE_ABI);
		FreeLibrary(module);
		return false;
	}

	Handle = module;
	CreateFunction = create;
	Name = InName;
	std::printf("[game] module '%s' loaded\n", Name.c_str());
	return true;
#else
	(void)InName;
	return false;
#endif
}

void CGameModule::Unload ()
{
#if !FRT_MONOLITHIC && defined(_WINDOWS)
	if (Handle)
	{
		FreeLibrary(static_cast<HMODULE>(Handle));
	}
#endif
	Handle = nullptr;
	CreateFunction = nullptr;
	Name.clear();
}

std::unique_ptr<GameInstance> CGameModule::CreateGame () const
{
	frt_assert(CreateFunction);
	return std::unique_ptr<GameInstance>(CreateFunction ? CreateFunction() : nullptr);
}

std::string CGameModule::GetConfiguredName (const config::CConfig& Config)
{
	const std::string configured = Config.Get("Game", "Module", "");
	if (!configured.empty())
	{
		return configured;
	}
	std::error_code ec;
	return std::filesystem::current_path(ec).filename().string();
}
}
