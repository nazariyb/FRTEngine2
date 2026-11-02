// The running process as the OS knows it (Platform/Process.h): its id and start time - what names the
// holder of a writer lock - and the crash handling of console tools: a crash, an abort or an invalid CRT
// parameter in a process that called ExitOnCrash ends it with the code it asked for, after one line on
// stderr, never with a dialog. The crashes run in Core-Test child processes.
//
// Also a tool run to its end: its arguments arrive as given, both its streams are captured however much
// it writes (a full pipe would block it), a timeout ends it, nothing it leaves behind can hold the call
// up, and it dies with the process that started it, so a killed host leaves no git holding index.lock.
// FindOnPath looks on PATH alone, never in the working directory.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include <io.h>

#include "Containers/String.h"
#include "Containers/WideString.h"
#include "Platform/FileSystem.h"
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

// Straight to the handle: the CRT's stdout turns \n into \r\n, and the tests compare bytes.
void WriteTo (DWORD Stream, FStringView Text)
{
	DWORD written = 0u;
	WriteFile(GetStdHandle(Stream), Text.GetData(), Text.Len(), &written, nullptr);
}

// "tool-echo <exit code> <arguments>...": each argument on a line, the working directory last.
int RunToolEcho (std::span<const FString> Arguments)
{
	for (const FString& argument : Arguments.subspan(1u))
	{
		WriteTo(STD_OUTPUT_HANDLE, argument);
		WriteTo(STD_OUTPUT_HANDLE, "\n");
	}
	WriteTo(STD_OUTPUT_HANDLE, FString::FromPath(std::filesystem::current_path()));
	WriteTo(STD_ERROR_HANDLE, "на stderr");
	return Arguments[0].View().ToInt32().value_or(99);
}
const bool bToolEchoRegistered = test::RegisterChildMode("tool-echo", &RunToolEcho);

// "tool-noisy <bytes>": that many bytes on each stream, in turns, far past what a pipe buffers.
int RunToolNoisy (std::span<const FString> Arguments)
{
	const uint32 total = Arguments[0].View().ToInt32().value_or(0);
	const FString chunk = FString::Repeat("x", 4096u);
	for (uint32 written = 0u; written < total; written += chunk.Len())
	{
		WriteTo(STD_OUTPUT_HANDLE, chunk);
		WriteTo(STD_ERROR_HANDLE, chunk);
	}
	return 0;
}
const bool bToolNoisyRegistered = test::RegisterChildMode("tool-noisy", &RunToolNoisy);

// "tool-sleep <pid file>": says who it is, then outlives any test that does not end it.
int RunToolSleep (std::span<const FString> Arguments)
{
	platform::WriteFileAtomic(Arguments[0].ToPath(), FString::Format("{}", ::GetCurrentProcessId()).View().AsBytes(), false);
	std::this_thread::sleep_for(std::chrono::seconds(120));
	return 0;
}
const bool bToolSleepRegistered = test::RegisterChildMode("tool-sleep", &RunToolSleep);

std::vector<FString> SleepArguments (const std::filesystem::path& PidFile)
{
	return { FString(test::ChildModeSwitch) + "tool-sleep", FString::FromPath(PidFile) };
}

// "tool-leaver <pid file>": starts a tool-sleep that inherits its standard output, and ends: what a tool that leaves a
// daemon behind does, which keeps the pipe open with nobody to close it.
int RunToolLeaver (std::span<const FString> Arguments)
{
	SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	FString commandLine = FString::Format("\"{}\" {}tool-sleep \"{}\"", FString::FromPath(platform::GetExecutablePath()),
		FStringView(test::ChildModeSwitch), Arguments[0]);
	FWideString wide(commandLine.View());
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(nullptr, wide.GetData(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
	{
		return 3;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	test::WaitUntil([&] { return platform::Stat(Arguments[0].ToPath()).has_value(); });
	return 0;
}
const bool bToolLeaverRegistered = test::RegisterChildMode("tool-leaver", &RunToolLeaver);

// "tool-host <pid file>": runs a tool-sleep through RunToolProcess, for a test to kill.
int RunToolHost (std::span<const FString> Arguments)
{
	platform::SToolProcessDesc desc;
	desc.Executable = platform::GetExecutablePath();
	desc.Arguments = SleepArguments(Arguments[0].ToPath());
	desc.TimeoutMs = 120000u;
	return platform::RunToolProcess(desc).ExitCode;
}
const bool bToolHostRegistered = test::RegisterChildMode("tool-host", &RunToolHost);

// The process named in PidFile ends within a few seconds. A process that cannot be opened has ended, and one that never
// said who it is did not get that far.
bool HasEnded (const std::filesystem::path& PidFile)
{
	const std::optional<FString> pid = platform::ReadFileText(PidFile);
	if (!pid.has_value())
	{
		return true;
	}
	const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid->View().ToInt32().value_or(0)));
	if (process == nullptr)
	{
		return true;
	}
	const bool bEnded = WaitForSingleObject(process, 10000u) == WAIT_OBJECT_0;
	CloseHandle(process);
	return bEnded;
}

// A directory of this test's own with a non-ASCII name, gone afterwards.
class ToolProcessTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Інструмент 工具 {}", ::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec));
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
	}

	std::filesystem::path Dir;
};

// PATH for as long as it exists.
class CScopedPath
{
public:
	explicit CScopedPath (FStringView Value)
		: Saved(platform::GetEnvironmentValue("PATH").value_or(FString()))
	{
		platform::SetEnvironmentValue("PATH", Value);
	}

	~CScopedPath ()
	{
		platform::SetEnvironmentValue("PATH", Saved);
	}

private:
	FString Saved;
};

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

TEST_F(ToolProcessTest, ArgumentsArriveAsGivenAndTheResultComesBack)
{
	platform::SToolProcessDesc desc;
	desc.Executable = platform::GetExecutablePath();
	desc.WorkingDir = Dir;
	desc.Arguments = { FString(test::ChildModeSwitch) + "tool-echo", "7", "прості слова 词", "ends with \\", "a \"quoted\" one", "", "back\\slash\\\"" };
	const platform::SToolProcessResult run = platform::RunToolProcess(desc);

	EXPECT_TRUE(run.bStarted);
	EXPECT_FALSE(run.bTimedOut);
	EXPECT_EQ(run.ExitCode, 7);
	const FString expected = FString::Format("прості слова 词\nends with \\\na \"quoted\" one\n\nback\\slash\\\"\n{}", FString::FromPath(Dir));
	EXPECT_EQ(FStringView(reinterpret_cast<const char*>(run.Stdout.data()), static_cast<uint32>(run.Stdout.size())), expected.View());
	EXPECT_EQ(run.Stderr, "на stderr");
}

TEST_F(ToolProcessTest, BothStreamsAreDrainedWhileItRuns)
{
	constexpr uint32 Bytes = 512u * 1024u;
	platform::SToolProcessDesc desc;
	desc.Executable = platform::GetExecutablePath();
	desc.Arguments = { FString(test::ChildModeSwitch) + "tool-noisy", FString::Format("{}", Bytes) };
	const platform::SToolProcessResult run = platform::RunToolProcess(desc);

	EXPECT_FALSE(run.bTimedOut);
	EXPECT_EQ(run.ExitCode, 0);
	EXPECT_EQ(run.Stdout.size(), Bytes);
	EXPECT_EQ(run.Stderr.Len(), Bytes);
}

TEST_F(ToolProcessTest, AToolThatCannotStartSaysWhy)
{
	platform::SToolProcessDesc desc;
	desc.Executable = Dir / FStringView("Немає.exe").ToPath();
	const platform::SToolProcessResult run = platform::RunToolProcess(desc);
	EXPECT_FALSE(run.bStarted);
	EXPECT_EQ(run.ExitCode, -1);
	EXPECT_TRUE(run.Stderr.Contains("Немає.exe")) << run.Stderr.CStr();
}

TEST_F(ToolProcessTest, ATimeoutEndsIt)
{
	const std::filesystem::path pidFile = Dir / "pid.txt";
	platform::SToolProcessDesc desc;
	desc.Executable = platform::GetExecutablePath();
	desc.Arguments = SleepArguments(pidFile);
	desc.TimeoutMs = 1500u;
	const auto start = std::chrono::steady_clock::now();
	const platform::SToolProcessResult run = platform::RunToolProcess(desc);

	EXPECT_TRUE(run.bStarted);
	EXPECT_TRUE(run.bTimedOut);
	EXPECT_EQ(run.ExitCode, -1);
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30));
	EXPECT_TRUE(HasEnded(pidFile));
}

TEST_F(ToolProcessTest, WhatAToolLeavesBehindDoesNotHoldTheCallUpAndDiesWithIt)
{
	const std::filesystem::path pidFile = Dir / "pid.txt";
	platform::SToolProcessDesc desc;
	desc.Executable = platform::GetExecutablePath();
	desc.Arguments = { FString(test::ChildModeSwitch) + "tool-leaver", FString::FromPath(pidFile) };
	const auto start = std::chrono::steady_clock::now();
	const platform::SToolProcessResult run = platform::RunToolProcess(desc);

	EXPECT_FALSE(run.bTimedOut);
	EXPECT_EQ(run.ExitCode, 0);
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(25));
	EXPECT_TRUE(HasEnded(pidFile));
}

TEST_F(ToolProcessTest, AToolDiesWithTheProcessThatStartedIt)
{
	const std::filesystem::path pidFile = Dir / "pid.txt";
	test::CTestProcess host;
	const FString arguments[] = { FString(test::ChildModeSwitch) + "tool-host", FString::FromPath(pidFile) };
	ASSERT_TRUE(host.Start(platform::GetExecutablePath(), arguments));
	ASSERT_TRUE(test::WaitUntil([&] { return platform::Stat(pidFile).has_value(); }));

	host.Kill();
	EXPECT_TRUE(HasEnded(pidFile));
}

TEST_F(ToolProcessTest, FindOnPathLooksOnPathAlone)
{
	const std::filesystem::path listed = Dir / FStringView("На шляху 路径").ToPath();
	const std::filesystem::path current = Dir / FStringView("Поточна").ToPath();
	std::filesystem::create_directories(listed);
	std::filesystem::create_directories(current);
	ASSERT_TRUE(platform::WriteFileText(listed / FStringView("інструмент.exe").ToPath(), "x"));
	ASSERT_TRUE(platform::WriteFileText(listed / FStringView("скрипт.bat").ToPath(), "x"));
	ASSERT_TRUE(platform::WriteFileText(current / FStringView("тут.exe").ToPath(), "x"));
	std::filesystem::create_directories(listed / FStringView("тека.exe").ToPath());

	const std::filesystem::path previous = std::filesystem::current_path();
	std::filesystem::current_path(current);
	{
		const CScopedPath path(FString::Format(".;;\"{}\";{}", FString::FromPath(listed), FString::FromPath(Dir)).View());
		EXPECT_EQ(platform::FindOnPath("інструмент"), listed / FStringView("інструмент.exe").ToPath());
		EXPECT_EQ(platform::FindOnPath("інструмент.EXE"), listed / FStringView("інструмент.EXE").ToPath());
		// In the working directory, and "." is on PATH: this is the search a bare CreateProcess would make.
		EXPECT_TRUE(platform::FindOnPath("тут").empty());
		EXPECT_TRUE(platform::FindOnPath("скрипт").empty());
		EXPECT_TRUE(platform::FindOnPath("тека").empty());
		EXPECT_TRUE(platform::FindOnPath("Немає").empty());
		EXPECT_TRUE(platform::FindOnPath("").empty());
		EXPECT_TRUE(platform::FindOnPath(FString::FromPath(listed / FStringView("інструмент").ToPath())).empty());
	}
	{
		const CScopedPath path("");
		EXPECT_TRUE(platform::FindOnPath("інструмент").empty());
	}
	std::filesystem::current_path(previous);
}
