#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::editor
{
/**
 * A command run in the background with its output collected: the build tools the project
 * manager runs (FRT.bat new, build), whose log it shows as they go. The command and everything
 * it starts (MSBuild, the compiler) run in one job: Cancel, or destroying this, ends them all.
 */
class CToolProcess
{
public:
	CToolProcess () = default;
	~CToolProcess ();
	CToolProcess (const CToolProcess&) = delete;
	CToolProcess& operator= (const CToolProcess&) = delete;

	// Runs CommandLine through cmd.exe in WorkingDir. False, with the reason in the output, if
	// it could not start; also when one is still running.
	bool Start (FStringView CommandLine, const std::filesystem::path& WorkingDir);
	void Cancel ();

	bool IsRunning () const;
	// Once it has finished (and all its output is in).
	std::optional<int> GetExitCode () const;
	// Everything it printed so far, as UTF-8 whatever code page each tool wrote in. A copy of what is
	// already decoded: cheap enough to ask every frame.
	FString GetOutput () const;

private:
	void ReadOutput ();
	void Close ();

	void* Process = nullptr;
	void* Job = nullptr;
	void* OutputRead = nullptr;
	std::thread Reader;
	mutable std::mutex OutputMutex;
	FString Output;                      // every complete line, decoded once as it arrived (UTF-8)
	FString PendingLine;                 // the bytes after the last '\n', decoded once their line is complete
	std::optional<int> ExitCode;
};

// Starts an executable on its own (the editor for another project), not waiting for it.
bool StartDetached (const std::filesystem::path& Executable, FStringView Arguments, const std::filesystem::path& WorkingDir);
// Runs an executable and waits for it to exit: its exit code, or -1 if it could not start.
int RunAndWait (const std::filesystem::path& Executable, FStringView Arguments, const std::filesystem::path& WorkingDir);
// Runs a command line through cmd.exe on its own, in a console of its own (bConsole) or with no
// window at all.
bool StartCommand (FStringView CommandLine, const std::filesystem::path& WorkingDir, bool bConsole);
// Runs the engine's FRT.bat with Arguments on its own, without a console window: a command whose
// result shows elsewhere (FRT.bat open).
bool StartEngineTool (FStringView Arguments);

// A command-line argument for a path, quoted.
FString QuoteArgument (const std::filesystem::path& Path);
}
