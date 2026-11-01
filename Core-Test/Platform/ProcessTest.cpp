// The running process as the OS knows it (Platform/Process.h): its id and start time - what names the
// holder of a writer lock - and the crash handling of console tools: a crash, an abort or an invalid CRT
// parameter in a process that called ExitOnCrash ends it with the code it asked for, after one line on
// stderr, never with a dialog. The crashes run in Core-Test child processes.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <chrono>
#include <cstdlib>
#include <span>

#include <io.h>

#include "Containers/String.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

#include <Windows.h>

using namespace frt;


namespace
{
// "Core-Test --frt-test-child=crash <how> <exit code>": calls ExitOnCrash(<exit code>), then crashes.
int RunCrash (std::span<const FString> Arguments)
{
	if (Arguments.size() != 2u)
	{
		return 2;
	}
	platform::ExitOnCrash(Arguments[1].View().ToInt32().value_or(99));
	if (Arguments[0] == "access-violation")
	{
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0u, 0u, nullptr);
	}
	else if (Arguments[0] == "abort")
	{
		std::abort();
	}
	else if (Arguments[0] == "invalid-parameter")
	{
		// A descriptor past every open one: the CRT calls its invalid parameter handler. (-2 would not:
		// it is _NO_CONSOLE_FILENO, which the CRT reports through errno alone.)
		(void)_get_osfhandle(100'000);
	}
	return 0;   // survived: the parent sees 0, not the code it asked for
}

const bool bCrashRegistered = test::RegisterChildMode("crash", &RunCrash);

test::SProcessResult RunCrashChild (FStringView How, int32 ExitCode)
{
	const FString arguments[] = { FString(test::ChildModeSwitch) + "crash", FString(How), FString::Format("{}", ExitCode) };
	return test::RunProcess(platform::GetExecutablePath(), arguments);
}

// FILETIME ticks of now: what GetProcessStartTime must not be after.
int64 GetNowFileTime ()
{
	FILETIME now = {};
	GetSystemTimeAsFileTime(&now);
	return static_cast<int64>((static_cast<uint64>(now.dwHighDateTime) << 32u) | now.dwLowDateTime);
}
}


TEST(ProcessTest, IdAndStartTimeAreThisProcesses)
{
	EXPECT_EQ(platform::GetCurrentProcessId(), static_cast<uint32>(::GetCurrentProcessId()));
	EXPECT_NE(platform::GetCurrentProcessId(), 0u);

	FILETIME creation = {};
	FILETIME exit = {};
	FILETIME kernel = {};
	FILETIME user = {};
	ASSERT_TRUE(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user));
	const int64 expected = static_cast<int64>((static_cast<uint64>(creation.dwHighDateTime) << 32u) | creation.dwLowDateTime);
	EXPECT_EQ(platform::GetProcessStartTime(), expected);
	EXPECT_EQ(platform::GetProcessStartTime(), platform::GetProcessStartTime());

	// Some time in the last day, never in the future.
	constexpr int64 TicksPerDay = 24ll * 3600ll * 10'000'000ll;
	EXPECT_LE(platform::GetProcessStartTime(), GetNowFileTime());
	EXPECT_GT(platform::GetProcessStartTime(), GetNowFileTime() - TicksPerDay);
}

TEST(ProcessTest, ACrashEndsTheProcessWithTheCodeItAskedFor)
{
	const test::SProcessResult crashed = RunCrashChild("access-violation", 42);
	EXPECT_EQ(crashed.ExitCode, 42) << crashed.Errors.CStr();
	EXPECT_TRUE(crashed.Errors.Contains("[crash] unhandled exception 0xC0000005")) << crashed.Errors.CStr();

	const test::SProcessResult aborted = RunCrashChild("abort", 43);
	EXPECT_EQ(aborted.ExitCode, 43) << aborted.Errors.CStr();
	EXPECT_TRUE(aborted.Errors.Contains("[crash] abort")) << aborted.Errors.CStr();

	const test::SProcessResult invalid = RunCrashChild("invalid-parameter", 44);
	EXPECT_EQ(invalid.ExitCode, 44) << invalid.Errors.CStr();
	EXPECT_TRUE(invalid.Errors.Contains("[crash] invalid parameter")) << invalid.Errors.CStr();

	// Nothing to catch: the child returns normally.
	EXPECT_EQ(RunCrashChild("nothing", 45).ExitCode, 0);
}
