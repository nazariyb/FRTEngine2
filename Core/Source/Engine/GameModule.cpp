#include "Engine/GameModule.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WINDOWS)
#include <Windows.h>
#endif

#include "BuildConfig.h"
#include "EnginePaths.h"
#include "GameInstance.h"
#include "Config/Config.h"
#include "Project/Project.h"


#if FRT_MONOLITHIC
// The game module is a static library linked into the executable (Premake: hostProject). These
// references are what pull its objects into the link. A link without a game - Core-Test, if it
// ever reaches this file - binds them to the fallbacks instead of failing, and Load() reports
// that no game is linked in. x64 names: C symbols carry no leading underscore.
extern "C" ::frt::GameInstance* FrtCreateGame ();
extern "C" unsigned int FrtGameModuleAbi ();
extern "C" const char* FrtGameModuleName ();
extern "C" ::frt::GameInstance* FrtCreateGameNotLinked () { return nullptr; }
extern "C" unsigned int FrtGameModuleAbiNotLinked () { return 0u; }
extern "C" const char* FrtGameModuleNameNotLinked () { return ""; }
#pragma comment(linker, "/ALTERNATENAME:FrtCreateGame=FrtCreateGameNotLinked")
#pragma comment(linker, "/ALTERNATENAME:FrtGameModuleAbi=FrtGameModuleAbiNotLinked")
#pragma comment(linker, "/ALTERNATENAME:FrtGameModuleName=FrtGameModuleNameNotLinked")
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

// Outside Release the build names binaries <Project>-<Configuration> (Premake/common.lua), and
// only non-monolithic builds have a module file at all.
std::string MakeModuleFileName (const std::string& Name)
{
	return Name + "-" + FRT_CONFIG_NAME + ".dll";
}
#endif

#if FRT_WITH_GAME_HOT_RELOAD && defined(_WINDOWS)
// Hot reload: the loader keeps a loaded DLL open, so the linker could not overwrite the build
// output while the game runs. The module is loaded from a copy instead, on a short path
// (LoadLibrary is limited to MAX_PATH):
//     <exe dir>/HotReload/<pid>-<n>/<Module>-<Configuration>.dll|.pdb
// The game project links with /PDBALTPATH:<Module>-<Configuration>.pdb, so the DLL names its
// PDB without a directory and the debugger takes the copy beside the loaded DLL, leaving the
// build's own PDB free for the next link too.
int64 GetWriteTime (const std::filesystem::path& Path)
{
	std::error_code ec;
	const auto time = std::filesystem::last_write_time(Path, ec);
	return ec ? 0 : static_cast<int64>(time.time_since_epoch().count());
}

// False while another process - the linker - has the file open. A missing file is finished.
bool IsWriteFinished (const std::filesystem::path& Path)
{
	const HANDLE file = CreateFileW(Path.c_str(), GENERIC_READ, 0u, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return GetLastError() == ERROR_FILE_NOT_FOUND;
	}
	CloseHandle(file);
	return true;
}

bool IsProcessAlive (DWORD Pid)
{
	const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
	if (!process)
	{
		return GetLastError() == ERROR_ACCESS_DENIED;
	}
	DWORD exitCode = 0u;
	const bool bAlive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
	CloseHandle(process);
	return bAlive;
}

// Copies left by this process or by processes that have exited. Whatever is still open - a
// debugger can hold a PDB a while after FreeLibrary - stays until a later Load.
void RemoveStaleCopies (const std::filesystem::path& Root)
{
	std::error_code ec;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(Root, ec))
	{
		const std::wstring name = entry.path().filename().wstring();
		const DWORD pid = static_cast<DWORD>(std::wcstoul(name.c_str(), nullptr, 10));
		if (pid == GetCurrentProcessId() || !IsProcessAlive(pid))
		{
			std::error_code removeEc;
			std::filesystem::remove_all(entry.path(), removeEc);
		}
	}
}

// The DLL (and its PDB) copied to a directory of their own; empty on failure.
std::filesystem::path MakeShadowCopy (const std::filesystem::path& Source)
{
	static std::atomic<uint32> generation = 0u;

	const std::filesystem::path root = Source.parent_path() / "HotReload";
	RemoveStaleCopies(root);

	std::error_code ec;
	const std::filesystem::path dir = root / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++generation));
	std::filesystem::create_directories(dir, ec);
	const std::filesystem::path copy = dir / Source.filename();
	if (ec || !std::filesystem::copy_file(Source, copy, std::filesystem::copy_options::overwrite_existing, ec))
	{
		return {};
	}
	std::filesystem::path pdb = Source;
	pdb.replace_extension(".pdb");
	if (std::filesystem::exists(pdb, ec))
	{
		// Without it the game still runs, only without symbols.
		std::filesystem::copy_file(pdb, dir / pdb.filename(), std::filesystem::copy_options::overwrite_existing, ec);
	}
	return copy;
}
#endif
}

CGameModule::~CGameModule ()
{
	Unload();
}

bool CGameModule::Load (const std::string& InName)
{
	// A copy first: InName may be Name itself (a reload), which Unload clears.
	const std::string requested = InName;
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
	Name = FrtGameModuleName();
	if (Name.empty())
	{
		Name = requested;
	}
	else if (!requested.empty() && requested != Name)
	{
		std::fprintf(stderr, "[game] module '%s' requested, but this executable is built with '%s'\n", requested.c_str(), Name.c_str());
	}
	std::printf("[game] module '%s' linked in\n", Name.c_str());
	return true;
#elif defined(_WINDOWS)
	// The project's binaries; the host's own directory when it runs without a project.
	const std::filesystem::path source = (paths::HasProject() ? paths::GetProjectBinariesDir() : GetExecutableDir()) / MakeModuleFileName(requested);
	std::filesystem::path path = source;
#if FRT_WITH_GAME_HOT_RELOAD
	// Taken before the copy, so a build finishing during it still reads as newer.
	const int64 sourceWriteTime = GetWriteTime(source);
	if (std::filesystem::path copy = MakeShadowCopy(source); !copy.empty())
	{
		path = copy;
	}
	else
	{
		std::fprintf(stderr, "[game] cannot copy '%ls' for hot reload; loading it in place\n", source.c_str());
	}
#endif
	// The module lives in the project, the engine (Core, third-party DLLs) beside the host: its
	// imports resolve from its own directory, then the application's.
	constexpr DWORD searchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
	HMODULE module = LoadLibraryExW(path.c_str(), nullptr, searchFlags);
#if FRT_WITH_GAME_HOT_RELOAD
	if (!module && path != source)
	{
		std::fprintf(stderr, "[game] cannot load the copy '%ls' (error %lu); loading the module in place\n", path.c_str(), GetLastError());
		std::error_code ec;
		std::filesystem::remove_all(path.parent_path(), ec);
		path = source;
		module = LoadLibraryExW(path.c_str(), nullptr, searchFlags);
	}
#endif
	if (!module)
	{
		std::fprintf(stderr, "[game] cannot load game module '%s' (%ls), error %lu\n", requested.c_str(), path.c_str(), GetLastError());
		return false;
	}

	using AbiFunction = unsigned int (*) ();
	const AbiFunction abi = reinterpret_cast<AbiFunction>(GetProcAddress(module, "FrtGameModuleAbi"));
	const auto create = reinterpret_cast<GameInstance* (*) ()>(GetProcAddress(module, "FrtCreateGame"));
	if (!abi || !create)
	{
		std::fprintf(stderr, "[game] '%ls' is not a game module (no FRT_IMPLEMENT_GAME_MODULE)\n", source.c_str());
		FreeLibrary(module);
		return false;
	}
	if (abi() != FRT_GAME_MODULE_ABI)
	{
		std::fprintf(stderr, "[game] '%ls' was built for another engine ABI (0x%08x, expected 0x%08x): rebuild it\n",
			source.c_str(), abi(), FRT_GAME_MODULE_ABI);
		FreeLibrary(module);
		return false;
	}

	Handle = module;
	CreateFunction = create;
	Name = requested;
#if FRT_WITH_GAME_HOT_RELOAD
	SourcePath = source.wstring();
	LoadedPath = path.wstring();
	SourceWriteTime = sourceWriteTime;
#endif
	std::printf("[game] module '%s' loaded\n", Name.c_str());
	return true;
#else
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
#if FRT_WITH_GAME_HOT_RELOAD
	if (!LoadedPath.empty() && LoadedPath != SourcePath)
	{
		// May fail while a debugger still holds the PDB; the next Load removes it.
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::path(LoadedPath).parent_path(), ec);
	}
#endif
	Handle = nullptr;
	CreateFunction = nullptr;
	Name.clear();
	SourcePath.clear();
	LoadedPath.clear();
	SourceWriteTime = 0;
}

bool CGameModule::HasNewerBuild () const
{
#if FRT_WITH_GAME_HOT_RELOAD && defined(_WINDOWS)
	if (!Handle || SourcePath.empty())
	{
		return false;
	}
	const std::filesystem::path source(SourcePath);
	const int64 writeTime = GetWriteTime(source);
	// No DLL at all (a clean, or a link that failed and deleted it) is nothing to reload yet.
	if (writeTime == 0 || writeTime == SourceWriteTime)
	{
		return false;
	}
	std::filesystem::path pdb = source;
	pdb.replace_extension(".pdb");
	return IsWriteFinished(source) && IsWriteFinished(pdb);
#else
	return false;
#endif
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
	return paths::HasProject() ? paths::GetProject().Module : std::string();
}
}
