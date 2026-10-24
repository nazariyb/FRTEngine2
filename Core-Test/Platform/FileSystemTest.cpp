// The asset system's file layer (Platform/FileSystem.h): identities follow a file through a rename
// and tell a copy apart, directory listings agree with Stat, WriteFileAtomic never leaves a torn
// file - not even when the writing process is killed at a random moment - and ReadFileChunksStable
// never calls a read stable when the file changed underneath it. Everything runs in directories
// with non-ASCII names, which every path conversion on the way has to survive.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "Containers/String.h"
#include "Containers/WideString.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace std::chrono_literals;


namespace
{
// Narrow text never becomes a path: the deleted twins catch every narrow spelling (Platform.h).
template <typename T>
concept CanStat = requires (const T& Path) { { platform::Stat(Path) }; };

template <typename T>
concept CanWriteAtomic = requires (const T& Path) { { platform::WriteFileAtomic(Path, FStringView("x")) }; };

template <typename T>
concept CanEnumerate = requires (const T& Path) { { platform::EnumerateDirectory(Path, [] (const platform::SDirectoryEntry&) {}) }; };

template <typename T>
concept CanReadStable = requires (const T& Path) { { platform::ReadFileChunksStable(Path, [] (std::span<const uint8>) {}) }; };

static_assert(CanStat<std::filesystem::path>);
static_assert(!CanStat<const char*>);
static_assert(!CanStat<std::string>);
static_assert(!CanStat<std::string_view>);
static_assert(!CanStat<FString>);
static_assert(!CanStat<FStringView>);
static_assert(CanWriteAtomic<std::filesystem::path>);
static_assert(!CanWriteAtomic<const char*>);
static_assert(!CanWriteAtomic<std::string>);
static_assert(CanEnumerate<std::filesystem::path>);
static_assert(!CanEnumerate<const char*>);
static_assert(CanReadStable<std::filesystem::path>);
static_assert(!CanReadStable<std::string_view>);


// Bytes that differ for every seed, so two versions of a file never compare equal by accident.
std::vector<uint8> MakeBytes (size_t Size, uint32 Seed)
{
	std::vector<uint8> bytes(Size);
	uint32 state = Seed * 2654435761u + 0x9E3779B9u;
	for (uint8& byte : bytes)
	{
		state ^= state << 13u;
		state ^= state >> 17u;
		state ^= state << 5u;
		byte = static_cast<uint8>(state >> 24u);
	}
	return bytes;
}

std::vector<uint8> ReadBytes (const std::filesystem::path& Path)
{
	std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path);
	return bytes ? std::move(*bytes) : std::vector<uint8>();
}

std::span<const uint8> AsBytes (FStringView Text)
{
	return std::span<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len());
}

// The names in Directory, sorted, as UTF-8.
std::vector<FString> ListNames (const std::filesystem::path& Directory)
{
	std::vector<FString> names;
	platform::EnumerateDirectory(Directory, [&names] (const platform::SDirectoryEntry& Entry) { names.emplace_back(Entry.Name); });
	std::sort(names.begin(), names.end());
	return names;
}

// What WriteFileAtomic's temporary files are called (FileSystem.h): ".<name>.<pid>-<n>.tmp".
bool IsTemporaryName (FStringView Name)
{
	return Name.StartsWith('.') && Name.EndsWith(".tmp");
}

// The last write time a file had an hour ago: a rewrite now is certain to change it, whatever the
// granularity of the file system's clock (FileSystem.h, SFileStat).
void AgeFile (const std::filesystem::path& Path)
{
	std::error_code ec;
	std::filesystem::last_write_time(Path, std::filesystem::file_time_type::clock::now() - 1h, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
}


// ---------------- The atomic writer child ----------------

// Version N of the file the atomic-writer child writes: its size and every byte follow from N, so a
// reader can tell one complete version from anything else - a torn mix of two, or a cut-off one.
// Sizes vary from 256 KiB to 1 MiB, so a write takes long enough to be killed in the middle.
std::vector<uint8> MakeAtomicVersion (uint32 Version)
{
	const size_t size = 256u * 1024u + (static_cast<size_t>(Version) * 40503u) % (768u * 1024u);
	std::vector<uint8> bytes = MakeBytes(size, Version);
	const FString header = FString::Format("FRT atomic v{}\n", Version);
	std::memcpy(bytes.data(), header.GetData(), header.Len());
	return bytes;
}

// The version Bytes are, when they are exactly one.
std::optional<uint32> ParseAtomicVersion (std::span<const uint8> Bytes)
{
	const FStringView text(reinterpret_cast<const char*>(Bytes.data()), static_cast<uint32>(Bytes.size() < 32u ? Bytes.size() : 32u));
	constexpr FStringLiteral Prefix = "FRT atomic v";
	const uint32 end = text.Find('\n');
	if (!text.StartsWith(Prefix) || end == FStringView::NotFound)
	{
		return std::nullopt;
	}
	const std::optional<uint32> version = text.Left(end).RightChop(Prefix.Len()).ToUInt32();
	if (!version.has_value())
	{
		return std::nullopt;
	}
	const std::vector<uint8> expected = MakeAtomicVersion(*version);
	if (expected.size() != Bytes.size() || std::memcmp(expected.data(), Bytes.data(), Bytes.size()) != 0)
	{
		return std::nullopt;
	}
	return version;
}

// "Core-Test --frt-test-child=atomic-writer <file>": rewrites the file with WriteFileAtomic, one
// version after the other, until it is killed. Starts after the version the file holds, so versions
// keep rising from child to child. Gives up after a minute, so it never outlives a parent that died
// without killing it (the parent's job object normally sees to that).
int RunAtomicWriter (std::span<const FString> Arguments)
{
	if (Arguments.empty())
	{
		return 2;
	}
	const std::filesystem::path target = Arguments[0].ToPath();

	uint32 version = 0u;
	if (const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(target))
	{
		version = ParseAtomicVersion(*bytes).value_or(0u);
	}

	const auto deadline = std::chrono::steady_clock::now() + 60s;
	while (std::chrono::steady_clock::now() < deadline)
	{
		++version;
		platform::WriteFileAtomic(target, MakeAtomicVersion(version), false);
	}
	return 0;
}

const bool bAtomicWriterRegistered = test::RegisterChildMode("atomic-writer", &RunAtomicWriter);


// A job object whose processes die when its last handle closes: a child started in it cannot
// outlive the test, whatever happens to the test.
class CKillOnCloseJob
{
public:
	CKillOnCloseJob ()
		: Job(CreateJobObjectW(nullptr, nullptr))
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (Job != nullptr && !SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		{
			CloseHandle(Job);
			Job = nullptr;
		}
	}

	~CKillOnCloseJob ()
	{
		if (Job != nullptr)
		{
			CloseHandle(Job);
		}
	}

	CKillOnCloseJob (const CKillOnCloseJob&) = delete;
	CKillOnCloseJob& operator= (const CKillOnCloseJob&) = delete;

	[[nodiscard]] HANDLE Get () const { return Job; }

private:
	HANDLE Job = nullptr;
};


// Every test works in a directory of this process's own with a non-ASCII name (PlatformTest.cpp has
// the reasons for a per-process parent).
class FileSystemTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Файли 文件 😀 {}", GetCurrentProcessId()).ToPath()
			/ "FileSystem";
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


// ---------------- Identity and stat ----------------

TEST_F(FileSystemTest, IdentitySurvivesARenameAndDiffersAfterACopy)
{
	const std::filesystem::path original = File("Оригінал 原件.bin");
	ASSERT_TRUE(platform::WriteFileBytes(original, MakeBytes(1000u, 1u)));
	const std::optional<platform::SFileIdentity> identity = platform::GetFileIdentity(original);
	ASSERT_TRUE(identity.has_value());
	EXPECT_TRUE(identity->IsValid());

	// A rename into another directory of the same volume keeps it.
	const std::filesystem::path renamed = File("Підтека 子目录") / FStringView("Перейменований 改名.bin").ToPath();
	std::error_code ec;
	std::filesystem::create_directories(renamed.parent_path(), ec);
	std::filesystem::rename(original, renamed, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	EXPECT_EQ(platform::GetFileIdentity(renamed), identity);
	EXPECT_FALSE(platform::GetFileIdentity(original).has_value());

	// A copy is another file with the same bytes.
	const std::filesystem::path copy = File("Копія 副本.bin");
	ASSERT_TRUE(std::filesystem::copy_file(renamed, copy, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	const std::optional<platform::SFileIdentity> copyIdentity = platform::GetFileIdentity(copy);
	ASSERT_TRUE(copyIdentity.has_value());
	EXPECT_NE(*copyIdentity, *identity);
	EXPECT_NE(copyIdentity->GetHash(), identity->GetHash());
	EXPECT_EQ(ReadBytes(copy), ReadBytes(renamed));

	// Directories have identities of their own too.
	const std::optional<platform::SFileIdentity> directory = platform::GetFileIdentity(renamed.parent_path());
	ASSERT_TRUE(directory.has_value());
	EXPECT_TRUE(directory->IsValid());
	EXPECT_NE(*directory, *identity);

	EXPECT_FALSE(platform::SFileIdentity().IsValid());
}

TEST_F(FileSystemTest, AtomicWritesGiveANewIdentityInPlaceWritesKeepIt)
{
	const std::filesystem::path file = File("Ідентичність.bin");
	ASSERT_TRUE(platform::WriteFileBytes(file, MakeBytes(100u, 1u)));
	const std::optional<platform::SFileIdentity> first = platform::GetFileIdentity(file);
	ASSERT_TRUE(first.has_value());

	ASSERT_TRUE(platform::WriteFileBytes(file, MakeBytes(200u, 2u)));
	EXPECT_EQ(platform::GetFileIdentity(file), first);

	ASSERT_TRUE(platform::WriteFileAtomic(file, MakeBytes(300u, 3u)));
	const std::optional<platform::SFileIdentity> replaced = platform::GetFileIdentity(file);
	ASSERT_TRUE(replaced.has_value());
	EXPECT_NE(*replaced, *first);
}

TEST_F(FileSystemTest, StatDescribesFilesAndDirectories)
{
	const std::filesystem::path file = File("Стат 統計.bin");
	ASSERT_TRUE(platform::WriteFileBytes(file, MakeBytes(12345u, 7u)));
	AgeFile(file);

	const std::optional<platform::SFileStat> stat = platform::Stat(file);
	ASSERT_TRUE(stat.has_value());
	EXPECT_EQ(stat->Size, 12345u);
	EXPECT_FALSE(stat->bDirectory);
	EXPECT_FALSE(stat->bReadOnly);
	EXPECT_EQ(stat->Identity, platform::GetFileIdentity(file));
	EXPECT_NE(stat->CreationTime, 0);

	// The documented unit: MSVC's file_time_type ticks (100 ns since 1601).
	std::error_code ec;
	EXPECT_EQ(stat->LastWriteTime, std::filesystem::last_write_time(file, ec).time_since_epoch().count());

	const FWideString wideFile(FString::FromPath(file));
	ASSERT_TRUE(SetFileAttributesW(wideFile.CStr(), FILE_ATTRIBUTE_READONLY));
	const std::optional<platform::SFileStat> readOnly = platform::Stat(file);
	SetFileAttributesW(wideFile.CStr(), FILE_ATTRIBUTE_NORMAL);
	ASSERT_TRUE(readOnly.has_value());
	EXPECT_TRUE(readOnly->bReadOnly);

	const std::optional<platform::SFileStat> directory = platform::Stat(Dir);
	ASSERT_TRUE(directory.has_value());
	EXPECT_TRUE(directory->bDirectory);
	EXPECT_EQ(directory->Size, 0u);

	FString error;
	EXPECT_FALSE(platform::Stat(File("Немає.bin"), &error).has_value());
	EXPECT_TRUE(error.Contains("Немає.bin")) << error.CStr();
}

TEST_F(FileSystemTest, StatIsNotRefusedByAnExclusiveOpen)
{
	const std::filesystem::path file = File("Замкнений.bin");
	ASSERT_TRUE(platform::WriteFileBytes(file, MakeBytes(10u, 1u)));

	// Another process holding the file with no sharing at all (a writer that locks it).
	const HANDLE exclusive = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);
	const std::optional<platform::SFileStat> stat = platform::Stat(file);
	CloseHandle(exclusive);
	ASSERT_TRUE(stat.has_value());
	EXPECT_EQ(stat->Size, 10u);
}


// ---------------- Directory listings ----------------

TEST_F(FileSystemTest, ListingMatchesStat)
{
	// Cyrillic, CJK, an emoji (a surrogate pair), an unpaired surrogate (allowed by NTFS, WTF-8 in
	// the listing), an empty file, a file over 2 MB, a directory.
	std::vector<std::filesystem::path> files =
	{
		File("Звичайний.txt"), File("漢字 😀.bin"), File("порожній"), File("Великий файл.bin"),
		Dir / std::filesystem::path(L"Lone \xD800 surrogate.txt"),
	};
	ASSERT_TRUE(platform::WriteFileBytes(files[0], AsBytes("abc")));
	ASSERT_TRUE(platform::WriteFileBytes(files[1], MakeBytes(4321u, 2u)));
	ASSERT_TRUE(platform::WriteFileBytes(files[2], {}));
	ASSERT_TRUE(platform::WriteFileBytes(files[3], MakeBytes(3u * 1024u * 1024u + 1u, 3u)));
	ASSERT_TRUE(platform::WriteFileBytes(files[4], AsBytes("x")));
	const std::filesystem::path directory = File("Тека 文件夹");
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directory(directory, ec));
	files.push_back(directory);

	std::set<FString> expected;
	for (const std::filesystem::path& file : files)
	{
		expected.insert(FString::FromPath(file.filename()));
	}

	std::set<FString> seen;
	FString error;
	const bool bListed = platform::EnumerateDirectory(Dir, [&] (const platform::SDirectoryEntry& Entry)
	{
		EXPECT_TRUE(seen.insert(FString(Entry.Name)).second) << "listed twice: " << FString(Entry.Name).CStr();
		const std::optional<platform::SFileStat> stat = platform::Stat(Dir / Entry.Name.ToPath());
		ASSERT_TRUE(stat.has_value()) << FString(Entry.Name).CStr();
		EXPECT_EQ(Entry.Stat.Identity, stat->Identity) << FString(Entry.Name).CStr();
		EXPECT_EQ(Entry.Stat.Size, stat->Size) << FString(Entry.Name).CStr();
		EXPECT_EQ(Entry.Stat.LastWriteTime, stat->LastWriteTime) << FString(Entry.Name).CStr();
		EXPECT_EQ(Entry.Stat.CreationTime, stat->CreationTime) << FString(Entry.Name).CStr();
		EXPECT_EQ(Entry.Stat.bDirectory, stat->bDirectory) << FString(Entry.Name).CStr();
		EXPECT_FALSE(Entry.bReparsePoint);
	}, &error);
	EXPECT_TRUE(bListed) << error.CStr();
	EXPECT_EQ(seen, expected);   // no "." or "..", and every name round-trips
}

TEST_F(FileSystemTest, ListingOfALargeDirectoryIsComplete)
{
	// Long names: about 300 entries fit one 64 KB listing call, so this takes several.
	constexpr uint32 Count = 1200u;
	for (uint32 i = 0u; i < Count; ++i)
	{
		ASSERT_TRUE(platform::WriteFileBytes(File(FString::Format("Файл з досить довгою назвою, номер {:05}.txt", i)), {}, false));
	}

	uint32 listed = 0u;
	std::set<FString> names;
	std::vector<platform::SFileIdentity> identities;
	ASSERT_TRUE(platform::EnumerateDirectory(Dir, [&] (const platform::SDirectoryEntry& Entry)
	{
		++listed;
		names.insert(FString(Entry.Name));
		identities.push_back(Entry.Stat.Identity);
	}));
	EXPECT_EQ(listed, Count);
	EXPECT_EQ(names.size(), Count);
	for (size_t i = 1u; i < identities.size(); ++i)
	{
		ASSERT_NE(identities[i], identities[i - 1u]);
	}
}

TEST_F(FileSystemTest, ListingStopsWhenTheCallbackSaysSoAndReportsErrors)
{
	for (uint32 i = 0u; i < 5u; ++i)
	{
		ASSERT_TRUE(platform::WriteFileBytes(File(FString::Format("{}.txt", i)), {}));
	}
	uint32 calls = 0u;
	EXPECT_TRUE(platform::EnumerateDirectory(Dir, [&calls] (const platform::SDirectoryEntry&) { return ++calls < 2u; }));
	EXPECT_EQ(calls, 2u);

	FString error;
	EXPECT_FALSE(platform::EnumerateDirectory(File("Немає"), [] (const platform::SDirectoryEntry&) {}, &error));
	EXPECT_TRUE(error.Contains("Немає")) << error.CStr();

	// A file is no directory.
	error.Clear();
	EXPECT_FALSE(platform::EnumerateDirectory(File("0.txt"), [] (const platform::SDirectoryEntry&) {}, &error));
	EXPECT_FALSE(error.IsEmpty());

	// An empty directory lists nothing and succeeds.
	const std::filesystem::path empty = File("Порожня");
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directory(empty, ec));
	EXPECT_TRUE(ListNames(empty).empty());
	EXPECT_TRUE(platform::EnumerateDirectory(empty, [] (const platform::SDirectoryEntry&) { ADD_FAILURE(); }));
}


// ---------------- Writes ----------------

TEST_F(FileSystemTest, WriteFileBytesRoundTrip)
{
	const std::filesystem::path file = File("Байти") / FStringView("дані 😀.bin").ToPath();
	const std::vector<uint8> bytes = MakeBytes(70000u, 4u);
	ASSERT_TRUE(platform::WriteFileBytes(file, bytes));   // creates the directory
	EXPECT_EQ(ReadBytes(file), bytes);

	ASSERT_TRUE(platform::WriteFileBytes(file, AsBytes("short")));
	EXPECT_EQ(ReadBytes(file), std::vector<uint8>(AsBytes("short").begin(), AsBytes("short").end()));

	FString error;
	EXPECT_FALSE(platform::WriteFileBytes(File("Немає") / "a.bin", AsBytes("x"), false, &error));
	EXPECT_FALSE(error.IsEmpty());
}

TEST_F(FileSystemTest, WriteFileAtomicReplacesAndLeavesNoTemporaryFile)
{
	const std::filesystem::path subdirectory = File("Атомарно 原子");
	const std::filesystem::path file = subdirectory / FStringView("Мета 😀.meta").ToPath();
	const std::vector<uint8> first = MakeBytes(5000u, 1u);
	ASSERT_TRUE(platform::WriteFileAtomic(file, first));   // creates the directory
	EXPECT_EQ(ReadBytes(file), first);

	const std::vector<uint8> second = MakeBytes(100u, 2u);
	ASSERT_TRUE(platform::WriteFileAtomic(file, second));
	EXPECT_EQ(ReadBytes(file), second);

	ASSERT_TRUE(platform::WriteFileAtomic(file, FStringView("Guid: текст\n")));
	EXPECT_EQ(platform::ReadFileText(file).value_or("<unreadable>"), "Guid: текст\n");

	ASSERT_TRUE(platform::WriteFileAtomic(file, std::span<const uint8>()));
	EXPECT_TRUE(ReadBytes(file).empty());

	// Only the file itself: every temporary file was renamed into place.
	EXPECT_EQ(ListNames(subdirectory), std::vector<FString>{ FString("Мета 😀.meta") });

	// A name longer than the part of it the temporary file's name keeps (64 UTF-16 units), cut where
	// a surrogate pair would be split ("a" puts every emoji's high surrogate at an odd unit). Short
	// enough for the whole path to stay within MAX_PATH, which the engine does not lift.
	const std::filesystem::path longName = File("a" + FString::Repeat("😀", 60u) + ".txt");
	FString error;
	ASSERT_TRUE(platform::WriteFileAtomic(longName, AsBytes("long"), true, &error)) << error.CStr();
	EXPECT_EQ(ReadBytes(longName).size(), 4u);
}

TEST_F(FileSystemTest, WriteFileAtomicRefusesAReadOnlyFileAndKeepsIt)
{
	const std::filesystem::path file = File("Тільки читання.txt");
	ASSERT_TRUE(platform::WriteFileAtomic(file, AsBytes("old")));
	const FWideString wideFile(FString::FromPath(file));
	ASSERT_TRUE(SetFileAttributesW(wideFile.CStr(), FILE_ATTRIBUTE_READONLY));

	const auto start = std::chrono::steady_clock::now();
	FString error;
	const bool bWritten = platform::WriteFileAtomic(file, AsBytes("new"), true, &error);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	SetFileAttributesW(wideFile.CStr(), FILE_ATTRIBUTE_NORMAL);

	EXPECT_FALSE(bWritten);
	EXPECT_FALSE(error.IsEmpty());
	EXPECT_LT(elapsed, 500ms);   // no retrying: it will not clear by itself
	EXPECT_EQ(platform::ReadFileText(file).value_or("<unreadable>"), "old");
	EXPECT_EQ(ListNames(Dir), std::vector<FString>{ FString("Тільки читання.txt") });
}

TEST_F(FileSystemTest, ReplaceFileAtomicWaitsForAHolderToLetGo)
{
	const std::filesystem::path target = File("Ціль.txt");
	const std::filesystem::path temp = File("Тимчасовий.tmp");
	ASSERT_TRUE(platform::WriteFileBytes(target, AsBytes("old")));
	ASSERT_TRUE(platform::WriteFileBytes(temp, AsBytes("new")));

	// Held open without FILE_SHARE_DELETE, as an indexer or a viewer may: the replace fails until the
	// handle is closed, 150 ms later.
	const HANDLE holder = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(holder, INVALID_HANDLE_VALUE);
	std::thread release([holder] { std::this_thread::sleep_for(150ms); CloseHandle(holder); });

	FString error;
	const bool bReplaced = platform::ReplaceFileAtomic(temp, target, &error);
	release.join();
	EXPECT_TRUE(bReplaced) << error.CStr();
	EXPECT_EQ(platform::ReadFileText(target).value_or("<unreadable>"), "new");
	EXPECT_FALSE(std::filesystem::exists(temp));
}

TEST_F(FileSystemTest, ReplaceFileAtomicGivesUpOnAFileThatStaysHeld)
{
	const std::filesystem::path target = File("Ціль.txt");
	const std::filesystem::path temp = File("Тимчасовий.tmp");
	ASSERT_TRUE(platform::WriteFileBytes(target, AsBytes("old")));
	ASSERT_TRUE(platform::WriteFileBytes(temp, AsBytes("new")));

	const HANDLE holder = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(holder, INVALID_HANDLE_VALUE);
	FString error;
	const bool bReplaced = platform::ReplaceFileAtomic(temp, target, &error);
	CloseHandle(holder);

	EXPECT_FALSE(bReplaced);
	EXPECT_TRUE(error.Contains("Ціль.txt")) << error.CStr();
	// Both untouched: the temporary file is the caller's to delete or retry.
	EXPECT_EQ(platform::ReadFileText(target).value_or("<unreadable>"), "old");
	EXPECT_EQ(platform::ReadFileText(temp).value_or("<unreadable>"), "new");
}

// The test I3 asks for: a writer killed at random moments, as a crash or a power cut would stop it,
// never leaves the file torn. Each round starts Core-Test again as an atomic-writer child (in a job
// that kills it if this process dies), waits until it is writing, lets it run a random few
// milliseconds more and kills it with TerminateProcess - no cleanup runs, whatever it was doing.
// The file must then hold one complete version, newer than before; temporary files of the killed
// writes may be left behind (they show the kills landed mid-write), nothing else.
TEST_F(FileSystemTest, WriteFileAtomicSurvivesAKilledWriter)
{
	const std::filesystem::path target = File("Атомарний запис 原子.bin");
	ASSERT_TRUE(platform::WriteFileAtomic(target, MakeAtomicVersion(0u)));

	const CKillOnCloseJob job;
	ASSERT_NE(job.Get(), nullptr);

	const uint32 seed = std::random_device()();
	SCOPED_TRACE(::testing::Message() << "seed " << seed);
	std::mt19937 random(seed);
	std::uniform_int_distribution<uint32> extraMs(0u, 40u);

	constexpr uint32 Rounds = 16u;
	uint32 version = 0u;
	for (uint32 round = 0u; round < Rounds; ++round)
	{
		const std::optional<platform::SFileIdentity> identityBefore = platform::GetFileIdentity(target);
		ASSERT_TRUE(identityBefore.has_value());

		FWideString commandLine(FString::Format("\"{}\" {}atomic-writer \"{}\"", FString::FromPath(platform::GetExecutablePath()),
			test::ChildModeSwitch, FString::FromPath(target)).View());
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process = {};
		ASSERT_TRUE(CreateProcessW(nullptr, commandLine.GetData(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
			nullptr, &startup, &process)) << platform::GetSystemErrorMessage(GetLastError()).CStr();
		const bool bInJob = AssignProcessToJobObject(job.Get(), process.hProcess) != 0;
		ResumeThread(process.hThread);

		// Writing: its first replace gave the file a new identity.
		const auto deadline = std::chrono::steady_clock::now() + 20s;
		bool bWriting = false;
		while (!bWriting && std::chrono::steady_clock::now() < deadline && WaitForSingleObject(process.hProcess, 0u) == WAIT_TIMEOUT)
		{
			const std::optional<platform::SFileIdentity> identity = platform::GetFileIdentity(target);
			bWriting = identity.has_value() && *identity != *identityBefore;
			if (!bWriting)
			{
				std::this_thread::sleep_for(1ms);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(extraMs(random)));

		TerminateProcess(process.hProcess, 9u);
		WaitForSingleObject(process.hProcess, 10'000u);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		ASSERT_TRUE(bInJob);
		ASSERT_TRUE(bWriting) << "round " << round << ": the child never wrote";

		const std::vector<uint8> bytes = ReadBytes(target);
		const std::optional<uint32> found = ParseAtomicVersion(bytes);
		ASSERT_TRUE(found.has_value()) << "round " << round << ": the file holds no complete version (" << bytes.size() << " bytes)";
		EXPECT_GT(*found, version) << "round " << round;
		version = *found;
	}

	uint32 temporaryFiles = 0u;
	for (const FString& name : ListNames(Dir))
	{
		if (name == FString::FromPath(target.filename()))
		{
			continue;
		}
		EXPECT_TRUE(IsTemporaryName(name)) << name.CStr();
		++temporaryFiles;
	}
	// At least one kill landed between creating a temporary file and renaming it: the test killed
	// writes in progress, not only a writer between writes.
	EXPECT_GT(temporaryFiles, 0u);
	RecordProperty("LastVersion", static_cast<int>(version));
	RecordProperty("TemporaryFilesLeft", static_cast<int>(temporaryFiles));
}


// ---------------- Stable reads ----------------

TEST_F(FileSystemTest, StableReadDeliversEveryByteOfAnUnchangedFile)
{
	const std::filesystem::path file = File("Хеш 哈希.bin");
	const std::vector<uint8> bytes = MakeBytes(3u * 1024u * 1024u + 512u * 1024u, 5u);
	ASSERT_TRUE(platform::WriteFileBytes(file, bytes));

	std::vector<uint8> delivered;
	std::vector<size_t> chunks;
	const platform::EStableRead result = platform::ReadFileChunksStable(file, [&] (std::span<const uint8> Chunk)
	{
		delivered.insert(delivered.end(), Chunk.begin(), Chunk.end());
		chunks.push_back(Chunk.size());
	});
	EXPECT_EQ(result, platform::EStableRead::Stable);
	EXPECT_EQ(delivered, bytes);
	EXPECT_EQ(chunks, (std::vector<size_t>{ 1048576u, 1048576u, 1048576u, 524288u }));

	// Smaller chunks on request.
	chunks.clear();
	EXPECT_EQ(platform::ReadFileChunksStable(file, [&] (std::span<const uint8> Chunk) { chunks.push_back(Chunk.size()); }, 1500000u),
		platform::EStableRead::Stable);
	EXPECT_EQ(chunks, (std::vector<size_t>{ 1500000u, 1500000u, 670016u }));

	// An empty file is stable and delivers nothing.
	const std::filesystem::path empty = File("Порожній");
	ASSERT_TRUE(platform::WriteFileBytes(empty, {}));
	EXPECT_EQ(platform::ReadFileChunksStable(empty, [] (std::span<const uint8>) { ADD_FAILURE(); }), platform::EStableRead::Stable);

	EXPECT_EQ(platform::ReadFileChunksStable(File("Немає"), [] (std::span<const uint8>) {}), platform::EStableRead::Failed);
	EXPECT_EQ(platform::ReadFileChunksStable(Dir, [] (std::span<const uint8>) {}), platform::EStableRead::Failed);
}

// The rewrite-during-hash tests: another thread rewrites the file while the first chunk is being
// hashed, and the result must not be Stable - whatever was computed from the chunks is stale.
TEST_F(FileSystemTest, StableReadReportsAnInPlaceRewrite)
{
	const std::filesystem::path file = File("Перезапис.bin");
	const std::vector<uint8> bytes = MakeBytes(3u * 1024u * 1024u, 6u);
	ASSERT_TRUE(platform::WriteFileBytes(file, bytes));
	AgeFile(file);

	bool bRewritten = false;
	const platform::EStableRead result = platform::ReadFileChunksStable(file, [&] (std::span<const uint8>)
	{
		if (!bRewritten)
		{
			// Same size, other bytes, in place (same identity): only the write time tells.
			std::thread writer([&] { bRewritten = platform::WriteFileBytes(file, MakeBytes(bytes.size(), 7u)); });
			writer.join();
		}
	});
	ASSERT_TRUE(bRewritten);
	EXPECT_EQ(result, platform::EStableRead::Changed);
}

TEST_F(FileSystemTest, StableReadReportsAnAtomicReplace)
{
	const std::filesystem::path file = File("Заміна.bin");
	const std::vector<uint8> bytes = MakeBytes(3u * 1024u * 1024u, 8u);
	ASSERT_TRUE(platform::WriteFileBytes(file, bytes));

	bool bReplaced = false;
	std::vector<uint8> delivered;
	const platform::EStableRead result = platform::ReadFileChunksStable(file, [&] (std::span<const uint8> Chunk)
	{
		delivered.insert(delivered.end(), Chunk.begin(), Chunk.end());
		if (!bReplaced)
		{
			std::thread writer([&] { bReplaced = platform::WriteFileAtomic(file, MakeBytes(bytes.size(), 9u)); });
			writer.join();
		}
	});
	ASSERT_TRUE(bReplaced);
	EXPECT_EQ(result, platform::EStableRead::Changed);
	// The open reader kept reading the old file to its end: consistent, but no longer the file.
	EXPECT_EQ(delivered, bytes);
}

TEST_F(FileSystemTest, StableReadReportsATruncation)
{
	const std::filesystem::path file = File("Обрізаний.bin");
	ASSERT_TRUE(platform::WriteFileBytes(file, MakeBytes(3u * 1024u * 1024u, 10u)));

	bool bTruncated = false;
	const platform::EStableRead result = platform::ReadFileChunksStable(file, [&] (std::span<const uint8>)
	{
		if (!bTruncated)
		{
			std::thread writer([&] { bTruncated = platform::WriteFileBytes(file, MakeBytes(1000u, 11u)); });
			writer.join();
		}
	});
	ASSERT_TRUE(bTruncated);
	EXPECT_EQ(result, platform::EStableRead::Changed);
}

// Many reads while a writer keeps replacing the file, pausing a random few milliseconds between
// versions so that some reads fall between two writes and others across one: every read called
// Stable must have delivered exactly one complete version - the hash a scan would store is never of
// a mix. The deterministic cases above cover Changed; this one needs some Stable reads to mean
// anything.
TEST_F(FileSystemTest, StableReadNeverCallsAMixedReadStable)
{
	const std::filesystem::path file = File("Гонка 竞争.bin");
	ASSERT_TRUE(platform::WriteFileAtomic(file, MakeAtomicVersion(1u)));

	const uint32 seed = std::random_device()();
	SCOPED_TRACE(::testing::Message() << "seed " << seed);
	std::atomic<bool> bStop = false;
	std::thread writer([&]
	{
		std::mt19937 random(seed);
		std::uniform_int_distribution<uint32> pauseMs(0u, 8u);
		for (uint32 version = 2u; !bStop.load(); ++version)
		{
			platform::WriteFileAtomic(file, MakeAtomicVersion(version));
			std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs(random)));
		}
	});

	uint32 stable = 0u;
	uint32 changed = 0u;
	const auto deadline = std::chrono::steady_clock::now() + 400ms;
	while (std::chrono::steady_clock::now() < deadline)
	{
		std::vector<uint8> delivered;
		const platform::EStableRead result = platform::ReadFileChunksStable(file, [&] (std::span<const uint8> Chunk)
		{
			delivered.insert(delivered.end(), Chunk.begin(), Chunk.end());
		}, 64u * 1024u);
		if (result == platform::EStableRead::Stable)
		{
			++stable;
			EXPECT_TRUE(ParseAtomicVersion(delivered).has_value()) << delivered.size() << " bytes";
		}
		else
		{
			EXPECT_EQ(result, platform::EStableRead::Changed);
			++changed;
		}
	}
	bStop = true;
	writer.join();
	EXPECT_GT(stable, 0u);
	RecordProperty("Stable", static_cast<int>(stable));
	RecordProperty("Changed", static_cast<int>(changed));
}


// ---------------- Recycle Bin ----------------

// Restoring from the Recycle Bin is not scriptable, so this checks the half that is: the file is
// gone from its directory and MoveToTrash says it reached the Bin. Each run leaves two small files
// named "FRT Core-Test ..." in the user's Recycle Bin.
TEST_F(FileSystemTest, MoveToTrashRemovesTheFile)
{
	const std::filesystem::path file = File("FRT Core-Test кошик 🗑.txt");
	ASSERT_TRUE(platform::WriteFileBytes(file, AsBytes("в кошик")));
	FString error;
	EXPECT_TRUE(platform::MoveToTrash(file, &error)) << error.CStr();
	EXPECT_FALSE(std::filesystem::exists(file));

	error.Clear();
	EXPECT_FALSE(platform::MoveToTrash(File("Немає.txt"), &error));
	EXPECT_FALSE(error.IsEmpty());

	// From a thread in the multithreaded apartment, where IFileOperation cannot run itself.
	const std::filesystem::path other = File("FRT Core-Test кошик MTA.txt");
	ASSERT_TRUE(platform::WriteFileBytes(other, AsBytes("в кошик")));
	bool bTrashed = false;
	std::thread mta([&]
	{
		const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		bTrashed = platform::MoveToTrash(other, &error);
		if (SUCCEEDED(init))
		{
			CoUninitialize();
		}
	});
	mta.join();
	EXPECT_TRUE(bTrashed) << error.CStr();
	EXPECT_FALSE(std::filesystem::exists(other));
}


// ---------------- Volumes ----------------

TEST_F(FileSystemTest, VolumeInfoOfTheTempDirectory)
{
	FString error;
	const std::optional<platform::SVolumeInfo> volume = platform::QueryVolumeInfo(Dir, &error);
	ASSERT_TRUE(volume.has_value()) << error.CStr();
	EXPECT_FALSE(volume->FileSystemName.IsEmpty());
	EXPECT_GT(volume->MaxComponentLength, 0u);
	EXPECT_FALSE(volume->bRemote);
	EXPECT_FALSE(volume->bReadOnly);
	EXPECT_FALSE(volume->bDirectoryCaseSensitive);   // a fresh directory never is
	EXPECT_TRUE(volume->bCasePreserving);
	if (volume->FileSystemName == "NTFS")
	{
		// NTFS can hold names that differ only in case, but lookups here do not tell them apart.
		EXPECT_TRUE(volume->bVolumeCaseSensitive);
	}
	EXPECT_EQ(volume->bCaseSensitive, volume->bVolumeCaseSensitive && volume->bDirectoryCaseSensitive)
		<< "unless this system turned the kernel's obcaseinsensitive off";

	// A file: the facts of its directory.
	const std::filesystem::path file = File("Том.txt");
	ASSERT_TRUE(platform::WriteFileBytes(file, AsBytes("x")));
	const std::optional<platform::SVolumeInfo> ofFile = platform::QueryVolumeInfo(file);
	ASSERT_TRUE(ofFile.has_value());
	EXPECT_EQ(ofFile->FileSystemName, volume->FileSystemName);
	EXPECT_EQ(ofFile->bCaseSensitive, volume->bCaseSensitive);

	error.Clear();
	EXPECT_FALSE(platform::QueryVolumeInfo(File("Немає"), &error).has_value());
	EXPECT_FALSE(error.IsEmpty());
}

// The per-directory attribute of "fsutil file setCaseSensitiveInfo": set on an empty directory, it
// makes names that differ only in case two files there. Setting it needs NTFS and, on some Windows
// versions, the WSL feature: skipped where it cannot be set.
TEST_F(FileSystemTest, VolumeInfoSeesACaseSensitiveDirectory)
{
	const std::filesystem::path directory = File("Чутлива до регістру");
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directory(directory, ec));

	const HANDLE handle = CreateFileW(directory.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	ASSERT_NE(handle, INVALID_HANDLE_VALUE);
	FILE_CASE_SENSITIVE_INFO caseInfo = {};
	caseInfo.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
	const bool bSet = SetFileInformationByHandle(handle, FileCaseSensitiveInfo, &caseInfo, sizeof(caseInfo)) != 0;
	const DWORD setError = GetLastError();
	CloseHandle(handle);
	if (!bSet)
	{
		GTEST_SKIP() << "cannot make a directory case-sensitive here: " << platform::GetSystemErrorMessage(setError).CStr();
	}

	const std::optional<platform::SVolumeInfo> volume = platform::QueryVolumeInfo(directory);
	ASSERT_TRUE(volume.has_value());
	EXPECT_TRUE(volume->bDirectoryCaseSensitive);
	EXPECT_TRUE(volume->bCaseSensitive);

	// And it is so: two files.
	const std::filesystem::path upper = directory / FStringView("Ім'я.txt").ToPath();
	const std::filesystem::path lower = directory / FStringView("ім'я.txt").ToPath();
	ASSERT_TRUE(platform::WriteFileBytes(upper, AsBytes("upper")));
	ASSERT_TRUE(platform::WriteFileBytes(lower, AsBytes("lower")));
	EXPECT_EQ(ListNames(directory).size(), 2u);
	EXPECT_NE(platform::GetFileIdentity(upper), platform::GetFileIdentity(lower));

	// Its parent is not.
	const std::optional<platform::SVolumeInfo> parent = platform::QueryVolumeInfo(Dir);
	ASSERT_TRUE(parent.has_value());
	EXPECT_FALSE(parent->bDirectoryCaseSensitive);
}
