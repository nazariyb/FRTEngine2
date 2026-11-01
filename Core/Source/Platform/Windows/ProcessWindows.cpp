#include "Platform/Process.h"

#include <atomic>
#include <csignal>
#include <cstdlib>

#include <crtdbg.h>

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
}
