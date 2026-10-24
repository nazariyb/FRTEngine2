// frt::log writes "[category] message" lines, filtered by a minimum level per category that can
// change at run time, and stays whole when many threads log at once - through the test hook, and
// through the real output: stderr and the debugger.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Log/LogCapture.h"

#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include "Containers/String.h"
#include "Log/Log.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;


namespace
{
// Every test starts and ends with the levels the process starts with.
class LogTest : public ::testing::Test
{
protected:
	void SetUp () override { log::ResetAllMinLevels(); }
	void TearDown () override { log::ResetAllMinLevels(); }
};

// Points the process's stderr (file descriptor 2, what std::fwrite(..., stderr) reaches) at a file
// for as long as it exists, then back at what it was. The file is opened in binary mode and _dup2
// carries the mode over, so the bytes the log writes arrive unchanged.
class CStderrRedirect
{
public:
	explicit CStderrRedirect (const std::filesystem::path& Path)
	{
		std::fflush(stderr);
		if (_wsopen_s(&File, Path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO,
			_S_IREAD | _S_IWRITE) != 0)
		{
			File = -1;
			return;
		}
		Saved = _dup(_fileno(stderr));
		if (Saved >= 0 && _dup2(File, _fileno(stderr)) != 0)
		{
			_close(Saved);
			Saved = -1;
		}
	}

	~CStderrRedirect ()
	{
		Restore();
	}

	CStderrRedirect (const CStderrRedirect&) = delete;
	CStderrRedirect& operator= (const CStderrRedirect&) = delete;

	[[nodiscard]] bool IsActive () const { return Saved >= 0; }

	void Restore ()
	{
		std::fflush(stderr);
		if (Saved >= 0)
		{
			_dup2(Saved, _fileno(stderr));
			_close(Saved);
			Saved = -1;
		}
		if (File >= 0)
		{
			_close(File);
			File = -1;
		}
	}

private:
	int File = -1;
	int Saved = -1;
};

// Records the text OutputDebugStringW passes to the system while no debugger is attached. It raises
// DBG_PRINTEXCEPTION_WIDE_C and, when nobody handles that, DBG_PRINTEXCEPTION_C with the text in the
// ANSI code page (the form DebugView and the DBWIN buffer receive); a vectored handler sees both
// before the function's own __except, and passes them on. With a debugger attached, the debugger
// takes them instead and nothing is recorded.
class CDebugOutputCapture
{
public:
	CDebugOutputCapture () noexcept
	{
		Instance = this;
		Handle = AddVectoredExceptionHandler(1u, &Handler);
	}

	~CDebugOutputCapture ()
	{
		if (Handle != nullptr)
		{
			RemoveVectoredExceptionHandler(Handle);
		}
		Instance = nullptr;
	}

	CDebugOutputCapture (const CDebugOutputCapture&) = delete;
	CDebugOutputCapture& operator= (const CDebugOutputCapture&) = delete;

	[[nodiscard]] bool IsActive () const { return Handle != nullptr; }

	[[nodiscard]] std::vector<FString> GetTexts () const
	{
		const std::scoped_lock lock(Mutex);
		return Texts;
	}

private:
	static LONG CALLBACK Handler (EXCEPTION_POINTERS* Info)
	{
		const EXCEPTION_RECORD& record = *Info->ExceptionRecord;
		CDebugOutputCapture* capture = Instance;
		if (capture != nullptr && record.ExceptionCode == DBG_PRINTEXCEPTION_C && record.NumberParameters >= 2u)
		{
			// The length counts the terminating NUL.
			const auto length = static_cast<uint32>(record.ExceptionInformation[0]);
			const auto* text = reinterpret_cast<const char*>(record.ExceptionInformation[1]);
			const std::scoped_lock lock(capture->Mutex);
			capture->Texts.emplace_back(FStringView(text, length > 0u ? length - 1u : 0u));
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	static inline CDebugOutputCapture* Instance = nullptr;
	void* Handle = nullptr;
	mutable std::mutex Mutex;
	std::vector<FString> Texts;
};

// Checks lines "[assets] thread T message I" from Threads threads, PerThread each: every line whole
// and each thread's lines in its own order. Lines carry their '\n'.
void ExpectWholeLinesInThreadOrder (const std::vector<FString>& Lines, int Threads, int PerThread)
{
	ASSERT_EQ(Lines.size(), static_cast<size_t>(Threads * PerThread));
	std::vector<int> next(static_cast<size_t>(Threads), 0);
	for (const FString& line : Lines)
	{
		int thread = -1;
		for (int t = 0; t < Threads; ++t)
		{
			if (line.StartsWith(FString::Format("[assets] thread {} ", t)))
			{
				thread = t;
			}
		}
		ASSERT_GE(thread, 0) << line.CStr();
		const int index = next[static_cast<size_t>(thread)]++;
		EXPECT_EQ(line, FString::Format("[assets] thread {} message {}\n", thread, index));
	}
}
}


TEST_F(LogTest, WritesOneLinePerMessageWithItsCategory)
{
	test::CLogCapture capture;
	log::Write(log::Assets, log::ELevel::Warning, "Content/Rock.png: missing meta");
	log::Info(log::Platform, "{} files in {:.2f} ms", 3, 1.25);
	log::Printf(log::Game, log::ELevel::Error, "module '%s' failed (%d)", "Demo", 5);

	const std::vector<test::CLogCapture::SEntry>& entries = capture.GetEntries();
	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0].Line, "[assets] Content/Rock.png: missing meta\n");
	EXPECT_EQ(entries[0].Category, "assets");
	EXPECT_EQ(entries[0].Level, log::ELevel::Warning);
	EXPECT_EQ(entries[0].Text, "Content/Rock.png: missing meta");
	EXPECT_EQ(entries[1].Line, "[platform] 3 files in 1.25 ms\n");
	EXPECT_EQ(entries[2].Line, "[game] module 'Demo' failed (5)\n");
	EXPECT_EQ(entries[2].Level, log::ELevel::Error);
}

TEST_F(LogTest, VerboseIsOffAndTheRestIsOnByDefault)
{
	EXPECT_EQ(log::GetMinLevel(log::Assets), log::ELevel::Info);
	EXPECT_TRUE(log::IsEnabled(log::Assets, log::ELevel::Error));
	EXPECT_TRUE(log::IsEnabled(log::Assets, log::ELevel::Info));
	EXPECT_FALSE(log::IsEnabled(log::Assets, log::ELevel::Verbose));

	test::CLogCapture capture;
	log::Verbose(log::Assets, "hidden {}", 1);
	log::Write(log::Assets, log::ELevel::Verbose, "hidden");
	EXPECT_TRUE(capture.GetEntries().empty());
}

TEST_F(LogTest, EachCategoryHasItsOwnMinimumLevel)
{
	test::CLogCapture capture;
	ASSERT_TRUE(log::SetMinLevel(log::Assets, log::ELevel::Verbose));
	ASSERT_TRUE(log::SetMinLevel(log::Platform, log::ELevel::Error));

	log::Verbose(log::Assets, "verbose assets");
	log::Verbose(log::Memory, "verbose memory");        // the default: Info
	log::Warning(log::Platform, "a platform warning");   // below Error
	log::Error(log::Platform, "a platform error");
	log::Info(log::Memory, "memory info");

	const std::vector<FString> lines = capture.GetLines();
	EXPECT_EQ(lines, (std::vector<FString>{ "[assets] verbose assets\n", "[platform] a platform error\n", "[memory] memory info\n" }));

	// A category named at run time (from a config value) is the same category as the literal.
	EXPECT_EQ(log::GetMinLevel(FString("assets")), log::ELevel::Verbose);

	log::ResetMinLevel(log::Assets);
	EXPECT_EQ(log::GetMinLevel(log::Assets), log::ELevel::Info);
	EXPECT_EQ(log::GetMinLevel(log::Platform), log::ELevel::Error);
}

TEST_F(LogTest, TheDefaultMinimumAppliesToCategoriesWithoutTheirOwn)
{
	log::SetMinLevel(log::Assets, log::ELevel::Info);
	log::SetDefaultMinLevel(log::ELevel::Warning);
	EXPECT_FALSE(log::IsEnabled(log::Memory, log::ELevel::Info));
	EXPECT_TRUE(log::IsEnabled(log::Memory, log::ELevel::Warning));
	EXPECT_TRUE(log::IsEnabled(log::Assets, log::ELevel::Info));

	log::SetDefaultMinLevel(log::ELevel::Verbose);
	EXPECT_TRUE(log::IsEnabled(log::Memory, log::ELevel::Verbose));
	EXPECT_FALSE(log::IsEnabled(log::Assets, log::ELevel::Verbose));

	log::ResetAllMinLevels();
	EXPECT_EQ(log::GetMinLevel(log::Assets), log::ELevel::Info);
	EXPECT_FALSE(log::IsEnabled(log::Memory, log::ELevel::Verbose));
}

TEST_F(LogTest, TheLevelTableIsBoundedAndRefusesOneCategoryTooMany)
{
	for (uint32 i = 0u; i < log::MaxCategoryLevels; ++i)
	{
		EXPECT_TRUE(log::SetMinLevel(FString::Format("category{}", i), log::ELevel::Warning));
	}
	EXPECT_FALSE(log::SetMinLevel("one-too-many", log::ELevel::Warning));
	// Changing a category that has a level still works.
	EXPECT_TRUE(log::SetMinLevel("category7", log::ELevel::Error));
	EXPECT_EQ(log::GetMinLevel("category7"), log::ELevel::Error);
	EXPECT_EQ(log::GetMinLevel("one-too-many"), log::ELevel::Info);
}

TEST_F(LogTest, LongCategoryNamesAreCutForLevelsButPrintedInFull)
{
	const FString name = FString::Repeat("x", log::MaxCategoryLength + 5u);
	ASSERT_TRUE(log::SetMinLevel(name, log::ELevel::Error));
	EXPECT_EQ(log::GetMinLevel(name), log::ELevel::Error);

	test::CLogCapture capture;
	log::Write(name, log::ELevel::Error, "message");
	ASSERT_EQ(capture.GetEntries().size(), 1u);
	EXPECT_EQ(capture.GetEntries()[0].Category, name);
}

TEST_F(LogTest, LevelNamesRoundTrip)
{
	for (const log::ELevel level : { log::ELevel::Error, log::ELevel::Warning, log::ELevel::Info, log::ELevel::Verbose })
	{
		EXPECT_EQ(log::ParseLevel(log::GetLevelName(level)), level);
	}
	EXPECT_EQ(log::ParseLevel("WARNING"), log::ELevel::Warning);
	EXPECT_EQ(log::ParseLevel("Verbose"), log::ELevel::Verbose);
	EXPECT_FALSE(log::ParseLevel("loud").has_value());
}

TEST_F(LogTest, LinesFromManyThreadsNeverInterleave)
{
	constexpr int threads = 8;
	constexpr int perThread = 500;
	test::CLogCapture capture;
	std::vector<std::thread> workers;
	for (int t = 0; t < threads; ++t)
	{
		workers.emplace_back([t] ()
		{
			for (int i = 0; i < perThread; ++i)
			{
				log::Info(log::Assets, "thread {} message {}", t, i);
			}
		});
	}
	for (std::thread& worker : workers)
	{
		worker.join();
	}

	const std::vector<test::CLogCapture::SEntry>& entries = capture.GetEntries();
	ASSERT_EQ(entries.size(), static_cast<size_t>(threads * perThread));
	std::vector<int> next(threads, 0);
	for (const test::CLogCapture::SEntry& entry : entries)
	{
		int thread = -1;
		for (int t = 0; t < threads; ++t)
		{
			if (entry.Text.StartsWith(FString::Format("thread {} ", t)))
			{
				thread = t;
			}
		}
		ASSERT_GE(thread, 0) << entry.Text.CStr();
		// Each thread's lines arrive whole and in its own order.
		const int index = next[thread]++;
		EXPECT_EQ(entry.Text, FString::Format("thread {} message {}", thread, index));
		EXPECT_EQ(entry.Line, FString::Format("[assets] thread {} message {}\n", thread, index));
	}
}

TEST_F(LogTest, WithoutAHookEveryLineReachesStderrWholeAndTheDebuggerToo)
{
	// No CLogCapture: the path every other test replaces. stderr goes to a file in a directory of
	// this process's own with a non-ASCII name for the duration.
	FString name("FRT Журнал 日志 😀 ");
	name.AppendNumber(GetCurrentProcessId());
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / name.ToPath();
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	ASSERT_TRUE(std::filesystem::create_directories(dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	const std::filesystem::path file = dir / u8"stderr журнал.txt";

	constexpr int threads = 8;
	constexpr int perThread = 500;
	std::vector<FString> debugTexts;
	bool bDebuggerAttached = false;
	{
		CDebugOutputCapture debugOutput;
		ASSERT_TRUE(debugOutput.IsActive());
		CStderrRedirect redirect(file);
		ASSERT_TRUE(redirect.IsActive()) << "cannot redirect stderr to " << FString::FromPath(file).CStr();

		std::vector<std::thread> workers;
		for (int t = 0; t < threads; ++t)
		{
			workers.emplace_back([t] ()
			{
				for (int i = 0; i < perThread; ++i)
				{
					log::Info(log::Assets, "thread {} message {}", t, i);
				}
			});
		}
		for (std::thread& worker : workers)
		{
			worker.join();
		}
		redirect.Restore();
		bDebuggerAttached = IsDebuggerPresent() != FALSE;
		debugTexts = debugOutput.GetTexts();
	}

	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(file);
	std::filesystem::remove_all(dir, ec);
	ASSERT_TRUE(bytes.has_value());

	// One line per message, each ending in '\n' and nothing after the last.
	const FStringView text(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size()));
	ASSERT_TRUE(text.EndsWith("\n"));
	std::vector<FString> lines;
	uint32 start = 0u;
	for (uint32 i = 0u; i < text.Len(); ++i)
	{
		if (text.GetData()[i] == '\n')
		{
			lines.emplace_back(FStringView(text.GetData() + start, i + 1u - start));
			start = i + 1u;
		}
	}
	ExpectWholeLinesInThreadOrder(lines, threads, perThread);

	// The same lines went to the debugger's output, one call each.
	if (bDebuggerAttached)
	{
		GTEST_SKIP() << "a debugger takes the debug output itself; only stderr was checked";
	}
	ExpectWholeLinesInThreadOrder(debugTexts, threads, perThread);
}
