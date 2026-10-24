// The writer lock's platform half (Platform/FileLock.h): one holder at a time, a refused caller reads
// who holds it, the lock goes with its holder - released, moved away, or its process killed without
// any cleanup - and a file left by a power cut never keeps anyone out. Readers never take or block
// the lock. Everything runs in directories with non-ASCII names.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "Containers/String.h"
#include "Platform/FileLock.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace std::chrono_literals;


namespace
{
// Narrow text never becomes a path (Platform.h).
template <typename T>
concept CanLock = requires (const T& Path) { { platform::TryLockFileExclusive(Path, "x", nullptr) }; };

static_assert(CanLock<std::filesystem::path>);
static_assert(!CanLock<const char*>);
static_assert(!CanLock<std::string>);
static_assert(!CanLock<FStringView>);


// "Core-Test --frt-test-child=file-lock-holder <lock file> <text>": takes the lock with that text and
// holds it until killed. Gives up after a minute, so it never outlives a parent that died without
// killing it (the parent's job object normally sees to that).
int RunLockHolder (std::span<const FString> Arguments)
{
	if (Arguments.size() != 2u)
	{
		return 2;
	}
	const std::optional<platform::CExclusiveFileLock> lock = platform::TryLockFileExclusive(Arguments[0].ToPath(), Arguments[1], nullptr);
	if (!lock.has_value())
	{
		return 1;
	}
	std::this_thread::sleep_for(60s);
	return 0;
}

const bool bLockHolderRegistered = test::RegisterChildMode("file-lock-holder", &RunLockHolder);


class FileLockTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Замок 锁 🔒 {}", GetCurrentProcessId()).ToPath() / "FileLock";
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	[[nodiscard]] std::filesystem::path File (FStringView Name) const
	{
		return Dir / Name.ToPath();
	}

	std::filesystem::path Dir;
};
}


TEST_F(FileLockTest, TheHolderWritesItsTextAndTheFileGoesWithTheLock)
{
	// Its directories are created as needed.
	const std::filesystem::path path = File("Локальне 本地") / FStringView("Assets/Власник.lock").ToPath();
	FString holder = "stale";
	FString error = "stale";
	std::optional<platform::CExclusiveFileLock> lock = platform::TryLockFileExclusive(path, "Pid=1\nExe=Тест.exe\n", &holder, &error);
	ASSERT_TRUE(lock.has_value()) << error.CStr();
	EXPECT_TRUE(holder.IsEmpty());
	EXPECT_TRUE(error.IsEmpty());
	EXPECT_TRUE(lock->IsLocked());
	EXPECT_EQ(lock->GetPath(), path);

	// Readers read it while it is held, and take nothing.
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "Pid=1\nExe=Тест.exe\n");
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "Pid=1\nExe=Тест.exe\n");

	lock->Release();
	EXPECT_FALSE(lock->IsLocked());
	EXPECT_TRUE(lock->GetPath().empty());
	EXPECT_FALSE(std::filesystem::exists(path));
	lock->Release();   // idempotent

	// And the destructor lets go the same way.
	{
		const std::optional<platform::CExclusiveFileLock> scoped = platform::TryLockFileExclusive(path, "scoped", nullptr);
		ASSERT_TRUE(scoped.has_value());
		EXPECT_TRUE(std::filesystem::exists(path));
	}
	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(FileLockTest, ASecondLockIsRefusedAndReadsWhoHoldsIt)
{
	const std::filesystem::path path = File("Owner.lock");
	std::optional<platform::CExclusiveFileLock> first = platform::TryLockFileExclusive(path, "Pid=1234\nExe=Редактор.exe\n", nullptr);
	ASSERT_TRUE(first.has_value());

	// Locks are per open: this process is refused like any other.
	FString holder;
	FString error;
	const auto start = std::chrono::steady_clock::now();
	const std::optional<platform::CExclusiveFileLock> second = platform::TryLockFileExclusive(path, "Pid=5678\n", &holder, &error);
	EXPECT_FALSE(second.has_value());
	EXPECT_LT(std::chrono::steady_clock::now() - start, 400ms);   // a live holder is reported at once
	EXPECT_EQ(holder, "Pid=1234\nExe=Редактор.exe\n");
	EXPECT_TRUE(error.Contains("Owner.lock")) << error.CStr();

	// The holder's text is unchanged by the attempt.
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "Pid=1234\nExe=Редактор.exe\n");

	first.reset();
	const std::optional<platform::CExclusiveFileLock> third = platform::TryLockFileExclusive(path, "Pid=5678\n", &holder, &error);
	ASSERT_TRUE(third.has_value()) << error.CStr();
	EXPECT_TRUE(holder.IsEmpty());
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "Pid=5678\n");
}

TEST_F(FileLockTest, AMovedLockKeepsHoldingIt)
{
	const std::filesystem::path path = File("Переміщений.lock");
	std::optional<platform::CExclusiveFileLock> taken = platform::TryLockFileExclusive(path, "moved", nullptr);
	ASSERT_TRUE(taken.has_value());

	platform::CExclusiveFileLock moved(std::move(*taken));
	EXPECT_FALSE(taken->IsLocked());
	EXPECT_TRUE(moved.IsLocked());
	EXPECT_EQ(moved.GetPath(), path);
	taken.reset();   // the moved-from object lets go of nothing
	EXPECT_FALSE(platform::TryLockFileExclusive(path, "other", nullptr).has_value());

	platform::CExclusiveFileLock assigned;
	EXPECT_FALSE(assigned.IsLocked());
	assigned = std::move(moved);
	EXPECT_FALSE(moved.IsLocked());
	EXPECT_TRUE(assigned.IsLocked());
	EXPECT_FALSE(platform::TryLockFileExclusive(path, "other", nullptr).has_value());

	// Assigning over a held lock lets go of it.
	const std::filesystem::path otherPath = File("Інший.lock");
	std::optional<platform::CExclusiveFileLock> other = platform::TryLockFileExclusive(otherPath, "other", nullptr);
	ASSERT_TRUE(other.has_value());
	assigned = std::move(*other);
	EXPECT_FALSE(std::filesystem::exists(path));
	EXPECT_EQ(assigned.GetPath(), otherPath);
}

// A power cut leaves the file behind with nobody holding it: the next caller takes it over, and its
// text replaces all of the old one (a longer stale text must not show through).
TEST_F(FileLockTest, AFileLeftByAPowerCutIsTakenOver)
{
	const std::filesystem::path path = File("Залишений.lock");
	const FStringView stale = "Pid=999999\nExe=C:\\Old\\Very long name of a process that died.exe\n";
	ASSERT_TRUE(platform::WriteFileBytes(path, stale.AsBytes()));

	FString error;
	const std::optional<platform::CExclusiveFileLock> lock = platform::TryLockFileExclusive(path, "Pid=7\n", nullptr, &error);
	ASSERT_TRUE(lock.has_value()) << error.CStr();
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "Pid=7\n");
}

// A holder that lets go while someone reads the file leaves it pending deletion until the reader closes
// it, and every open fails meanwhile: the lock is tried again for a moment instead of failing.
TEST_F(FileLockTest, ALockFreedWhileSomeoneReadsTheFileIsTakenOnceTheReaderCloses)
{
	const std::filesystem::path path = File("Читач.lock");
	std::optional<platform::CExclusiveFileLock> first = platform::TryLockFileExclusive(path, "first", nullptr);
	ASSERT_TRUE(first.has_value());

	// A reader as ReadFileText opens it: sharing everything, so the holder is never blocked.
	const HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(reader, INVALID_HANDLE_VALUE);
	first.reset();
	std::thread closeLater([reader] { std::this_thread::sleep_for(100ms); CloseHandle(reader); });

	FString error;
	const std::optional<platform::CExclusiveFileLock> second = platform::TryLockFileExclusive(path, "second", nullptr, &error);
	closeLater.join();
	ASSERT_TRUE(second.has_value()) << error.CStr();
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "second");
}

TEST_F(FileLockTest, FailuresWithoutAHolderSayWhy)
{
	// A directory where the file would be.
	const std::filesystem::path directory = File("Тека.lock");
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directory(directory, ec));
	FString holder = "stale";
	FString error;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(platform::TryLockFileExclusive(directory, "text", &holder, &error).has_value());
	EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
	EXPECT_TRUE(holder.IsEmpty());
	EXPECT_TRUE(error.Contains("Тека.lock")) << error.CStr();

	// No text: a refused caller could not tell it from a holder that has not written yet.
	error.Clear();
	EXPECT_FALSE(platform::TryLockFileExclusive(File("Порожній.lock"), "", &holder, &error).has_value());
	EXPECT_TRUE(holder.IsEmpty());
	EXPECT_FALSE(error.IsEmpty());
	EXPECT_FALSE(std::filesystem::exists(File("Порожній.lock")));
}

// The test the design asks for: another process holds the lock, this one is refused and reads who
// holds it; the holder is killed (TerminateProcess: no cleanup runs, as in a crash) and the lock is
// free at once - the OS closed its handle, and the file went with it.
TEST_F(FileLockTest, ALockHeldByAChildProcessGoesAwayWhenTheChildIsKilled)
{
	const std::filesystem::path path = File("Дочірній 子进程.lock");
	const FString text = "Pid=child\nExe=Core-Test (дочірній)\n";

	test::CTestProcess child;
	const FString arguments[] = { FString(test::ChildModeSwitch) + "file-lock-holder", FString::FromPath(path), text };
	FString startError;
	ASSERT_TRUE(child.Start(platform::GetExecutablePath(), arguments, false, {}, &startError)) << startError.CStr();

	// Held: the file has the child's text.
	EXPECT_TRUE(test::WaitUntil([&] { return platform::ReadFileText(path, ETextFileEncoding::Raw) == std::optional<FString>(text); }))
		<< "the child never took the lock; its errors: " << child.ReadErrors().CStr();
	ASSERT_TRUE(child.IsRunning());

	FString holder;
	EXPECT_FALSE(platform::TryLockFileExclusive(path, "Pid=parent\n", &holder).has_value());
	EXPECT_EQ(holder, text);

	child.Kill();
	EXPECT_FALSE(child.IsRunning());
	EXPECT_FALSE(std::filesystem::exists(path));
	FString error;
	const std::optional<platform::CExclusiveFileLock> lock = platform::TryLockFileExclusive(path, "Pid=parent\n", &holder, &error);
	ASSERT_TRUE(lock.has_value()) << error.CStr() << " / holder: " << holder.CStr();
	EXPECT_EQ(platform::ReadFileText(path, ETextFileEncoding::Raw).value_or("<unreadable>"), "Pid=parent\n");
}
