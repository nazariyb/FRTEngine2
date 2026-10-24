#include "TestProcess.h"

#include <atomic>
#include <optional>
#include <system_error>

#include "Containers/WideString.h"
#include "Platform/Platform.h"

#include <Windows.h>


namespace frt::test
{
namespace
{
std::atomic<uint32> gProcessCounter = 0u;

// One argument as CommandLineToArgvW reads it back: quoted, a quote inside escaped, the backslashes
// before a quote - the closing one too - doubled.
void AppendArgument (FString& Out, FStringView Argument)
{
	Out.AppendChar('"');
	uint32 backslashes = 0u;
	for (const char c : Argument)
	{
		if (c == '\\')
		{
			++backslashes;
			continue;
		}
		if (c == '"')
		{
			Out.Append(FString::Repeat("\\", 2u * backslashes + 1u));
		}
		else
		{
			Out.Append(FString::Repeat("\\", backslashes));
		}
		backslashes = 0u;
		Out.AppendChar(c);
	}
	Out.Append(FString::Repeat("\\", 2u * backslashes));
	Out.AppendChar('"');
}

// Where the children's output files go: a directory of this process's own with a non-ASCII name.
std::filesystem::path GetOutputDir ()
{
	return std::filesystem::temp_directory_path() / FString::Format("FRT Core-Test процеси 进程 {}", ::GetCurrentProcessId()).ToPath();
}

HANDLE CreateInheritableFile (const std::filesystem::path& Path)
{
	SECURITY_ATTRIBUTES security = {};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	return CreateFileW(Path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
}

FString ReadFileOrEmpty (const std::filesystem::path& Path)
{
	return Path.empty() ? FString() : platform::ReadFileText(Path, ETextFileEncoding::Raw).value_or(FString());
}
}


CTestProcess::~CTestProcess ()
{
	Close();
	std::error_code ec;
	if (!OutputFile.empty())
	{
		std::filesystem::remove(OutputFile, ec);
		std::filesystem::remove(ErrorFile, ec);
		std::filesystem::remove(OutputFile.parent_path(), ec);   // only once it is empty
	}
}

bool CTestProcess::Start (const std::filesystem::path& Executable, std::span<const FString> Arguments, bool bKeepInputOpen,
	const std::filesystem::path& WorkingDir, FString* OutError)
{
	const auto fail = [OutError] (FStringView What)
	{
		if (OutError != nullptr)
		{
			*OutError = FString::Format("{}: {}", What, platform::GetSystemErrorMessage(GetLastError()));
		}
		return false;
	};

	const std::filesystem::path dir = GetOutputDir();
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const uint32 number = ++gProcessCounter;
	OutputFile = dir / FString::Format("{}.out", number).ToPath();
	ErrorFile = dir / FString::Format("{}.err", number).ToPath();

	const HANDLE output = CreateInheritableFile(OutputFile);
	const HANDLE errors = CreateInheritableFile(ErrorFile);
	SECURITY_ATTRIBUTES security = {};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	HANDLE inputRead = nullptr;
	HANDLE inputWrite = nullptr;
	const bool bPipe = CreatePipe(&inputRead, &inputWrite, &security, 0u) != 0;
	if (output == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE || !bPipe)
	{
		const bool bResult = fail("cannot create the child's standard handles");
		for (const HANDLE handle : { output, errors, inputRead, inputWrite })
		{
			if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle);
			}
		}
		return bResult;
	}
	// The child inherits the read end only.
	SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0u);

	FString commandLine;
	AppendArgument(commandLine, FString::FromPath(Executable));
	for (const FString& argument : Arguments)
	{
		commandLine.AppendChar(' ');
		AppendArgument(commandLine, argument);
	}
	FWideString wideCommandLine(commandLine.View());

	Job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (Job != nullptr)
	{
		SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = inputRead;
	startup.hStdOutput = output;
	startup.hStdError = errors;
	PROCESS_INFORMATION process = {};
	const bool bStarted = CreateProcessW(Executable.c_str(), wideCommandLine.GetData(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, WorkingDir.empty() ? nullptr : WorkingDir.c_str(), &startup, &process) != 0;
	const bool bResult = bStarted || fail(FString::Format("cannot start {}", FString::FromPath(Executable)));
	CloseHandle(output);
	CloseHandle(errors);
	CloseHandle(inputRead);
	if (!bStarted)
	{
		CloseHandle(inputWrite);
		return bResult;
	}

	if (Job != nullptr)
	{
		AssignProcessToJobObject(Job, process.hProcess);
	}
	ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	Process = process.hProcess;
	ProcessId = process.dwProcessId;
	InputWrite = inputWrite;
	if (!bKeepInputOpen)
	{
		CloseInput();
	}
	return true;
}

void CTestProcess::CloseInput ()
{
	if (InputWrite != nullptr)
	{
		CloseHandle(InputWrite);
		InputWrite = nullptr;
	}
}

bool CTestProcess::IsRunning () const
{
	return Process != nullptr && WaitForSingleObject(Process, 0u) == WAIT_TIMEOUT;
}

void CTestProcess::Kill ()
{
	if (Process != nullptr)
	{
		TerminateProcess(Process, 9u);
		WaitForSingleObject(Process, 10'000u);
	}
}

int32 CTestProcess::Wait (std::chrono::milliseconds Timeout)
{
	if (Process == nullptr || WaitForSingleObject(Process, static_cast<DWORD>(Timeout.count())) != WAIT_OBJECT_0)
	{
		return -1;
	}
	DWORD code = 0u;
	return GetExitCodeProcess(Process, &code) ? static_cast<int32>(code) : -1;
}

FString CTestProcess::ReadOutput () const
{
	return ReadFileOrEmpty(OutputFile);
}

FString CTestProcess::ReadErrors () const
{
	return ReadFileOrEmpty(ErrorFile);
}

void CTestProcess::Close ()
{
	CloseInput();
	if (Process != nullptr)
	{
		// Ended before its output files are deleted: a running child still has them open.
		if (IsRunning())
		{
			Kill();
		}
		CloseHandle(Process);
		Process = nullptr;
	}
	if (Job != nullptr)
	{
		CloseHandle(Job);   // kills the child if it still runs
		Job = nullptr;
	}
}


SProcessResult RunProcess (const std::filesystem::path& Executable, std::span<const FString> Arguments,
	const std::filesystem::path& WorkingDir, std::chrono::milliseconds Timeout)
{
	SProcessResult result;
	CTestProcess process;
	if (!process.Start(Executable, Arguments, false, WorkingDir, &result.Errors))
	{
		return result;
	}
	result.ExitCode = process.Wait(Timeout);
	if (result.ExitCode == -1)
	{
		process.Kill();
	}
	result.Output = process.ReadOutput();
	result.Errors = process.ReadErrors();
	return result;
}
}
