#include "ToolProcess.h"

#include <Windows.h>

#include "EnginePaths.h"
#include "Containers/WideString.h"
#include "Platform/Platform.h"


namespace frt::editor
{
CToolProcess::~CToolProcess ()
{
	Cancel();
	Close();
}

bool CToolProcess::Start (FStringView CommandLine, const std::filesystem::path& WorkingDir)
{
	if (IsRunning())
	{
		return false;
	}
	Close();
	{
		std::lock_guard lock(OutputMutex);
		Output.Clear();
		ExitCode.reset();
	}

	const auto fail = [this] (const char* What)
	{
		const DWORD error = GetLastError();
		std::lock_guard lock(OutputMutex);
		Output.Append(What).Append(" (error ").AppendNumber(error).Append(")\n");
		ExitCode = -1;
		return false;
	};

	// The child writes its output and errors into one pipe; reads from NUL.
	SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
	HANDLE read = nullptr;
	HANDLE write = nullptr;
	if (!CreatePipe(&read, &write, &inheritable, 0u))
	{
		return fail("cannot create a pipe");
	}
	SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0u);
	HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0u, nullptr);

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input;
	startup.hStdOutput = write;
	startup.hStdError = write;

	// MSBuild's worker nodes would otherwise stay behind after a build, holding the pipe.
	SetEnvironmentVariableW(L"MSBUILDDISABLENODEREUSE", L"1");

	// cmd.exe /s /c "<command line>": runs batch files, and keeps the command's own quotes. Its
	// console is switched to UTF-8 first, so the tools that write in the console's code page
	// (MSBuild, the compiler) write UTF-8; a line in anything else is decoded by GetOutput.
	FWideString command = ToWide(FString::Format("cmd.exe /d /s /c \"chcp 65001>nul & {}\"", CommandLine));
	PROCESS_INFORMATION process {};
	const BOOL started = CreateProcessW(nullptr, command.GetData(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, WorkingDir.empty() ? nullptr : WorkingDir.c_str(), &startup, &process);
	CloseHandle(write);
	if (input != INVALID_HANDLE_VALUE)
	{
		CloseHandle(input);
	}
	if (!started)
	{
		CloseHandle(read);
		return fail("cannot start the command");
	}

	// Everything it starts goes with it.
	Job = CreateJobObjectW(nullptr, nullptr);
	if (Job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
		AssignProcessToJobObject(Job, process.hProcess);
	}
	ResumeThread(process.hThread);
	CloseHandle(process.hThread);

	Process = process.hProcess;
	OutputRead = read;
	Reader = std::thread([this] { ReadOutput(); });
	return true;
}

void CToolProcess::ReadOutput ()
{
	char buffer[4096];
	const auto readAvailable = [&] ()
	{
		DWORD available = 0u;
		DWORD count = 0u;
		if (!PeekNamedPipe(OutputRead, nullptr, 0u, nullptr, &available, nullptr) || available == 0u ||
			!ReadFile(OutputRead, buffer, available < sizeof(buffer) ? available : sizeof(buffer), &count, nullptr))
		{
			return false;
		}
		// Bytes, whatever they encode: a chunk may end inside a character.
		std::lock_guard lock(OutputMutex);
		for (DWORD i = 0u; i < count; ++i)
		{
			if (buffer[i] != '\r')
			{
				Output.AppendChar(buffer[i]);
			}
		}
		return true;
	};
	// Until the command exits - not until the pipe closes: a process it started may outlive it
	// holding the pipe (a build server), and must not keep the command running here.
	while (true)
	{
		if (readAvailable())
		{
			continue;
		}
		if (WaitForSingleObject(Process, 20u) != WAIT_TIMEOUT)
		{
			while (readAvailable())
			{
			}
			break;
		}
	}
	DWORD code = 1u;
	GetExitCodeProcess(Process, &code);
	std::lock_guard lock(OutputMutex);
	ExitCode = static_cast<int>(code);
}

void CToolProcess::Cancel ()
{
	if (Job)
	{
		TerminateJobObject(Job, 1u);
	}
	else if (Process)
	{
		TerminateProcess(Process, 1u);
	}
}

void CToolProcess::Close ()
{
	if (Reader.joinable())
	{
		Reader.join();
	}
	for (void** handle : { &Process, &Job, &OutputRead })
	{
		if (*handle)
		{
			CloseHandle(*handle);
			*handle = nullptr;
		}
	}
}

bool CToolProcess::IsRunning () const
{
	std::lock_guard lock(OutputMutex);
	return Process != nullptr && !ExitCode.has_value();
}

std::optional<int> CToolProcess::GetExitCode () const
{
	std::lock_guard lock(OutputMutex);
	return ExitCode;
}

FString CToolProcess::GetOutput () const
{
	std::lock_guard lock(OutputMutex);
	// All of it at once, never per read: a character split between two reads would otherwise
	// come out as two replacement characters.
	return platform::DecodeToolOutput(Output);
}

bool StartDetached (const std::filesystem::path& Executable, FStringView Arguments, const std::filesystem::path& WorkingDir)
{
	FString commandLine = QuoteArgument(Executable);
	if (!Arguments.IsEmpty())
	{
		commandLine << ' ' << Arguments;
	}
	FWideString command = ToWide(commandLine);
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(Executable.c_str(), command.GetData(), nullptr, nullptr, FALSE, 0u, nullptr,
		WorkingDir.empty() ? nullptr : WorkingDir.c_str(), &startup, &process))
	{
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

int RunAndWait (const std::filesystem::path& Executable, FStringView Arguments, const std::filesystem::path& WorkingDir)
{
	FWideString command = ToWide(QuoteArgument(Executable) + " " + Arguments);
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(Executable.c_str(), command.GetData(), nullptr, nullptr, FALSE, 0u, nullptr,
		WorkingDir.empty() ? nullptr : WorkingDir.c_str(), &startup, &process))
	{
		return -1;
	}
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = 1u;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return static_cast<int>(code);
}

bool StartCommand (FStringView CommandLine, const std::filesystem::path& WorkingDir, bool bConsole)
{
	FWideString command = ToWide(FString::Format("cmd.exe /d /s /c \"{}\"", CommandLine));
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(nullptr, command.GetData(), nullptr, nullptr, FALSE, bConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
		nullptr, WorkingDir.empty() ? nullptr : WorkingDir.c_str(), &startup, &process))
	{
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

bool StartEngineTool (FStringView Arguments)
{
	return StartCommand(QuoteArgument(paths::GetEngineDir() / "FRT.bat") + " " + Arguments, paths::GetEngineDir(), false);
}

FString QuoteArgument (const std::filesystem::path& Path)
{
	// Paths hold no quotes. Backslashes before the closing quote are doubled, or the program
	// reading the argument would take the last one as escaping the quote (a drive root, D:\).
	const FString text = FString::FromPath(Path, EPathStyle::Preferred);
	const uint32 trailing = text.Len() - text.View().TrimEnd("\\").Len();
	FString quoted;
	quoted.Reserve(text.Len() + trailing + 2u);
	quoted << '"' << text;
	quoted.AppendChars('\\', trailing);
	quoted << '"';
	return quoted;
}
}
