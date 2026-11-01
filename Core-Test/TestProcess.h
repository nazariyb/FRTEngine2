#pragma once

#include <chrono>
#include <filesystem>
#include <span>
#include <thread>

#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::test
{
/**
 * Another program run by a test, with its output: AssetTool's exit codes and --json lines, a Core-Test
 * child mode holding a lock while the parent checks it (TestCommon.h, RegisterChildMode).
 *
 * The child runs in a job object that kills it when the CTestProcess goes (or Core-Test dies), so no
 * test leaves a process behind. Its standard output and error go to files of their own, read back by
 * ReadOutput / ReadErrors at any time (a pipe could fill up and block the child while the test waits
 * for it). Its standard input is a pipe: closed at once, so the child reads the end of its input, or
 * with bKeepInputOpen held until CloseInput - "Enter" for a child that waits for its input to end.
 * Arguments are quoted as CommandLineToArgvW reads them back, so they arrive exactly as given,
 * non-ASCII text and spaces included.
 */
class CTestProcess
{
public:
	CTestProcess () = default;
	~CTestProcess ();

	CTestProcess (const CTestProcess&) = delete;
	CTestProcess& operator= (const CTestProcess&) = delete;

	// False (with the reason in *OutError) if it could not be started. WorkingDir empty: this process's.
	bool Start (const std::filesystem::path& Executable, std::span<const FString> Arguments, bool bKeepInputOpen = false,
		const std::filesystem::path& WorkingDir = {}, FString* OutError = nullptr);

	// The end of the child's input.
	void CloseInput ();
	[[nodiscard]] uint32 GetProcessId () const { return ProcessId; }
	[[nodiscard]] bool IsRunning () const;
	// Ends it at once (TerminateProcess: no cleanup runs in the child, as in a crash).
	void Kill ();

	// Its exit code once it ended; -1 if it still runs after Timeout (it is left running).
	int32 Wait (std::chrono::milliseconds Timeout = std::chrono::seconds(60));

	// What it has written so far, as raw bytes.
	[[nodiscard]] FString ReadOutput () const;
	[[nodiscard]] FString ReadErrors () const;

private:
	void Close ();

	void* Job = nullptr;
	void* Process = nullptr;
	void* InputWrite = nullptr;
	uint32 ProcessId = 0u;
	std::filesystem::path OutputFile;
	std::filesystem::path ErrorFile;
};

// One run to its end: CTestProcess Start, CloseInput, Wait.
struct SProcessResult
{
	int32 ExitCode = -1;   // -1: it did not start, or did not end within the timeout (then killed)
	FString Output;
	FString Errors;
};

SProcessResult RunProcess (const std::filesystem::path& Executable, std::span<const FString> Arguments,
	const std::filesystem::path& WorkingDir = {}, std::chrono::milliseconds Timeout = std::chrono::seconds(60));

// Calls Condition every few milliseconds until it returns true or Timeout passes; its last answer.
template <typename F>
bool WaitUntil (F&& Condition, std::chrono::milliseconds Timeout = std::chrono::seconds(20));
}


template <typename F>
bool frt::test::WaitUntil (F&& Condition, std::chrono::milliseconds Timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + Timeout;
	for (;;)
	{
		if (Condition())
		{
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}
