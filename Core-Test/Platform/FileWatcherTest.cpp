// The platform's change events (Platform/FileWatcher.h) map the OS's reports onto the portable kinds
// exactly as the measured Windows facts say (gap-watch-meta §1.3): a created, written and deleted file
// carries the identity Stat reports; a rename in one directory is RenamedOld then RenamedNew with one
// identity; a move between directories is Removed then Added with one identity; a case-only rename
// loses its leading Removed; a directory moved in is one event; a buffer too small for the burst is
// one Overflow; a deleted or renamed root is RootLost and, once it is back, an Overflow; Disarm drops
// what is queued; and the writer probe tells a held file from a free one. Everything runs in
// directories with non-ASCII names. The tests never assume an exact event list - other processes
// (indexers, scanners) may touch files too - only the facts that matter.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/FileWatcher.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

#include <Windows.h>

using namespace frt;
using namespace std::chrono_literals;


namespace
{
template <typename T>
concept CanProbe = requires (const T& Path) { { platform::ProbeFileWriters(Path) }; };

static_assert(CanProbe<std::filesystem::path>);
static_assert(!CanProbe<const char*>);
static_assert(!CanProbe<FString>);

template <typename T>
concept CanArm = requires (platform::IFileWatcher& Watcher, const T& Path) { { Watcher.Arm(Path) }; };

static_assert(CanArm<std::filesystem::path>);
static_assert(!CanArm<const char*>);
static_assert(!CanArm<FStringView>);


const char* KindName (platform::EFileEventKind Kind)
{
	switch (Kind)
	{
	case platform::EFileEventKind::Added: return "Added";
	case platform::EFileEventKind::Removed: return "Removed";
	case platform::EFileEventKind::Modified: return "Modified";
	case platform::EFileEventKind::RenamedOld: return "RenamedOld";
	case platform::EFileEventKind::RenamedNew: return "RenamedNew";
	case platform::EFileEventKind::Overflow: return "Overflow";
	case platform::EFileEventKind::RootLost: return "RootLost";
	}
	return "?";
}

// Every event seen so far, one per line, for failure messages.
FString Describe (const std::vector<platform::SFileEvent>& Events)
{
	FString text;
	for (const platform::SFileEvent& event : Events)
	{
		text.AppendFormat("  {} {} '{}'\n", event.RootId, KindName(event.Kind), event.Path);
	}
	return text;
}

// Takes a watcher's events until Done(all events so far) holds or Timeout passes.
class CEventLog
{
public:
	explicit CEventLog (platform::IFileWatcher& InWatcher) : Watcher(InWatcher) {}

	bool WaitFor (const std::function<bool (const std::vector<platform::SFileEvent>&)>& Done, std::chrono::milliseconds Timeout = 5s)
	{
		const auto deadline = std::chrono::steady_clock::now() + Timeout;
		for (;;)
		{
			Watcher.TakeEvents(Events);
			if (Done(Events))
			{
				return true;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
			{
				return false;
			}
			Watcher.WaitForEvents(std::chrono::duration<double>(deadline - now).count());
		}
	}

	// Takes what arrives within For, for checks that something did NOT happen.
	void Collect (std::chrono::milliseconds For)
	{
		const auto deadline = std::chrono::steady_clock::now() + For;
		while (std::chrono::steady_clock::now() < deadline)
		{
			Watcher.WaitForEvents(0.02);
			Watcher.TakeEvents(Events);
		}
	}

	void Clear () { Events.clear(); }

	std::vector<platform::SFileEvent> Events;

private:
	platform::IFileWatcher& Watcher;
};

// The index of the first event at or after Start of that kind and path, or -1.
int64 Find (const std::vector<platform::SFileEvent>& Events, platform::EFileEventKind Kind, FStringView Path, size_t Start = 0u)
{
	for (size_t i = Start; i < Events.size(); ++i)
	{
		if (Events[i].Kind == Kind && Events[i].Path == Path)
		{
			return static_cast<int64>(i);
		}
	}
	return -1;
}

bool Has (const std::vector<platform::SFileEvent>& Events, platform::EFileEventKind Kind, FStringView Path)
{
	return Find(Events, Kind, Path) >= 0;
}

bool HasKind (const std::vector<platform::SFileEvent>& Events, platform::EFileEventKind Kind)
{
	for (const platform::SFileEvent& event : Events)
	{
		if (event.Kind == Kind)
		{
			return true;
		}
	}
	return false;
}

void WriteText (const std::filesystem::path& Path, FStringView Text)
{
	ASSERT_TRUE(platform::WriteFileBytes(Path, Text.AsBytes())) << FString::FromPath(Path).CStr();
}


// Every test works in a directory of this process's own with a non-ASCII name (PlatformTest.cpp has
// the reasons for a per-process parent).
class FileWatcherTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Спостерігач 监视 😀 {}", platform::GetCurrentProcessId()).ToPath()
			/ "FileWatcher";
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		Root = Dir / FStringView("Корінь 根").ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(Root, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		Watcher.reset();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	// A watcher armed on Root.
	uint32 Start (const platform::SFileWatcherDesc& Desc = {})
	{
		Watcher = platform::CreateFileWatcher(Desc);
		FString error;
		const uint32 id = Watcher->Arm(Root, &error);
		EXPECT_NE(id, 0u) << error.CStr();
		Log = std::make_unique<CEventLog>(*Watcher);
		return id;
	}

	[[nodiscard]] std::filesystem::path File (FStringView RelativePath) const
	{
		return Root / RelativePath.ToPath();
	}

	std::filesystem::path Dir;
	std::filesystem::path Root;
	std::unique_ptr<platform::IFileWatcher> Watcher;
	std::unique_ptr<CEventLog> Log;
};
}


TEST_F(FileWatcherTest, ReportsACreatedWrittenAndDeletedFileWithItsIdentity)
{
	const uint32 id = Start();
	const FStringView name = "Файл 文件.txt";

	WriteText(File(name), "one");
	const std::optional<platform::SFileStat> stat = platform::Stat(File(name));
	ASSERT_TRUE(stat.has_value());
	ASSERT_TRUE(Log->WaitFor([&] (const auto& Events) { return Has(Events, platform::EFileEventKind::Added, name); }))
		<< Describe(Log->Events).CStr();
	const platform::SFileEvent& added = Log->Events[static_cast<size_t>(Find(Log->Events, platform::EFileEventKind::Added, name))];
	EXPECT_EQ(added.RootId, id);
	EXPECT_EQ(added.EntryType, platform::EFileEntryType::File);
	EXPECT_TRUE(added.Identity.IsValid());
	EXPECT_EQ(added.Identity, stat->Identity) << "an event's identity is the one Stat reports";
	EXPECT_NE(added.Timestamp, 0u);

	Log->Clear();
	WriteText(File(name), "two, longer");
	EXPECT_TRUE(Log->WaitFor([&] (const auto& Events) { return Has(Events, platform::EFileEventKind::Modified, name); }))
		<< Describe(Log->Events).CStr();

	Log->Clear();
	ASSERT_TRUE(platform::RemoveFile(File(name)));
	ASSERT_TRUE(Log->WaitFor([&] (const auto& Events) { return Has(Events, platform::EFileEventKind::Removed, name); }))
		<< Describe(Log->Events).CStr();
	const platform::SFileEvent& removed = Log->Events[static_cast<size_t>(Find(Log->Events, platform::EFileEventKind::Removed, name))];
	EXPECT_EQ(removed.Identity, stat->Identity) << "a Removed event carries the identity the file had";
}

TEST_F(FileWatcherTest, ARenameInOneDirectoryIsOldThenNewWithOneIdentity)
{
	WriteText(File("Старе.txt"), "x");
	Start();

	std::error_code ec;
	std::filesystem::rename(File("Старе.txt"), File("Нове.txt"), ec);
	ASSERT_FALSE(ec);
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::RenamedNew, "Нове.txt"); }))
		<< Describe(Log->Events).CStr();
	const int64 oldIndex = Find(Log->Events, platform::EFileEventKind::RenamedOld, "Старе.txt");
	const int64 newIndex = Find(Log->Events, platform::EFileEventKind::RenamedNew, "Нове.txt");
	ASSERT_GE(oldIndex, 0) << Describe(Log->Events).CStr();
	EXPECT_EQ(newIndex, oldIndex + 1) << "the two halves are adjacent";
	EXPECT_EQ(Log->Events[static_cast<size_t>(oldIndex)].Identity, Log->Events[static_cast<size_t>(newIndex)].Identity);
	EXPECT_EQ(Log->Events[static_cast<size_t>(newIndex)].Identity, platform::Stat(File("Нове.txt"))->Identity);
}

TEST_F(FileWatcherTest, AMoveBetweenDirectoriesIsRemovedThenAddedWithOneIdentity)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Один"), ec);
	std::filesystem::create_directories(File("Два"), ec);
	WriteText(File("Один/Ф.txt"), "x");
	Start();

	std::filesystem::rename(File("Один/Ф.txt"), File("Два/Ф.txt"), ec);
	ASSERT_FALSE(ec);
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::Added, "Два/Ф.txt"); }))
		<< Describe(Log->Events).CStr();
	const int64 removedIndex = Find(Log->Events, platform::EFileEventKind::Removed, "Один/Ф.txt");
	const int64 addedIndex = Find(Log->Events, platform::EFileEventKind::Added, "Два/Ф.txt");
	ASSERT_GE(removedIndex, 0) << "S9: no RENAMED events across directories\n" << Describe(Log->Events).CStr();
	EXPECT_LT(removedIndex, addedIndex);
	EXPECT_EQ(Log->Events[static_cast<size_t>(removedIndex)].Identity, Log->Events[static_cast<size_t>(addedIndex)].Identity);
}

TEST_F(FileWatcherTest, ACaseOnlyRenameLosesItsLeadingRemoved)
{
	WriteText(File("текстура.png"), "x");
	Start();

	// MoveFileExW, as Explorer, cmd's ren and git mv do it (S16, G3, G4).
	ASSERT_TRUE(MoveFileExW(File("текстура.png").c_str(), File("Текстура.png").c_str(), 0u));
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::RenamedNew, "Текстура.png"); }))
		<< Describe(Log->Events).CStr();
	Log->Collect(100ms);
	EXPECT_TRUE(Has(Log->Events, platform::EFileEventKind::RenamedOld, "текстура.png")) << Describe(Log->Events).CStr();
	EXPECT_FALSE(Has(Log->Events, platform::EFileEventKind::Removed, "текстура.png")) << Describe(Log->Events).CStr();
}

TEST_F(FileWatcherTest, ADirectoryMovedInIsOneEventThatSaysItIsADirectory)
{
	// Built outside the root, then moved in (S13): its children report nothing.
	const std::filesystem::path outside = Dir / FStringView("Пакунок 包").ToPath();
	std::error_code ec;
	std::filesystem::create_directories(outside / "Вкладене", ec);
	WriteText(outside / "a.txt", "a");
	WriteText(outside / FStringView("Вкладене/b.txt").ToPath(), "b");
	Start();

	std::filesystem::rename(outside, File("Пакунок 包"), ec);
	ASSERT_FALSE(ec);
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::Added, "Пакунок 包"); }))
		<< Describe(Log->Events).CStr();
	Log->Collect(100ms);
	const platform::SFileEvent& added = Log->Events[static_cast<size_t>(Find(Log->Events, platform::EFileEventKind::Added, "Пакунок 包"))];
	EXPECT_EQ(added.EntryType, platform::EFileEntryType::Directory);
	EXPECT_FALSE(Has(Log->Events, platform::EFileEventKind::Added, "Пакунок 包/a.txt")) << Describe(Log->Events).CStr();
	EXPECT_FALSE(Has(Log->Events, platform::EFileEventKind::Added, "Пакунок 包/Вкладене/b.txt")) << Describe(Log->Events).CStr();

	// A directory's own MODIFIED (its times, changed by an entry inside) is not reported.
	Log->Clear();
	WriteText(File("Пакунок 包/c.txt"), "c");
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::Added, "Пакунок 包/c.txt"); }));
	Log->Collect(100ms);
	EXPECT_FALSE(Has(Log->Events, platform::EFileEventKind::Modified, "Пакунок 包")) << Describe(Log->Events).CStr();
}

TEST_F(FileWatcherTest, ATinyBufferOverflowsIntoAnOverflowEvent)
{
	// A buffer smaller than one record: every report overflows it, deterministically (a flood into a
	// small buffer overflows only when the reader falls behind, which a fast thread may not).
	platform::SFileWatcherDesc desc;
	desc.BufferBytes = 64u;
	const uint32 id = Start(desc);

	for (uint32 i = 0u; i < 20u; ++i)
	{
		WriteText(File(FString::Format("Переповнення переповнення переповнення {:04}.txt", i)), "x");
	}
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return HasKind(Events, platform::EFileEventKind::Overflow); }))
		<< Describe(Log->Events).CStr();
	for (const platform::SFileEvent& event : Log->Events)
	{
		if (event.Kind == platform::EFileEventKind::Overflow)
		{
			EXPECT_EQ(event.RootId, id);
			EXPECT_TRUE(event.Path.IsEmpty());
		}
	}

	// Still armed afterwards: the next change overflows again.
	Log->Collect(50ms);
	Log->Clear();
	WriteText(File("Після.txt"), "x");
	EXPECT_TRUE(Log->WaitFor([] (const auto& Events) { return HasKind(Events, platform::EFileEventKind::Overflow); }))
		<< Describe(Log->Events).CStr();
}

TEST_F(FileWatcherTest, ADeletedRootIsLostAndComesBackAsAnOverflow)
{
	platform::SFileWatcherDesc desc;
	desc.RootPollMs = 50u;
	const uint32 id = Start(desc);
	WriteText(File("a.txt"), "a");
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::Added, "a.txt"); }));

	std::error_code ec;
	std::filesystem::remove_all(Root, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return HasKind(Events, platform::EFileEventKind::RootLost); }))
		<< Describe(Log->Events).CStr();

	Log->Clear();
	ASSERT_TRUE(std::filesystem::create_directories(Root, ec));
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return HasKind(Events, platform::EFileEventKind::Overflow); }))
		<< "a root that is back is armed again, and says events were missed\n" << Describe(Log->Events).CStr();
	EXPECT_EQ(Log->Events.back().RootId, id);

	Log->Clear();
	WriteText(File("b.txt"), "b");
	EXPECT_TRUE(Log->WaitFor([] (const auto& Events) { return Has(Events, platform::EFileEventKind::Added, "b.txt"); }))
		<< Describe(Log->Events).CStr();
}

TEST_F(FileWatcherTest, ARenamedRootIsLostAndComesBackWhenItsPathIsADirectoryAgain)
{
	platform::SFileWatcherDesc desc;
	desc.RootPollMs = 50u;
	Start(desc);

	// Windows says nothing when a watched root is renamed (measured): the final-path check notices.
	const std::filesystem::path renamed = Dir / FStringView("Перейменований").ToPath();
	std::error_code ec;
	std::filesystem::rename(Root, renamed, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return HasKind(Events, platform::EFileEventKind::RootLost); }))
		<< Describe(Log->Events).CStr();

	// Files written under the old name's new place are not reported as the root's.
	Log->Clear();
	WriteText(renamed / "a.txt", "a");
	Log->Collect(200ms);
	EXPECT_FALSE(Has(Log->Events, platform::EFileEventKind::Added, "a.txt")) << Describe(Log->Events).CStr();

	std::filesystem::rename(renamed, Root, ec);
	ASSERT_FALSE(ec);
	ASSERT_TRUE(Log->WaitFor([] (const auto& Events) { return HasKind(Events, platform::EFileEventKind::Overflow); }))
		<< Describe(Log->Events).CStr();
}

TEST_F(FileWatcherTest, DisarmDropsQueuedEventsAndStopsNewOnes)
{
	const uint32 id = Start();
	WriteText(File("a.txt"), "a");
	// Let the events reach the queue without taking them.
	ASSERT_TRUE(Watcher->WaitForEvents(5.0));
	std::this_thread::sleep_for(50ms);

	Watcher->Disarm(id);
	Log->Collect(10ms);
	EXPECT_TRUE(Log->Events.empty()) << Describe(Log->Events).CStr();

	WriteText(File("b.txt"), "b");
	Log->Collect(200ms);
	EXPECT_TRUE(Log->Events.empty()) << Describe(Log->Events).CStr();
	Watcher->Disarm(id);   // twice: ignored
}

TEST_F(FileWatcherTest, ArmRefusesAMissingDirectoryAndAFileAndRootsHaveDistinctIds)
{
	Watcher = platform::CreateFileWatcher();
	FString error;
	EXPECT_EQ(Watcher->Arm(File("Немає"), &error), 0u);
	EXPECT_FALSE(error.IsEmpty());

	WriteText(File("файл.txt"), "x");
	error.Clear();
	EXPECT_EQ(Watcher->Arm(File("файл.txt"), &error), 0u);
	EXPECT_FALSE(error.IsEmpty());

	std::error_code ec;
	std::filesystem::create_directories(File("Під"), ec);
	const uint32 first = Watcher->Arm(Root);
	const uint32 second = Watcher->Arm(File("Під"));
	EXPECT_NE(first, 0u);
	EXPECT_NE(second, 0u);
	EXPECT_NE(first, second);

	// Overlapping roots each report their own events, with paths relative to themselves.
	CEventLog log(*Watcher);
	WriteText(File("Під/x.txt"), "x");
	EXPECT_TRUE(log.WaitFor([&] (const auto& Events)
	{
		bool bFirst = false;
		bool bSecond = false;
		for (const platform::SFileEvent& event : Events)
		{
			bFirst = bFirst || (event.RootId == first && event.Path == "Під/x.txt");
			bSecond = bSecond || (event.RootId == second && event.Path == "x.txt");
		}
		return bFirst && bSecond;
	})) << Describe(log.Events).CStr();
}

TEST_F(FileWatcherTest, TheWriterProbeTellsAHeldFileFromAFreeOne)
{
	const std::filesystem::path path = File("Записуваний.bin");
	WriteText(path, "x");
	EXPECT_EQ(platform::ProbeFileWriters(path), platform::EWriterProbe::Free);
	EXPECT_EQ(platform::ProbeFileWriters(File("Немає.bin")), platform::EWriterProbe::Missing);

	{
		// A writer that lets others read (an editor mid-save).
		const HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0u, nullptr);
		ASSERT_NE(writer, INVALID_HANDLE_VALUE);
		EXPECT_EQ(platform::ProbeFileWriters(path), platform::EWriterProbe::Held);
		CloseHandle(writer);
	}
	{
		// S19's writer: share mode 0.
		const HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING, 0u, nullptr);
		ASSERT_NE(writer, INVALID_HANDLE_VALUE);
		EXPECT_EQ(platform::ProbeFileWriters(path), platform::EWriterProbe::Held);
		CloseHandle(writer);
	}
	{
		// A reader that shares everything does not count.
		const HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, 0u, nullptr);
		ASSERT_NE(reader, INVALID_HANDLE_VALUE);
		EXPECT_EQ(platform::ProbeFileWriters(path), platform::EWriterProbe::Free);
		CloseHandle(reader);
	}
	EXPECT_EQ(platform::ProbeFileWriters(path), platform::EWriterProbe::Free);
}
