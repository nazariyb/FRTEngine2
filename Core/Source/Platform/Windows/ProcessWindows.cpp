#include "Platform/Process.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <vector>

#include <crtdbg.h>

#include "Containers/WideString.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Windows/WindowsCommon.h"


namespace frt::platform
{
namespace
{
std::atomic<int32> gCrashExitCode = 3;

// One line on stderr, written straight to the handle: whatever crashed may have been inside the CRT
// (a heap or stdio lock held), and a stack overflow leaves little stack. No allocation, no CRT.
void WriteCrashLine (const char* What, uint32 Code) noexcept
{
	char line[128];
	uint32 length = 0u;
	const auto append = [&line, &length] (const char* Text)
	{
		while (*Text != '\0' && length < sizeof(line) - 1u)
		{
			line[length++] = *Text++;
		}
	};
	append("[crash] ");
	append(What);
	if (Code != 0u)
	{
		append(" 0x");
		for (int32 shift = 28; shift >= 0; shift -= 4)
		{
			line[length++] = "0123456789ABCDEF"[(Code >> static_cast<uint32>(shift)) & 0xFu];
		}
	}
	line[length++] = '\n';

	const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
	if (stderrHandle != nullptr && stderrHandle != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0u;
		WriteFile(stderrHandle, line, length, &written, nullptr);
	}
}

[[noreturn]] void EndProcess () noexcept
{
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(gCrashExitCode.load()));
	for (;;)
	{
		// TerminateProcess on the current process does not return.
	}
}

LONG WINAPI OnUnhandledException (EXCEPTION_POINTERS* Exception)
{
	const DWORD code = Exception != nullptr && Exception->ExceptionRecord != nullptr ? Exception->ExceptionRecord->ExceptionCode : 0u;
	WriteCrashLine("unhandled exception", code);
	EndProcess();
}

void OnAbort (int)
{
	WriteCrashLine("abort", 0u);
	EndProcess();
}

void OnInvalidParameter (const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
	WriteCrashLine("invalid parameter passed to a CRT function", 0u);
	EndProcess();
}


// ---------------- RunToolProcess ----------------

using windows::CScopedHandle;

constexpr DWORD ToolPipeBufferBytes = 64u * 1024u;

// How long a tool's output may stay open after the tool ended and its job was killed, and how long a killed tool may take
// to go. Only a process outside the job can hold the output that long (one that inherited the pipe's write end from another
// thread's CreateProcess in this process); its output is not this tool's, so the wait gives up rather than last as long as
// that process does. The job's handle closing on the way out is the last word on the tool.
constexpr DWORD ToolDrainGraceMs = 1000u;

std::atomic<uint32> gToolPipeCounter = 0u;

// One argument as CommandLineToArgvW reads it back: quoted, a quote inside escaped, the backslashes before a quote - the
// closing one too - doubled.
void AppendToolArgument (FString& Out, FStringView Argument)
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
		Out.Append(FString::Repeat("\\", c == '"' ? 2u * backslashes + 1u : backslashes));
		backslashes = 0u;
		Out.AppendChar(c);
	}
	Out.Append(FString::Repeat("\\", 2u * backslashes));
	Out.AppendChar('"');
}

/**
 * The read end of one of the tool's output streams. Overlapped, so that one thread can wait on both streams, the process
 * and the timeout together: with blocking reads each stream needs a thread of its own, and a stream some other process
 * holds open would keep that thread from ever being joined.
 */
class CToolPipe
{
public:
	// The pipe is named only because anonymous pipes cannot be overlapped; the name is this process's own and the pipe takes
	// one client.
	explicit CToolPipe (std::vector<uint8>& InBytes)
		: Bytes(InBytes)
		, Buffer(ToolPipeBufferBytes)
		, Name(FString::Format("\\\\.\\pipe\\FRT-Tool-{}-{}", ::GetCurrentProcessId(), ++gToolPipeCounter).View())
		, Read(CreateNamedPipeW(Name.CStr(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1u, 0u, ToolPipeBufferBytes, 0u, nullptr))
		, Event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
	{
		Overlapped.hEvent = Event.Get();
	}

	// The write end, inheritable, for the child's standard handles. INVALID_HANDLE_VALUE when it cannot be had.
	[[nodiscard]] HANDLE OpenWriteEnd ()
	{
		if (!Read.IsValid() || !Event.IsValid())
		{
			return INVALID_HANDLE_VALUE;
		}
		SECURITY_ATTRIBUTES security = {};
		security.nLength = sizeof(security);
		security.bInheritHandle = TRUE;
		return CreateFileW(Name.CStr(), GENERIC_WRITE, 0u, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	[[nodiscard]] bool IsOpen () const noexcept { return bOpen; }
	[[nodiscard]] HANDLE GetEvent () const noexcept { return Event.Get(); }

	// Reads until a read has to wait for the tool (the event signals it) or the stream ends.
	void StartRead ()
	{
		while (bOpen && !bPending)
		{
			DWORD got = 0u;
			if (ReadFile(Read.Get(), Buffer.data(), ToolPipeBufferBytes, &got, &Overlapped))
			{
				Append(got);
			}
			else if (GetLastError() == ERROR_IO_PENDING)
			{
				bPending = true;
			}
			else
			{
				bOpen = false;   // ERROR_BROKEN_PIPE: every write end is closed
			}
		}
	}

	// The event signalled.
	void FinishRead ()
	{
		DWORD got = 0u;
		bPending = false;
		if (GetOverlappedResult(Read.Get(), &Overlapped, &got, FALSE))
		{
			Append(got);
			StartRead();
		}
		else
		{
			bOpen = false;
		}
	}

	// Waiting ends: the read in progress writes into Buffer, so it has to be over before Buffer goes.
	void Abandon ()
	{
		if (bPending)
		{
			CancelIoEx(Read.Get(), &Overlapped);
			DWORD got = 0u;
			if (GetOverlappedResult(Read.Get(), &Overlapped, &got, TRUE))
			{
				Append(got);
			}
			bPending = false;
		}
		bOpen = false;
	}

private:
	void Append (DWORD Count)
	{
		Bytes.insert(Bytes.end(), Buffer.begin(), Buffer.begin() + Count);
	}

	std::vector<uint8>& Bytes;
	std::vector<uint8> Buffer;
	FWideString Name;
	CScopedHandle Read;
	CScopedHandle Event;
	OVERLAPPED Overlapped = {};
	bool bPending = false;
	bool bOpen = true;
};
}


uint32 GetCurrentProcessId () noexcept
{
	return static_cast<uint32>(::GetCurrentProcessId());
}

int64 GetProcessStartTime () noexcept
{
	static const int64 startTime = []
	{
		FILETIME creation = {};
		FILETIME exit = {};
		FILETIME kernel = {};
		FILETIME user = {};
		return GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) ? windows::ToFileTime(creation) : 0;
	}();
	return startTime;
}

void ExitOnCrash (int32 ExitCode) noexcept
{
	gCrashExitCode = ExitCode;
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	SetUnhandledExceptionFilter(&OnUnhandledException);
	_set_abort_behavior(0u, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	std::signal(SIGABRT, &OnAbort);
	_set_invalid_parameter_handler(&OnInvalidParameter);
#if defined(_DEBUG)
	// The debug CRT's assertion and error reports (a debug iterator's, a failed _ASSERTE) print
	// instead of asking Abort, Retry or Ignore. The release CRT has none.
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

SToolProcessResult RunToolProcess (const SToolProcessDesc& Desc)
{
	SToolProcessResult result;
	const auto fail = [&result, &Desc] (FStringView What, DWORD Error)
	{
		result.Stderr = FString::Format("{} '{}': {}", What, FString::FromPath(Desc.Executable), GetSystemErrorMessage(Error));
		return result;
	};

	std::vector<uint8> errorBytes;
	CToolPipe output(result.Stdout);
	CToolPipe errors(errorBytes);
	CScopedHandle outputWrite(output.OpenWriteEnd());
	CScopedHandle errorsWrite(errors.OpenWriteEnd());
	if (!outputWrite.IsValid() || !errorsWrite.IsValid())
	{
		return fail("Cannot create the pipes for", GetLastError());
	}
	// The null device for its input: a tool that asks a question reads the end of its input rather than waiting for ever.
	SECURITY_ATTRIBUTES security = {};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	const CScopedHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0u,
		nullptr));
	if (!input.IsValid())
	{
		return fail("Cannot open the null device for", GetLastError());
	}

	// Only these three reach the child. Inheriting by default would also hand it the write end of a pipe that another
	// worker's RunToolProcess has just made, and that tool's reader would then wait for this one to end.
	HANDLE inherited[] = { input.Get(), outputWrite.Get(), errorsWrite.Get() };
	SIZE_T attributeBytes = 0u;
	InitializeProcThreadAttributeList(nullptr, 1u, 0u, &attributeBytes);
	std::vector<uint8> attributeStorage(attributeBytes);
	const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	if (!InitializeProcThreadAttributeList(attributes, 1u, 0u, &attributeBytes))
	{
		return fail("Cannot prepare the handles for", GetLastError());
	}
	const bool bAttributes = UpdateProcThreadAttribute(attributes, 0u, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
		nullptr, nullptr) != 0;

	FString commandLine;
	AppendToolArgument(commandLine, FString::FromPath(Desc.Executable));
	for (const FString& argument : Desc.Arguments)
	{
		commandLine.AppendChar(' ');
		AppendToolArgument(commandLine, argument);
	}
	FWideString wideCommandLine(commandLine.View());

	STARTUPINFOEXW startup = {};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = input.Get();
	startup.StartupInfo.hStdOutput = outputWrite.Get();
	startup.StartupInfo.hStdError = errorsWrite.Get();
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION process = {};
	const bool bStarted = bAttributes && CreateProcessW(Desc.Executable.c_str(), wideCommandLine.GetData(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
		Desc.WorkingDir.empty() ? nullptr : Desc.WorkingDir.c_str(), &startup.StartupInfo, &process) != 0;
	const DWORD startError = GetLastError();
	DeleteProcThreadAttributeList(attributes);
	if (!bStarted)
	{
		return fail("Cannot start", startError);
	}
	result.bStarted = true;
	const CScopedHandle processHandle(process.hProcess);
	const CScopedHandle threadHandle(process.hThread);

	// In its job before it runs, so that nothing it starts escapes. Without a job (the OS refused) only the tool itself can
	// be killed, and the drain's grace covers what it left holding the pipes.
	const CScopedHandle job(CreateJobObjectW(nullptr, nullptr));
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	const bool bInJob = job.IsValid()
		&& SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))
		&& AssignProcessToJobObject(job.Get(), processHandle.Get());
	ResumeThread(threadHandle.Get());

	// The child holds them now. Ours would keep the streams from ever ending.
	outputWrite.Reset();
	errorsWrite.Reset();
	output.StartRead();
	errors.StartRead();

	const auto kill = [&]
	{
		if (bInJob)
		{
			TerminateJobObject(job.Get(), 1u);
		}
		else
		{
			TerminateProcess(processHandle.Get(), 1u);
		}
	};

	const uint64 startTick = GetTickCount64();
	uint64 drainStart = 0u;
	bool bRunning = true;
	while (bRunning || output.IsOpen() || errors.IsOpen())
	{
		HANDLE waits[3];
		CToolPipe* pipes[3] = {};
		DWORD waitCount = 0u;
		for (CToolPipe* pipe : { &output, &errors })
		{
			if (pipe->IsOpen())
			{
				pipes[waitCount] = pipe;
				waits[waitCount++] = pipe->GetEvent();
			}
		}
		if (bRunning)
		{
			waits[waitCount++] = processHandle.Get();
		}

		// Running: until the timeout. Ended or killed: the grace.
		const bool bDraining = !bRunning || result.bTimedOut;
		const uint64 waited = GetTickCount64() - (bDraining ? drainStart : startTick);
		const uint64 limit = bDraining ? ToolDrainGraceMs : Desc.TimeoutMs;
		const DWORD waitMs = waited < limit ? static_cast<DWORD>(limit - waited) : 0u;
		const DWORD signalled = WaitForMultipleObjects(waitCount, waits, FALSE, waitMs);
		if (signalled == WAIT_TIMEOUT)
		{
			if (bDraining)
			{
				break;
			}
			result.bTimedOut = true;
			drainStart = GetTickCount64();
			kill();
		}
		else if (signalled >= WAIT_OBJECT_0 && signalled < WAIT_OBJECT_0 + waitCount)
		{
			const DWORD index = signalled - WAIT_OBJECT_0;
			if (pipes[index] != nullptr)
			{
				pipes[index]->FinishRead();
			}
			else
			{
				// What it left behind goes with it: a process of its still holding the streams would hold up this wait.
				bRunning = false;
				drainStart = GetTickCount64();
				kill();
			}
		}
		else
		{
			// WAIT_FAILED: not something waiting longer mends.
			kill();
			break;
		}
	}
	output.Abandon();
	errors.Abandon();

	DWORD exitCode = 0u;
	if (!result.bTimedOut && GetExitCodeProcess(processHandle.Get(), &exitCode))
	{
		result.ExitCode = static_cast<int32>(exitCode);
	}
	result.Stderr = FString::FromUtf8Lossy(FStringView(reinterpret_cast<const char*>(errorBytes.data()),
		static_cast<uint32>(errorBytes.size())));
	return result;
}

std::filesystem::path FindOnPath (FStringView Name)
{
	if (Name.IsEmpty() || Name.Contains('/') || Name.Contains('\\') || Name.Contains(':'))
	{
		return {};
	}
	const std::optional<FString> path = GetEnvironmentValue("PATH");
	if (!path.has_value())
	{
		return {};
	}
	// A bare name is an .exe: nothing else starts without a shell.
	const FString file = Name.EndsWith<ESearchCase::IgnoreCaseAscii>(".exe") ? FString(Name) : FString(Name) + ".exe";
	for (const FStringView entry : path->View().Split(';', EStringSplit::SkipEmpty | EStringSplit::Trim))
	{
		// A relative entry ("." is the classic) is the working directory's search this function exists to avoid.
		const std::filesystem::path directory = entry.TrimQuotes().ToPath();
		if (!directory.is_absolute())
		{
			continue;
		}
		const std::filesystem::path candidate = directory / file.ToPath();
		if (const std::optional<SFileStat> stat = Stat(candidate); stat.has_value() && !stat->bDirectory)
		{
			return candidate;
		}
	}
	return {};
}
}
