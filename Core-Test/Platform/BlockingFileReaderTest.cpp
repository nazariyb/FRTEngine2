// CBlockingFileReader (Platform/BlockingFileReader.h): positional reads return exactly the bytes at
// their offset, stop cleanly at the end of the file, work past 2 GB and 4 GB, and never stand in the
// way of a writer - an in-place write or an atomic replace goes through while the reader is open.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "Containers/String.h"
#include "Platform/BlockingFileReader.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;


namespace
{
template <typename T>
concept CanOpen = requires (platform::CBlockingFileReader& Reader, const T& Path) { { Reader.Open(Path) }; };

static_assert(CanOpen<std::filesystem::path>);
static_assert(!CanOpen<const char*>);
static_assert(!CanOpen<FString>);


// Byte i of the test file: changes with every offset, so a read from the wrong place shows.
uint8 PatternByte (uint64 Offset)
{
	return static_cast<uint8>((Offset * 131u) ^ (Offset >> 8u));
}

std::vector<uint8> MakePattern (size_t Size)
{
	std::vector<uint8> bytes(Size);
	for (size_t i = 0u; i < Size; ++i)
	{
		bytes[i] = PatternByte(i);
	}
	return bytes;
}

std::span<const uint8> AsBytes (FStringView Text)
{
	return std::span<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len());
}

class BlockingFileReaderTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Читач 读取 😀 {}", GetCurrentProcessId()).ToPath() / "Reader";
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

	std::filesystem::path Dir;
};
}


TEST_F(BlockingFileReaderTest, ReadsRangesAndStopsAtTheEnd)
{
	const std::filesystem::path file = Dir / FStringView("Діапазони 范围.bin").ToPath();
	const std::vector<uint8> pattern = MakePattern(10000u);
	ASSERT_TRUE(platform::WriteFileBytes(file, pattern));

	platform::CBlockingFileReader reader;
	FString error;
	ASSERT_TRUE(reader.Open(file, &error)) << error.CStr();
	EXPECT_TRUE(reader.IsOpen());
	EXPECT_EQ(reader.GetSize(), 10000u);

	std::vector<uint8> buffer(100u);
	ASSERT_EQ(reader.ReadAt(0u, buffer), std::optional<uint64>(100u));
	EXPECT_EQ(0, std::memcmp(buffer.data(), pattern.data(), 100u));
	ASSERT_EQ(reader.ReadAt(4321u, buffer), std::optional<uint64>(100u));
	EXPECT_EQ(0, std::memcmp(buffer.data(), pattern.data() + 4321u, 100u));

	// Short at the end, nothing at or past it; an empty read reads nothing.
	EXPECT_EQ(reader.ReadAt(9990u, buffer), std::optional<uint64>(10u));
	EXPECT_EQ(0, std::memcmp(buffer.data(), pattern.data() + 9990u, 10u));
	EXPECT_EQ(reader.ReadAt(10000u, buffer), std::optional<uint64>(0u));
	EXPECT_EQ(reader.ReadAt(1u << 20u, buffer), std::optional<uint64>(0u));
	EXPECT_EQ(reader.ReadAt(0u, std::span<uint8>()), std::optional<uint64>(0u));

	EXPECT_TRUE(reader.ReadExactAt(5000u, buffer));
	EXPECT_FALSE(reader.ReadExactAt(9990u, buffer, &error));
	EXPECT_FALSE(error.IsEmpty());

	// The whole file in one read.
	std::vector<uint8> whole(20000u);
	ASSERT_EQ(reader.ReadAt(0u, whole), std::optional<uint64>(10000u));
	whole.resize(10000u);
	EXPECT_EQ(whole, pattern);

	reader.Close();
	EXPECT_FALSE(reader.IsOpen());
	EXPECT_EQ(reader.GetSize(), 0u);
	EXPECT_FALSE(reader.ReadAt(0u, buffer).has_value());
}

TEST_F(BlockingFileReaderTest, OpenFailsForMissingFilesAndDirectories)
{
	platform::CBlockingFileReader reader;
	FString error;
	EXPECT_FALSE(reader.Open(Dir / FStringView("Немає.bin").ToPath(), &error));
	EXPECT_TRUE(error.Contains("Немає.bin")) << error.CStr();
	EXPECT_FALSE(reader.IsOpen());

	error.Clear();
	EXPECT_FALSE(reader.Open(Dir, &error));
	EXPECT_FALSE(error.IsEmpty());
}

TEST_F(BlockingFileReaderTest, MovingTransfersTheOpenFile)
{
	const std::filesystem::path file = Dir / "move.bin";
	ASSERT_TRUE(platform::WriteFileBytes(file, MakePattern(64u)));

	platform::CBlockingFileReader first;
	ASSERT_TRUE(first.Open(file));
	platform::CBlockingFileReader second(std::move(first));
	EXPECT_FALSE(first.IsOpen());
	ASSERT_TRUE(second.IsOpen());
	EXPECT_EQ(second.GetSize(), 64u);

	platform::CBlockingFileReader third;
	third = std::move(second);
	EXPECT_FALSE(second.IsOpen());
	uint8 byte = 0u;
	ASSERT_TRUE(third.ReadExactAt(63u, std::span<uint8>(&byte, 1u)));
	EXPECT_EQ(byte, PatternByte(63u));
}

// Offsets past 2^31 and 2^32 in a 5 GiB sparse file (no disk space used): a 32-bit offset or size
// anywhere on the way would read the wrong place or fail.
TEST_F(BlockingFileReaderTest, ReadsPastFourGigabytes)
{
	constexpr uint64 FileSize = 5ull << 30u;
	const std::filesystem::path file = Dir / FStringView("Великий розріджений.bin").ToPath();
	{
		const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		ASSERT_NE(handle, INVALID_HANDLE_VALUE);
		DWORD returned = 0u;
		if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0u, nullptr, 0u, &returned, nullptr))
		{
			CloseHandle(handle);
			GTEST_SKIP() << "the temp volume has no sparse files";
		}
		LARGE_INTEGER end = {};
		end.QuadPart = static_cast<LONGLONG>(FileSize);
		ASSERT_TRUE(SetFilePointerEx(handle, end, nullptr, FILE_BEGIN));
		ASSERT_TRUE(SetEndOfFile(handle));

		// 16-byte markers straddling 2 GiB and 4 GiB, and ending at the end of the file.
		for (const uint64 offset : { (1ull << 31u) - 8u, (1ull << 32u) - 8u, FileSize - 16u })
		{
			uint8 marker[16];
			for (uint64 i = 0u; i < sizeof(marker); ++i)
			{
				marker[i] = PatternByte(offset + i);
			}
			OVERLAPPED overlapped = {};
			overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
			overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32u);
			DWORD written = 0u;
			ASSERT_TRUE(WriteFile(handle, marker, sizeof(marker), &written, &overlapped));
			ASSERT_EQ(written, sizeof(marker));
		}
		CloseHandle(handle);
	}

	platform::CBlockingFileReader reader;
	ASSERT_TRUE(reader.Open(file));
	EXPECT_EQ(reader.GetSize(), FileSize);

	for (const uint64 offset : { (1ull << 31u) - 8u, (1ull << 32u) - 8u, FileSize - 16u })
	{
		uint8 marker[16] = {};
		ASSERT_TRUE(reader.ReadExactAt(offset, marker)) << offset;
		for (uint64 i = 0u; i < sizeof(marker); ++i)
		{
			EXPECT_EQ(marker[i], PatternByte(offset + i)) << "offset " << offset + i;
		}
	}

	// The hole between reads as zeros; the end is the end.
	uint8 hole[32] = { 1u };
	ASSERT_TRUE(reader.ReadExactAt(3ull << 30u, hole));
	for (const uint8 byte : hole)
	{
		EXPECT_EQ(byte, 0u);
	}
	EXPECT_EQ(reader.ReadAt(FileSize - 4u, hole), std::optional<uint64>(4u));
	EXPECT_EQ(reader.ReadAt(FileSize, hole), std::optional<uint64>(0u));
}

TEST_F(BlockingFileReaderTest, AnOpenReaderNeverBlocksAWriter)
{
	const std::filesystem::path file = Dir / FStringView("Спільний.bin").ToPath();
	ASSERT_TRUE(platform::WriteFileBytes(file, AsBytes("old bytes")));

	platform::CBlockingFileReader reader;
	ASSERT_TRUE(reader.Open(file));

	// An atomic replace goes through; the reader keeps the file it opened.
	FString error;
	ASSERT_TRUE(platform::WriteFileAtomic(file, AsBytes("NEW BYTES!"), true, &error)) << error.CStr();
	std::vector<uint8> buffer(9u);
	ASSERT_TRUE(reader.ReadExactAt(0u, buffer));
	EXPECT_EQ(FStringView(reinterpret_cast<const char*>(buffer.data()), buffer.size()), "old bytes");
	reader.Close();

	// So does an in-place write to the file a reader has open.
	ASSERT_TRUE(reader.Open(file));
	ASSERT_TRUE(platform::WriteFileBytes(file, AsBytes("rewritten"), true, &error)) << error.CStr();
	ASSERT_TRUE(reader.ReadExactAt(0u, buffer));
	EXPECT_EQ(FStringView(reinterpret_cast<const char*>(buffer.data()), buffer.size()), "rewritten");
}
