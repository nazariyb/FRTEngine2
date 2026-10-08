#include "ToolProcess.h"

#include <vector>

#include <Windows.h>

#include "EnginePaths.h"


namespace frt::editor
{
CToolProcess::~CToolProcess ()
{
	Cancel();
	Close();
}

bool CToolProcess::Start (const std::wstring& CommandLine, const std::filesystem::path& WorkingDir)
{
	if (IsRunning())
	{
		return false;
	}
	Close();
	{
		std::lock_guard lock(OutputMutex);
		Output.clear();
		ExitCode.reset();
	}

	const auto fail = [this] (const char* What)
	{
		std::lock_guard lock(OutputMutex);
		Output += What;
		Output += " (error " + std::to_string(GetLastError()) + ")\n";
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

	// cmd.exe /s /c "<command line>": runs batch files, and keeps the command's own quotes.
	std::wstring command = L"cmd.exe /d /s /c \"" + CommandLine + L"\"";
	PROCESS_INFORMATION process {};
	const std::wstring dir = WorkingDir.wstring();
	const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.empty() ? nullptr : dir.c_str(), &startup, &process);
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
		std::lock_guard lock(OutputMutex);
		for (DWORD i = 0u; i < count; ++i)
		{
			if (buffer[i] != '\r')
			{
				Output += buffer[i];
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

std::string CToolProcess::GetOutput () const
{
	std::lock_guard lock(OutputMutex);
	return Output;
}

bool StartDetached (const std::filesystem::path& Executable, const std::wstring& Arguments, const std::filesystem::path& WorkingDir)
{
	std::wstring command = QuoteArgument(Executable);
	if (!Arguments.empty())
	{
		command += L" " + Arguments;
	}
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	const std::wstring dir = WorkingDir.wstring();
	if (!CreateProcessW(Executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0u, nullptr,
		dir.empty() ? nullptr : dir.c_str(), &startup, &process))
	{
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

int RunAndWait (const std::filesystem::path& Executable, const std::wstring& Arguments, const std::filesystem::path& WorkingDir)
{
	std::wstring command = QuoteArgument(Executable) + L" " + Arguments;
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	const std::wstring dir = WorkingDir.wstring();
	if (!CreateProcessW(Executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0u, nullptr,
		dir.empty() ? nullptr : dir.c_str(), &startup, &process))
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

bool StartCommand (const std::wstring& CommandLine, const std::filesystem::path& WorkingDir, bool bConsole)
{
	std::wstring command = L"cmd.exe /d /s /c \"" + CommandLine + L"\"";
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	const std::wstring dir = WorkingDir.wstring();
	if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, bConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
		nullptr, dir.empty() ? nullptr : dir.c_str(), &startup, &process))
	{
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

bool StartEngineTool (const std::wstring& Arguments)
{
	return StartCommand(QuoteArgument(paths::GetEngineDir() / "FRT.bat") + L" " + Arguments, paths::GetEngineDir(), false);
}

std::filesystem::path GetExecutablePath ()
{
	std::vector<wchar_t> buffer(MAX_PATH);
	while (true)
	{
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0u)
		{
			return {};
		}
		if (length < buffer.size())
		{
			return std::filesystem::path(std::wstring(buffer.data(), length));
		}
		buffer.resize(buffer.size() * 2u);
	}
}

std::wstring QuoteArgument (const std::filesystem::path& Path)
{
	// A path never ends in a backslash here, nor holds a quote, so plain quotes are enough.
	return L"\"" + Path.wstring() + L"\"";
}
}
