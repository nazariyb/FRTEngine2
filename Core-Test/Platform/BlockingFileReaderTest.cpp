// CBlockingFileReader (Platform/BlockingFileReader.h): positional reads return exactly the bytes at
// their offset, stop cleanly at the end of the file, work past 2 GB and 4 GB and beyond what one OS
// call reads, say when a failure is a file in use (worth a retry), and never stand in the way of a
// writer - an in-place write or an atomic replace goes through while the reader is open.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "Containers/String.h"
#include "Platform/BlockingFileReader.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/VirtualMemory.h"

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

// A literal's bytes, for the byte-span overloads: AsBytes("x") reads better in a call than
// FStringView("x").AsBytes().
std::span<const uint8> AsBytes (FStringView Text)
{
	return Text.AsBytes();
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

	// A sparse file of Size bytes (no disk space used) with a 16-byte pattern marker at each of
	// MarkerOffsets, zeros everywhere else. Sets bNoSparseFiles instead where the volume has none.
	void CreateSparseFile (const std::filesystem::path& File, uint64 Size, std::initializer_list<uint64> MarkerOffsets)
	{
		const HANDLE handle = CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		ASSERT_NE(handle, INVALID_HANDLE_VALUE);
		DWORD returned = 0u;
		if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0u, nullptr, 0u, &returned, nullptr))
		{
			CloseHandle(handle);
			bNoSparseFiles = true;
			return;
		}
		LARGE_INTEGER end = {};
		end.QuadPart = static_cast<LONGLONG>(Size);
		const bool bSized = SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
		bool bMarked = true;
		for (const uint64 offset : MarkerOffsets)
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
			bMarked = bMarked && WriteFile(handle, marker, sizeof(marker), &written, &overlapped) && written == sizeof(marker);
		}
		CloseHandle(handle);
		ASSERT_TRUE(bSized);
		ASSERT_TRUE(bMarked);
	}

	std::filesystem::path Dir;
	bool bNoSparseFiles = false;
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
	EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::NotFound);
	EXPECT_FALSE(reader.Open(Dir / FStringView("Немає").ToPath() / FStringView("теки.bin").ToPath()));
	EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::NotFound);

	error.Clear();
	EXPECT_FALSE(reader.Open(Dir, &error));
	EXPECT_FALSE(error.IsEmpty());
	EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::Other);
}

// The failures a caller retries after the watcher's settle window (ReadFileChunksStable does): a
// writer holding the file without sharing reads, and a writer's lock on the bytes being read.
TEST_F(BlockingFileReaderTest, ReportsAFileInUse)
{
	const std::filesystem::path file = Dir / FStringView("Зайнятий.bin").ToPath();
	ASSERT_TRUE(platform::WriteFileBytes(file, MakePattern(8192u)));

	platform::CBlockingFileReader reader;
	{
		const HANDLE exclusive = CreateFileW(file.c_str(), GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);
		const bool bOpened = reader.Open(file);
		CloseHandle(exclusive);
		EXPECT_FALSE(bOpened);
		EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::InUse);
	}

	ASSERT_TRUE(reader.Open(file));
	EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::None);
	const HANDLE writer = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(writer, INVALID_HANDLE_VALUE);
	OVERLAPPED range = {};
	range.Offset = 4096u;
	ASSERT_TRUE(LockFileEx(writer, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0u, 100u, 0u, &range));

	std::vector<uint8> buffer(256u);
	FString error;
	EXPECT_FALSE(reader.ReadAt(4000u, buffer, &error).has_value());
	EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::InUse) << error.CStr();
	// Outside the locked range reads go on.
	EXPECT_EQ(reader.ReadAt(0u, buffer), std::optional<uint64>(256u));
	EXPECT_EQ(reader.GetLastFailure(), platform::EFileFailure::None);

	UnlockFileEx(writer, 0u, 100u, 0u, &range);
	CloseHandle(writer);
	EXPECT_EQ(reader.ReadAt(4000u, buffer), std::optional<uint64>(256u));
	EXPECT_EQ(0, std::memcmp(buffer.data(), MakePattern(4256u).data() + 4000u, 256u));
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
	// 16-byte markers straddling 2 GiB and 4 GiB, and ending at the end of the file.
	ASSERT_NO_FATAL_FAILURE(CreateSparseFile(file, FileSize, { (1ull << 31u) - 8u, (1ull << 32u) - 8u, FileSize - 16u }));
	if (bNoSparseFiles)
	{
		GTEST_SKIP() << "the temp volume has no sparse files";
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

// A read larger than one ReadFile call carries (1 GiB, BlockingFileReaderWindows.cpp) is split, each
// call at its own file offset and its own place in the buffer: 1 GiB + 16 bytes from 3 GiB of the
// sparse file, so the second call starts at 4 GiB, in the middle of a marker. A wrong offset or
// buffer position in the split shows as a marker in the wrong place. The 1 GiB buffer is reserved
// and committed address space, so its pages cost memory only while the read fills them.
TEST_F(BlockingFileReaderTest, SplitsAReadLargerThanOneCall)
{
	constexpr uint64 FileSize = 5ull << 30u;
	constexpr uint64 Start = 3ull << 30u;
	constexpr uint64 Length = (1ull << 30u) + 16u;
	constexpr uint64 Split = 1ull << 30u;   // where the second call starts, relative to Start
	const std::filesystem::path file = Dir / FStringView("Розділене читання.bin").ToPath();
	ASSERT_NO_FATAL_FAILURE(CreateSparseFile(file, FileSize, { Start, Start + Split - 8u }));
	if (bNoSparseFiles)
	{
		GTEST_SKIP() << "the temp volume has no sparse files";
	}

	struct SReservation
	{
		uint8* Base = static_cast<uint8*>(platform::ReserveVirtual(Length));

		~SReservation ()
		{
			if (Base != nullptr)
			{
				platform::ReleaseVirtual(Base, Length);
			}
		}
	} reservation;
	uint8* const buffer = reservation.Base;
	ASSERT_NE(buffer, nullptr);
	if (!platform::CommitVirtual(buffer, Length))
	{
		GTEST_SKIP() << "cannot commit 1 GiB for the read";
	}
	std::memset(buffer + Split - 64u, 0xEE, 80u);   // not what the file holds there: the read must overwrite it

	platform::CBlockingFileReader reader;
	ASSERT_TRUE(reader.Open(file));
	FString error;
	const std::optional<uint64> read = reader.ReadAt(Start, std::span<uint8>(buffer, static_cast<size_t>(Length)), &error);
	EXPECT_EQ(read, std::optional<uint64>(Length)) << error.CStr();

	// The marker at the start (first call), the one across the split (both calls), zeros elsewhere.
	const auto expectMarker = [&] (uint64 Offset)
	{
		for (uint64 i = 0u; i < 16u; ++i)
		{
			EXPECT_EQ(buffer[Offset + i], PatternByte(Start + Offset + i)) << "buffer offset " << Offset + i;
		}
	};
	expectMarker(0u);
	expectMarker(Split - 8u);
	static const uint8 zeros[64 * 1024] = {};
	for (uint64 offset = 16u; offset < Split - 8u; offset += sizeof(zeros))
	{
		const size_t count = static_cast<size_t>(Split - 8u - offset < sizeof(zeros) ? Split - 8u - offset : sizeof(zeros));
		ASSERT_EQ(0, std::memcmp(buffer + offset, zeros, count)) << "near buffer offset " << offset;
	}
	EXPECT_EQ(0, std::memcmp(buffer + Split + 8u, zeros, 8u));
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
