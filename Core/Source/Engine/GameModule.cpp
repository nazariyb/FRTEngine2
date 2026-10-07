#include "Engine/GameModule.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(_WINDOWS)
#include <Windows.h>
#endif

#include "GameInstance.h"
#include "Config/Config.h"


namespace frt
{
namespace
{
std::filesystem::path GetExecutableDir ()
{
#if defined(_WINDOWS)
	wchar_t buffer[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	if (length > 0u && length < MAX_PATH)
	{
		return std::filesystem::path(buffer).parent_path();
	}
#endif
	std::error_code ec;
	return std::filesystem::current_path(ec);
}

// The build names binaries <Project>-<Configuration> outside Release (Premake/common.lua).
std::string MakeModuleFileName (const std::string& Name)
{
#if defined(RELEASE)
	return Name + ".dll";
#else
	return Name + "-" + FRT_CONFIG_NAME + ".dll";
#endif
}
}

CGameModule::~CGameModule ()
{
	Unload();
}

bool CGameModule::Load (const std::string& InName)
{
	Unload();
#if defined(_WINDOWS)
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
#if defined(_WINDOWS)
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
