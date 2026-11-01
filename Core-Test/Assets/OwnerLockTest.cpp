// The project writer lock (Assets/OwnerLock.h): what the owner writes into Owner.lock - {pid, exe,
// start time} - reads back to the same holder, a refused process names the owner the way a message
// shows it, a lock file some other tool wrote still names something, and the lock lives where the
// design puts it (<root>/Local/Assets/Owner.lock). Runs in directories with non-ASCII names.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <filesystem>
#include <optional>
#include <system_error>

#include "Assets/OwnerLock.h"
#include "Containers/String.h"
#include "Platform/FileLock.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

using namespace frt;


namespace
{
class OwnerLockTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Root = std::filesystem::temp_directory_path()
			/ FString::Format("FRT Власник 所有者 {}", platform::GetCurrentProcessId()).ToPath() / "Проєкт 项目";
		std::error_code ec;
		std::filesystem::remove_all(Root, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Root, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Root, ec);
		std::filesystem::remove(Root.parent_path(), ec);
	}

	std::filesystem::path Root;
};
}


TEST(OwnerLock, TimesAreWrittenInUtcToTheSecond)
{
	EXPECT_EQ(assets::FormatUtcTime(0), "1601-01-01T00:00:00Z");
	EXPECT_EQ(assets::FormatUtcTime(116'444'736'000'000'000ll), "1970-01-01T00:00:00Z");
	// 2026-10-24 09:41:07.1234567 UTC: the fraction is dropped, never rounded up.
	EXPECT_EQ(assets::FormatUtcTime(134'373'084'671'234'567ll), "2026-10-24T09:41:07Z");
	EXPECT_EQ(assets::FormatUtcTime(134'373'084'679'999'999ll), "2026-10-24T09:41:07Z");
	// Before 1970 the second still rounds down.
	EXPECT_EQ(assets::FormatUtcTime(116'444'735'995'000'000ll), "1969-12-31T23:59:59Z");
}

TEST(OwnerLock, TheLockFileTextRoundTrips)
{
	assets::SLockOwner owner;
	owner.ProcessId = 1234u;
	owner.Executable = "D:\\Ігри 游戏\\FRTEngine2\\Binaries\\Win64\\Editor-Development-Default.exe";
	owner.StartTime = "2026-10-24T09:41:07Z";

	const FString text = assets::FormatOwnerLock(owner);
	EXPECT_EQ(text, "Pid=1234\nExe=D:\\Ігри 游戏\\FRTEngine2\\Binaries\\Win64\\Editor-Development-Default.exe\n"
		"Started=2026-10-24T09:41:07Z\n");
	EXPECT_EQ(assets::ParseOwnerLock(text), owner);

	// CRLF (a file someone opened in an editor), unknown keys, a value holding '='.
	const std::optional<assets::SLockOwner> parsed = assets::ParseOwnerLock("Host=build-01\r\nPid=77\r\nExe=C:\\a=b\\x.exe\r\n");
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed->ProcessId, 77u);
	EXPECT_EQ(parsed->Executable, "C:\\a=b\\x.exe");
	EXPECT_TRUE(parsed->StartTime.IsEmpty());

	// No usable Pid: not an Owner.lock.
	EXPECT_FALSE(assets::ParseOwnerLock("").has_value());
	EXPECT_FALSE(assets::ParseOwnerLock("Exe=x.exe\n").has_value());
	EXPECT_FALSE(assets::ParseOwnerLock("Pid=0\n").has_value());
	EXPECT_FALSE(assets::ParseOwnerLock("Pid=twelve\n").has_value());
	EXPECT_FALSE(assets::ParseOwnerLock("an unknown process (its lock file could not be read)").has_value());
}

TEST(OwnerLock, DescribeNamesTheExecutablePidAndStart)
{
	assets::SLockOwner owner;
	owner.ProcessId = 1234u;
	owner.Executable = "D:\\Ігри\\Binaries\\Win64\\Editor-Development-Default.exe";
	owner.StartTime = "2026-10-24T09:41:07Z";
	EXPECT_EQ(owner.Describe(), "Editor-Development-Default.exe, pid 1234, started 2026-10-24 09:41:07 UTC");

	owner.Executable = "/opt/frt/AssetTool";
	owner.StartTime.Clear();
	EXPECT_EQ(owner.Describe(), "AssetTool, pid 1234");

	// A start time in a form this version does not write is shown as it is.
	owner.StartTime = "yesterday";
	EXPECT_EQ(owner.Describe(), "AssetTool, pid 1234, started yesterday");

	owner.Executable.Clear();
	owner.StartTime.Clear();
	EXPECT_EQ(owner.Describe(), "a process, pid 1234");
}

TEST(OwnerLock, ThisProcessDescribesItselfAsTheOSKnowsIt)
{
	const assets::SLockOwner self = assets::GetThisProcessAsOwner();
	EXPECT_EQ(self.ProcessId, platform::GetCurrentProcessId());
	EXPECT_EQ(self.Executable, FString::FromPath(platform::GetExecutablePath()));
	EXPECT_EQ(self.StartTime, assets::FormatUtcTime(platform::GetProcessStartTime()));
	EXPECT_EQ(assets::ParseOwnerLock(assets::FormatOwnerLock(self)), self);
}

TEST(OwnerLock, TheLockIsInTheMountsLocalAssets)
{
	const std::filesystem::path root = FStringView("D:/Ігри/Гра").ToPath();
	EXPECT_EQ(assets::GetOwnerLockPath(root), root / "Local" / "Assets" / "Owner.lock");
}

TEST_F(OwnerLockTest, ASecondOwnerIsRefusedAndToldWhoOwnsTheMount)
{
	FString owner = "stale";
	FString error;
	std::optional<platform::CExclusiveFileLock> lock = assets::TryTakeOwnerLock(Root, &owner, &error);
	ASSERT_TRUE(lock.has_value()) << error.CStr();
	EXPECT_TRUE(owner.IsEmpty());
	const std::filesystem::path lockFile = Root / "Local" / "Assets" / "Owner.lock";
	EXPECT_EQ(lock->GetPath(), lockFile);

	// The file says who holds it.
	const std::optional<FString> text = platform::ReadFileText(lockFile, ETextFileEncoding::Raw);
	ASSERT_TRUE(text.has_value());
	EXPECT_EQ(assets::ParseOwnerLock(*text), assets::GetThisProcessAsOwner());

	// Refused, with the owner as a message names it; not an error of the other kind.
	EXPECT_FALSE(assets::TryTakeOwnerLock(Root, &owner, &error).has_value());
	EXPECT_EQ(owner, assets::GetThisProcessAsOwner().Describe());
	EXPECT_FALSE(error.IsEmpty());

	lock.reset();
	EXPECT_TRUE(assets::TryTakeOwnerLock(Root, &owner, &error).has_value()) << error.CStr();
	EXPECT_TRUE(owner.IsEmpty());
}

TEST_F(OwnerLockTest, ALockFileOfAnotherFormatStillNamesItsHolder)
{
	const std::optional<platform::CExclusiveFileLock> foreign =
		platform::TryLockFileExclusive(assets::GetOwnerLockPath(Root), "  Some other tool  \n", nullptr);
	ASSERT_TRUE(foreign.has_value());

	FString owner;
	EXPECT_FALSE(assets::TryTakeOwnerLock(Root, &owner).has_value());
	EXPECT_EQ(owner, "Some other tool");
}

TEST_F(OwnerLockTest, AFailureThatIsNoOwnerLeavesTheOwnerEmpty)
{
	// Local is a file: no lock can be made below it.
	ASSERT_TRUE(platform::WriteFileBytes(Root / "Local", FStringView("not a directory").AsBytes()));
	FString owner = "stale";
	FString error;
	EXPECT_FALSE(assets::TryTakeOwnerLock(Root, &owner, &error).has_value());
	EXPECT_TRUE(owner.IsEmpty());
	EXPECT_FALSE(error.IsEmpty());
}
