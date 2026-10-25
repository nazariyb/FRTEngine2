// The watcher service (Assets/FileWatcherService.h) turns the platform's reports into the changes its
// subscribers are told, by the rules of design §5.4, replayed here on the real Windows watcher with
// the measured scenarios (gap-watch-meta §1.3, S1-S19 and G1-G5) as far as a test can produce them:
// create, write and delete; a rename in one directory and a move between two (paired by identity);
// directory renames, moves out (one record for the subtree) and moves in (the subtree enumerated:
// a Recycle Bin restore and a folder drag-in); a file moved into a directory that then goes, with no
// event after it; a file taking a removed directory's place; a case-only rename; atomic saves of three
// kinds, which change the identity and are still one Modified; a writer that holds the file (nothing is
// told until it lets go); an overflow (a Rescan once the tree is quiet); Flush's fence, also over roots
// that overlap; self-writes that diff to nothing; a slow checkout of files arriving over seconds; and a
// scripted git checkout of 300 files, told as one batch after the tree settles, with no callback in the
// middle of it. Filters, the directory that does not exist yet, the VCS seam, the off switch and its
// scope are covered too, and - through a scripted platform watcher - a file system without entry types,
// whose directories report their own Modified. Everything runs in directories with non-ASCII names.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "Assets/FileWatcherService.h"
#include "Config/Config.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"
#include "Platform/Timestamp.h"

#include <Windows.h>

using namespace frt;
using namespace std::chrono_literals;


namespace
{
template <typename T>
concept CanSubscribe = requires (assets::CFileWatcherService& Service, const T& Path)
{
	{ Service.Subscribe(Path, assets::SWatchFilter(), [] (std::span<const assets::SFileChange>) {}) };
};

static_assert(CanSubscribe<std::filesystem::path>);
static_assert(!CanSubscribe<const char*>);
static_assert(!CanSubscribe<FString>);


const char* KindName (assets::EFileChange Kind)
{
	switch (Kind)
	{
	case assets::EFileChange::Added: return "Added";
	case assets::EFileChange::Modified: return "Modified";
	case assets::EFileChange::Removed: return "Removed";
	case assets::EFileChange::Moved: return "Moved";
	case assets::EFileChange::Rescan: return "Rescan";
	}
	return "?";
}

// What one subscription was told: every change, and every callback (its time and size).
struct SRecorder
{
	struct SCall
	{
		std::chrono::steady_clock::time_point Time;
		size_t Changes = 0u;
	};

	std::vector<assets::SFileChange> Changes;
	std::vector<SCall> Calls;

	assets::FFileChangesCallback MakeCallback ()
	{
		return [this] (std::span<const assets::SFileChange> Told)
		{
			Calls.push_back({ std::chrono::steady_clock::now(), Told.size() });
			Changes.insert(Changes.end(), Told.begin(), Told.end());
		};
	}

	void Clear ()
	{
		Changes.clear();
		Calls.clear();
	}

	[[nodiscard]] FString Describe () const
	{
		FString text;
		for (const assets::SFileChange& change : Changes)
		{
			text.AppendFormat("  {}{} '{}'", KindName(change.Kind), change.bDirectory ? " (directory)" : "", FString::FromPath(change.Path));
			if (!change.OldPath.empty())
			{
				text.AppendFormat(" from '{}'", FString::FromPath(change.OldPath));
			}
			text.AppendChar('\n');
		}
		return text;
	}

	// The changes whose relative path is RelativePath.
	[[nodiscard]] std::vector<const assets::SFileChange*> Find (FStringView RelativePath) const
	{
		std::vector<const assets::SFileChange*> found;
		for (const assets::SFileChange& change : Changes)
		{
			if (change.RelativePath == RelativePath)
			{
				found.push_back(&change);
			}
		}
		return found;
	}

	// Exactly one change at RelativePath, of Kind.
	[[nodiscard]] bool HasOnly (FStringView RelativePath, assets::EFileChange Kind) const
	{
		const std::vector<const assets::SFileChange*> found = Find(RelativePath);
		return found.size() == 1u && found[0]->Kind == Kind;
	}
};

void WriteText (const std::filesystem::path& Path, FStringView Text)
{
	ASSERT_TRUE(platform::WriteFileBytes(Path, Text.AsBytes())) << FString::FromPath(Path).CStr();
}

// A platform watcher the test speaks for, reporting what a file system without entry types reports
// (ReadDirectoryChangesW's fallback on FAT, ReFS or a share): every event's EntryType is Unknown.
class CScriptedWatcher final : public platform::IFileWatcher
{
public:
	struct SQueue
	{
		std::vector<platform::SFileEvent> Events;
		uint32 LastRootId = 0u;

		void Push (platform::EFileEventKind Kind, FStringView Path)
		{
			platform::SFileEvent event;
			event.RootId = LastRootId;
			event.Kind = Kind;
			event.Path = FString(Path);
			Events.push_back(std::move(event));
		}
	};

	explicit CScriptedWatcher (std::shared_ptr<SQueue> InQueue)
		: Queue(std::move(InQueue))
	{}

	uint32 Arm (const std::filesystem::path&, FString*) override
	{
		return ++Queue->LastRootId;
	}

	void Disarm (uint32) override {}

	void TakeEvents (std::vector<platform::SFileEvent>& Out) override
	{
		Out.insert(Out.end(), Queue->Events.begin(), Queue->Events.end());
		Queue->Events.clear();
	}

	bool WaitForEvents (double) override
	{
		return !Queue->Events.empty();
	}

private:
	std::shared_ptr<SQueue> Queue;
};

std::optional<std::filesystem::path> FindGitOnPath ()
{
	const std::optional<FString> path = platform::GetEnvironmentValue("PATH");
	if (!path)
	{
		return std::nullopt;
	}
	for (const FStringView dir : path->View().Split(';', EStringSplit::SkipEmpty | EStringSplit::Trim))
	{
		const std::filesystem::path candidate = dir.TrimQuotes().ToPath() / "git.exe";
		if (platform::Stat(candidate).has_value())
		{
			return candidate;
		}
	}
	return std::nullopt;
}


// Every test works in a directory of this process's own with a non-ASCII name; Content is the watched
// directory, Outside a sibling for moves in and out.
class FileWatcherServiceTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Служба 服务 😀 {}", platform::GetCurrentProcessId()).ToPath()
			/ "FileWatcherService";
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		Content = Dir / FStringView("Вміст 内容").ToPath();
		Outside = Dir / FStringView("Поза 外面").ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(Content, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		ASSERT_TRUE(std::filesystem::create_directories(Outside, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		Service.reset();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	// A service with Settings and one subscription on Content (or Directory) with Filter.
	void Start (const assets::SFileWatcherSettings& Settings = {}, const assets::SWatchFilter& Filter = {})
	{
		Service = std::make_unique<assets::CFileWatcherService>(Settings);
		Handle = Service->Subscribe(Content, Filter, Recorder.MakeCallback());
		ASSERT_TRUE(Handle.IsValid());
		ASSERT_EQ(Service->GetStats().Roots, 1u);
	}

	[[nodiscard]] std::filesystem::path File (FStringView RelativePath) const
	{
		return Content / RelativePath.ToPath();
	}

	void Flush ()
	{
		ASSERT_TRUE(Service->Flush(Content));
	}

	// Pumps every few milliseconds until Done() or Timeout.
	bool PumpUntil (const std::function<bool ()>& Done, std::chrono::milliseconds Timeout = 5s)
	{
		const auto deadline = std::chrono::steady_clock::now() + Timeout;
		for (;;)
		{
			Service->Pump();
			if (Done())
			{
				return true;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::sleep_for(5ms);
		}
	}

	std::filesystem::path Dir;
	std::filesystem::path Content;
	std::filesystem::path Outside;
	SRecorder Recorder;
	std::unique_ptr<assets::CFileWatcherService> Service;
	SDelegateHandle Handle;
};
}


// ---------------- Files ----------------

TEST_F(FileWatcherServiceTest, ACreatedFileSettlesIntoOneAddedWithoutAFlush)
{
	Start();
	WriteText(File("Матеріал.frtmat.yml"), "a: 1\n");

	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty() && Service->IsIdle(); })) << Recorder.Describe().CStr();
	std::this_thread::sleep_for(300ms);
	Service->Pump();
	EXPECT_TRUE(Recorder.HasOnly("Матеріал.frtmat.yml", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Calls.size(), 1u) << "S1's ADDED and MODIFIED are one change";
	const assets::SFileChange& change = Recorder.Changes[0];
	EXPECT_EQ(change.Path, File("Матеріал.frtmat.yml")) << "absolute, spelled from the subscription's directory";
	EXPECT_FALSE(change.bDirectory);
}

TEST_F(FileWatcherServiceTest, WritesAndDeletesAreModifiedAndRemoved)
{
	WriteText(File("Файл.txt"), "one");
	Start();

	WriteText(File("Файл.txt"), "two, in place");
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Файл.txt", assets::EFileChange::Modified)) << Recorder.Describe().CStr();

	Recorder.Clear();
	ASSERT_TRUE(platform::RemoveFile(File("Файл.txt")));
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Файл.txt", assets::EFileChange::Removed)) << Recorder.Describe().CStr();

	// A temporary file that comes and goes within the window is nothing at all.
	Recorder.Clear();
	WriteText(File("Тимчасовий.txt"), "x");
	ASSERT_TRUE(platform::RemoveFile(File("Тимчасовий.txt")));
	Flush();
	EXPECT_TRUE(Recorder.Changes.empty()) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, ARenameInOneDirectoryIsOneMoved)
{
	WriteText(File("Старе.txt"), "x");
	Start();

	std::error_code ec;
	std::filesystem::rename(File("Старе.txt"), File("Нове.txt"), ec);
	ASSERT_FALSE(ec);
	Flush();
	ASSERT_TRUE(Recorder.HasOnly("Нове.txt", assets::EFileChange::Moved)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 1u) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes[0].OldPath, File("Старе.txt"));
}

TEST_F(FileWatcherServiceTest, AMoveBetweenDirectoriesPairsByIdentityIntoOneMoved)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Один"), ec);
	std::filesystem::create_directories(File("Два"), ec);
	WriteText(File("Один/Ф.txt"), "x");
	WriteText(File("Один/Ф.txt.meta"), "Guid: 1\n");
	Start();

	// S18: an asset and its meta moved one after the other: two REMOVED/ADDED pairs.
	std::filesystem::rename(File("Один/Ф.txt"), File("Два/Ф.txt"), ec);
	std::filesystem::rename(File("Один/Ф.txt.meta"), File("Два/Ф.txt.meta"), ec);
	ASSERT_FALSE(ec);
	Flush();
	ASSERT_TRUE(Recorder.HasOnly("Два/Ф.txt", assets::EFileChange::Moved)) << Recorder.Describe().CStr();
	ASSERT_TRUE(Recorder.HasOnly("Два/Ф.txt.meta", assets::EFileChange::Moved)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 2u) << "no Removed for the sources\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Find("Два/Ф.txt")[0]->OldPath, File("Один/Ф.txt"));
}

TEST_F(FileWatcherServiceTest, ACaseOnlyRenameIsAMovedWhosePathsDifferInCaseOnly)
{
	WriteText(File("текстура.png"), "x");
	Start();

	ASSERT_TRUE(MoveFileExW(File("текстура.png").c_str(), File("Текстура.png").c_str(), 0u));
	Flush();
	ASSERT_EQ(Recorder.Changes.size(), 1u) << Recorder.Describe().CStr();
	const assets::SFileChange& change = Recorder.Changes[0];
	EXPECT_EQ(change.Kind, assets::EFileChange::Moved) << Recorder.Describe().CStr();
	EXPECT_EQ(change.RelativePath, "Текстура.png");
	EXPECT_EQ(change.OldPath, File("текстура.png"));
}

TEST_F(FileWatcherServiceTest, AtomicSavesChangeTheIdentityAndAreStillOneModified)
{
	WriteText(File("Документ.yml"), "v: 0\n");
	assets::SWatchFilter filter;
	filter.Extensions = { ".yml" };   // the temporary files and the backup are not .yml
	Start({}, filter);
	const platform::SFileIdentity before = platform::Stat(File("Документ.yml"))->Identity;

	// FRT's own (a POSIX-semantics rename over the file).
	ASSERT_TRUE(platform::WriteFileAtomic(File("Документ.yml"), FStringView("v: 1\n")));
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Документ.yml", assets::EFileChange::Modified)) << Recorder.Describe().CStr();
	EXPECT_NE(platform::Stat(File("Документ.yml"))->Identity, before);

	// S5: temporary file, then MoveFileEx(REPLACE_EXISTING) (VS Code's atomic path, .NET File.Move).
	Recorder.Clear();
	WriteText(File("Документ.yml.tmp"), "v: 2\n");
	ASSERT_TRUE(MoveFileExW(File("Документ.yml.tmp").c_str(), File("Документ.yml").c_str(), MOVEFILE_REPLACE_EXISTING));
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Документ.yml", assets::EFileChange::Modified)) << Recorder.Describe().CStr();

	// S4: ReplaceFileW with a backup (.NET File.Replace, many Win32 applications).
	Recorder.Clear();
	WriteText(File("Документ.yml.new"), "v: 3\n");
	ASSERT_TRUE(ReplaceFileW(File("Документ.yml").c_str(), File("Документ.yml.new").c_str(), File("Документ.yml.bak").c_str(), 0u, nullptr,
		nullptr));
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Документ.yml", assets::EFileChange::Modified)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 1u) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, AFileAWriterHoldsIsToldOnlyOnceItLetsGo)
{
	Start();
	const std::filesystem::path path = File("Записується.bin");
	const HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0u, nullptr);
	ASSERT_NE(writer, INVALID_HANDLE_VALUE);
	DWORD written = 0u;
	ASSERT_TRUE(WriteFile(writer, "0123456789", 10u, &written, nullptr));
	ASSERT_TRUE(FlushFileBuffers(writer));

	// S19: the file exists, its writer still holds it: the service waits.
	Flush();
	EXPECT_TRUE(Recorder.Changes.empty()) << Recorder.Describe().CStr();
	EXPECT_GE(Service->GetStats().WriterBackoffs, 1u);
	EXPECT_EQ(Service->GetStats().PendingPaths, 1u);

	CloseHandle(writer);
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Записується.bin", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Service->IsIdle());
}

// ---------------- Directories ----------------

TEST_F(FileWatcherServiceTest, ARenamedDirectoryIsOneMovedAndWhatWasPendingBelowItMovesAlong)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Стара"), ec);
	WriteText(File("Стара/f.txt"), "x");
	assets::SFileWatcherSettings settings;
	settings.SettleMs = 10000u;   // nothing settles by itself: only the fence tells
	Start(settings);

	WriteText(File("Стара/Нове.txt"), "pending when the directory is renamed");
	std::filesystem::rename(File("Стара"), File("Нова"), ec);
	ASSERT_FALSE(ec);
	Flush();
	ASSERT_TRUE(Recorder.HasOnly("Нова", assets::EFileChange::Moved)) << Recorder.Describe().CStr();
	const assets::SFileChange& moved = *Recorder.Find("Нова")[0];
	EXPECT_TRUE(moved.bDirectory);
	EXPECT_EQ(moved.OldPath, File("Стара"));
	EXPECT_TRUE(Recorder.HasOnly("Нова/Нове.txt", assets::EFileChange::Added)) << "re-pathed\n" << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.Find("Стара/Нове.txt").empty()) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.Find("Нова/f.txt").empty()) << "a paired rename is not enumerated\n" << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, ADirectoryMovedOutIsOneRemovedForItsWholeSubtree)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Геть/Глибше"), ec);
	WriteText(File("Геть/a.txt"), "a");
	WriteText(File("Геть/Глибше/b.txt"), "b");
	Start();
	// A second subscriber that watches one file inside it: told about it as a whole.
	SRecorder single;
	assets::SWatchFilter filter;
	filter.Prefix = "Геть/Глибше/b.txt";
	Service->Subscribe(Content, filter, single.MakeCallback());

	std::filesystem::rename(File("Геть"), Outside / "Геть", ec);
	ASSERT_FALSE(ec);
	Flush();
	ASSERT_EQ(Recorder.Changes.size(), 1u) << "S14: one record, no ghosts\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes[0].Kind, assets::EFileChange::Removed);
	EXPECT_TRUE(Recorder.Changes[0].bDirectory);
	EXPECT_EQ(Recorder.Changes[0].RelativePath, "Геть");

	ASSERT_EQ(single.Changes.size(), 1u) << single.Describe().CStr();
	EXPECT_EQ(single.Changes[0].Kind, assets::EFileChange::Removed);
	EXPECT_EQ(single.Changes[0].Path, File("Геть/Глибше/b.txt"));
}

TEST_F(FileWatcherServiceTest, ADirectoryMovedInHasItsSubtreeEnumerated)
{
	// "Restore from the Recycle Bin" and Explorer's move from another folder of the volume are both a
	// rename into the root (S13): one event for the directory, none for its children.
	std::error_code ec;
	const std::filesystem::path package = Outside / FStringView("Відновлений 恢复").ToPath();
	std::filesystem::create_directories(package / "Вкладене" / "Ще", ec);
	WriteText(package / "a.txt", "a");
	WriteText(package / FStringView("Вкладене/b.txt").ToPath(), "b");
	WriteText(package / FStringView("Вкладене/Ще/c.txt").ToPath(), "c");
	Start();

	std::filesystem::rename(package, File("Відновлений 恢复"), ec);
	ASSERT_FALSE(ec);
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Відновлений 恢复/a.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Відновлений 恢复/Вкладене/b.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Відновлений 恢复/Вкладене/Ще/c.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 3u) << Recorder.Describe().CStr();
	EXPECT_GE(Service->GetStats().DirectoriesEnumerated, 3u);

	// A folder dragged in from another volume or copied: the directory, then each file as it is written.
	Recorder.Clear();
	const std::filesystem::path dragged = Outside / FStringView("Перетягнутий").ToPath();
	std::filesystem::create_directories(dragged / "Під", ec);
	WriteText(dragged / "x.txt", "x");
	WriteText(dragged / FStringView("Під/y.txt").ToPath(), "y");
	std::filesystem::copy(dragged, File("Перетягнутий"), std::filesystem::copy_options::recursive, ec);
	ASSERT_FALSE(ec);
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Перетягнутий/x.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Перетягнутий/Під/y.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 2u) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, AFileMovedIntoADirectoryThatThenGoesIsRemovedWhereItCameFromWithNoLaterEvent)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Звідки"), ec);
	std::filesystem::create_directories(File("Куди"), ec);
	WriteText(File("Звідки/X.txt"), "x");
	Start();

	// X moves into Куди/ (Removed and Added, one identity), Куди/ leaves the root with it, and then
	// nothing happens at all: the directory's removal must expire by the clock, and X's removal from
	// where it was must be told although its entry remembered that X had moved away.
	std::filesystem::rename(File("Звідки/X.txt"), File("Куди/X.txt"), ec);
	ASSERT_FALSE(ec);
	std::filesystem::rename(File("Куди"), Outside / "Куди", ec);
	ASSERT_FALSE(ec);
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Find("Звідки/X.txt").empty() && Service->IsIdle(); })) << Recorder.Describe().CStr();
	PumpUntil([] { return false; }, 300ms);
	EXPECT_TRUE(Recorder.HasOnly("Звідки/X.txt", assets::EFileChange::Removed)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Куди", assets::EFileChange::Removed)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.Find("Куди/X.txt").empty()) << "the directory's removal stands for it\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 2u) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, AFileThatTakesARemovedDirectorysPlaceIsAddedAfterTheDirectorysRemoval)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Шлях"), ec);
	WriteText(File("Шлях/a.txt"), "a");
	Start();

	// git switching a path from a directory on one branch to a file on another, within the window.
	std::filesystem::remove_all(File("Шлях"), ec);
	ASSERT_FALSE(ec);
	WriteText(File("Шлях"), "a file now");
	Flush();
	const std::vector<const assets::SFileChange*> found = Recorder.Find("Шлях");
	ASSERT_EQ(found.size(), 2u) << Recorder.Describe().CStr();
	EXPECT_EQ(found[0]->Kind, assets::EFileChange::Removed) << Recorder.Describe().CStr();
	EXPECT_TRUE(found[0]->bDirectory) << "the directory first, standing for a.txt";
	EXPECT_EQ(found[1]->Kind, assets::EFileChange::Added) << Recorder.Describe().CStr();
	EXPECT_FALSE(found[1]->bDirectory);
	EXPECT_EQ(Recorder.Changes.size(), 2u) << Recorder.Describe().CStr();
	EXPECT_TRUE(assets::IsFileAffected(*found[1], File("Шлях"))) << "a loader of the file looks at it";
}

TEST_F(FileWatcherServiceTest, WithoutEntryTypesADirectorysOwnModifiedIsDropped)
{
	// The fallback without extended information says nothing about what an entry is, and reports a
	// directory's own MODIFIED whenever an entry inside it changes.
	std::error_code ec;
	std::filesystem::create_directories(File("Шейдери"), ec);
	WriteText(File("Шейдери/Common.hlsli"), "// one\n");
	const auto queue = std::make_shared<CScriptedWatcher::SQueue>();
	assets::SFileWatcherSettings settings;
	settings.SettleMs = 20u;
	settings.RemoveGraceMs = 20u;
	Service = std::make_unique<assets::CFileWatcherService>(settings, std::make_unique<CScriptedWatcher>(queue));
	Service->Subscribe(Content, {}, Recorder.MakeCallback());   // no filter, as the shader root's
	ASSERT_EQ(Service->GetStats().Roots, 1u);

	WriteText(File("Шейдери/Common.hlsli"), "// two\n");
	queue->Push(platform::EFileEventKind::Modified, "Шейдери/Common.hlsli");
	queue->Push(platform::EFileEventKind::Modified, "Шейдери");
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty() && Service->IsIdle(); })) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Шейдери/Common.hlsli", assets::EFileChange::Modified)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 1u) << "no Rescan of the edited file's directory\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Service->GetStats().Rescans, 0u);

	// A directory that appears is still known as one, by the disk, and enumerated.
	Recorder.Clear();
	std::filesystem::create_directories(File("Нова"), ec);
	WriteText(File("Нова/x.hlsli"), "x");
	queue->Push(platform::EFileEventKind::Added, "Нова");
	queue->Push(platform::EFileEventKind::Modified, "Нова");
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty() && Service->IsIdle(); })) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Нова/x.hlsli", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 1u) << Recorder.Describe().CStr();
}

// ---------------- Overflow, fences, self-writes ----------------

TEST_F(FileWatcherServiceTest, AnOverflowBecomesOneRescanOnceTheTreeIsQuiet)
{
	assets::SFileWatcherSettings settings;
	settings.Watcher.BufferBytes = 64u;   // smaller than one record: every report overflows
	settings.VcsSettleMs = 300u;
	Start(settings);

	for (uint32 i = 0u; i < 10u; ++i)
	{
		WriteText(File(FString::Format("Потоп {:02}.txt", i)), "x");
	}
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty(); })) << Recorder.Describe().CStr();
	ASSERT_EQ(Recorder.Changes.size(), 1u) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes[0].Kind, assets::EFileChange::Rescan);
	EXPECT_TRUE(Recorder.Changes[0].bDirectory);
	EXPECT_EQ(Recorder.Changes[0].Path, Content);
	EXPECT_GE(Service->GetStats().Overflows, 1u);
	EXPECT_GE(Service->GetStats().Rescans, 1u);

	// Nothing else follows: the rescan stood for everything that was pending.
	PumpUntil([] { return false; }, 400ms);
	EXPECT_EQ(Recorder.Calls.size(), 1u) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, ALostRootIsARescanAndSoIsItsReturn)
{
	WriteText(File("a.txt"), "a");
	assets::SFileWatcherSettings settings;
	settings.Watcher.RootPollMs = 50u;
	settings.VcsSettleMs = 300u;
	Start(settings);

	std::error_code ec;
	std::filesystem::remove_all(Content, ec);
	ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty(); })) << Recorder.Describe().CStr();
	ASSERT_EQ(Recorder.Changes.size(), 1u) << "the rescan stands for the removals before it\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes[0].Kind, assets::EFileChange::Rescan);
	EXPECT_EQ(Recorder.Changes[0].Path, Content);
	EXPECT_EQ(Service->GetStats().RootsLost, 1u);

	Recorder.Clear();
	ASSERT_TRUE(std::filesystem::create_directories(Content, ec));
	WriteText(File("b.txt"), "b");
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty() && Service->IsIdle(); })) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes[0].Kind, assets::EFileChange::Rescan) << Recorder.Describe().CStr();

	// Watching again.
	Recorder.Clear();
	WriteText(File("c.txt"), "c");
	ASSERT_TRUE(Service->Flush(Content));
	EXPECT_TRUE(Recorder.HasOnly("c.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, FlushTellsEverythingBeforeItsFenceAndNothingOfTheFence)
{
	assets::SFileWatcherSettings settings;
	settings.SettleMs = 60000u;
	settings.RemoveGraceMs = 60000u;
	Start(settings);

	WriteText(File("a.txt"), "a");
	WriteText(File("b.txt"), "b");
	// Without the fence nothing would settle for a minute.
	PumpUntil([] { return false; }, 300ms);
	EXPECT_TRUE(Recorder.Changes.empty()) << Recorder.Describe().CStr();
	EXPECT_EQ(Service->GetStats().PendingPaths, 2u);

	Flush();
	EXPECT_TRUE(Recorder.HasOnly("a.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("b.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Calls.size(), 1u) << "one callback per call";
	for (const assets::SFileChange& change : Recorder.Changes)
	{
		EXPECT_FALSE(change.RelativePath.StartsWith(".frt-fence-")) << Recorder.Describe().CStr();
	}

	// The fence is gone, and its removal is never told either.
	bool bFenceLeft = false;
	platform::EnumerateDirectory(Content, [&bFenceLeft] (const platform::SDirectoryEntry& Entry)
	{
		bFenceLeft = bFenceLeft || Entry.Name.StartsWith(".frt-fence-");
	});
	EXPECT_FALSE(bFenceLeft);
	Recorder.Clear();
	Flush();
	EXPECT_TRUE(Recorder.Changes.empty()) << Recorder.Describe().CStr();

	// A directory nobody watches cannot be flushed.
	EXPECT_FALSE(Service->Flush(Outside));
}

TEST_F(FileWatcherServiceTest, FlushReachesEveryRootWhenADirectoryWasSubscribedBeforeOneAboveIt)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Моделі/Спонза"), ec);
	assets::SFileWatcherSettings settings;
	settings.SettleMs = 60000u;   // nothing settles by itself: only the fences tell
	settings.RemoveGraceMs = 60000u;
	Service = std::make_unique<assets::CFileWatcherService>(settings);
	// A material in a subdirectory loaded before one above it: two roots, each subscription on its own.
	SRecorder inner;
	Service->Subscribe(File("Моделі/Спонза"), {}, inner.MakeCallback());
	Service->Subscribe(Content, {}, Recorder.MakeCallback());
	ASSERT_EQ(Service->GetStats().Roots, 2u) << "the inner directory keeps its own root";

	// Flushing the inner directory tells the outer root's subscriber as well...
	WriteText(File("Моделі/Спонза/a.frtmat.yml"), "a");
	ASSERT_TRUE(Service->Flush(File("Моделі/Спонза")));
	EXPECT_TRUE(inner.HasOnly("a.frtmat.yml", assets::EFileChange::Added)) << inner.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Моделі/Спонза/a.frtmat.yml", assets::EFileChange::Added)) << Recorder.Describe().CStr();

	// ... and flushing the outer one tells the inner root's.
	inner.Clear();
	Recorder.Clear();
	WriteText(File("Моделі/Спонза/b.frtmat.yml"), "b");
	ASSERT_TRUE(Service->Flush(Content));
	EXPECT_TRUE(inner.HasOnly("b.frtmat.yml", assets::EFileChange::Added)) << inner.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("Моделі/Спонза/b.frtmat.yml", assets::EFileChange::Added)) << Recorder.Describe().CStr();

	// No root ever tells a fence, its own or the other's.
	inner.Clear();
	Recorder.Clear();
	ASSERT_TRUE(Service->Flush(Content));
	ASSERT_TRUE(Service->Flush(File("Моделі/Спонза")));
	EXPECT_TRUE(inner.Changes.empty()) << inner.Describe().CStr();
	EXPECT_TRUE(Recorder.Changes.empty()) << Recorder.Describe().CStr();
	EXPECT_EQ(Service->GetStats().PendingPaths, 0u);
}

TEST_F(FileWatcherServiceTest, AnExpectedWriteDiffsToNothingAndSomeoneElsesWriteIsStillTold)
{
	WriteText(File("Налаштування.ini"), "[A]\nB=1\n");
	Start();

	const FStringView mine = "[A]\nB=2\n";
	Service->ExpectWrite(File("Налаштування.ini"), mine.AsBytes());
	ASSERT_TRUE(platform::WriteFileAtomic(File("Налаштування.ini"), mine));
	Flush();
	EXPECT_TRUE(Recorder.Changes.empty()) << "the echo of our own write\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Service->GetStats().SelfWritesSuppressed, 1u);

	// Expected, but another process wrote other bytes: told as usual.
	Service->ExpectWrite(File("Налаштування.ini"), FStringView("[A]\nB=3\n").AsBytes());
	WriteText(File("Налаштування.ini"), "[A]\nB=someone else\n");
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Налаштування.ini", assets::EFileChange::Modified)) << Recorder.Describe().CStr();

	// An expectation is used once: the next identical write is told.
	Recorder.Clear();
	WriteText(File("Налаштування.ini"), "[A]\nB=someone else\n");
	Flush();
	EXPECT_TRUE(Recorder.HasOnly("Налаштування.ini", assets::EFileChange::Modified)) << Recorder.Describe().CStr();
}

// ---------------- Checkouts ----------------

TEST_F(FileWatcherServiceTest, ASlowCheckoutTellsEveryFileOnceAsModified)
{
	// A smudge filter or a Perforce sync: each file unlinked, then written a moment later, the files
	// arriving one by one over a couple of seconds.
	constexpr uint32 fileCount = 12u;
	for (uint32 i = 0u; i < fileCount; ++i)
	{
		WriteText(File(FString::Format("Повільний {:02}.yml", i)), "old\n");
	}
	Start();

	for (uint32 i = 0u; i < fileCount; ++i)
	{
		const std::filesystem::path path = File(FString::Format("Повільний {:02}.yml", i));
		ASSERT_TRUE(platform::RemoveFile(path));
		const auto written = std::chrono::steady_clock::now() + 120ms;
		while (std::chrono::steady_clock::now() < written)
		{
			Service->Pump();
			std::this_thread::sleep_for(5ms);
		}
		WriteText(path, FString::Format("new {}\n", i));
		Service->Pump();
	}
	ASSERT_TRUE(PumpUntil([&] { return Recorder.Changes.size() >= fileCount; })) << Recorder.Describe().CStr();
	Flush();
	EXPECT_EQ(Recorder.Changes.size(), fileCount) << Recorder.Describe().CStr();
	for (uint32 i = 0u; i < fileCount; ++i)
	{
		EXPECT_TRUE(Recorder.HasOnly(FString::Format("Повільний {:02}.yml", i), assets::EFileChange::Modified)) << Recorder.Describe().CStr();
	}
}

TEST_F(FileWatcherServiceTest, AGitCheckoutOf300FilesIsOneBatchAfterTheTreeSettles)
{
	const std::optional<std::filesystem::path> git = FindGitOnPath();
	if (!git)
	{
		GTEST_SKIP() << "no git.exe on PATH";
	}
	const std::filesystem::path repository = Dir / FStringView("Сховище 仓库").ToPath();
	const std::filesystem::path content = repository / "Content";
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directories(content / "Materials", ec));
	const auto runGit = [&] (std::vector<FString> Arguments)
	{
		const std::vector<FString> prefix = { "-C", FString::FromPath(repository), "-c", "core.autocrlf=false", "-c", "user.name=FRT Test", "-c",
			"user.email=frt-test@example.invalid", "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main" };
		Arguments.insert(Arguments.begin(), prefix.begin(), prefix.end());
		const test::SProcessResult result = test::RunProcess(*git, Arguments, repository);
		ASSERT_EQ(result.ExitCode, 0) << Arguments.back().CStr() << ": " << result.Errors.CStr() << result.Output.CStr();
	};

	// main: 300 materials and 10 files that feature deletes; feature: every material changed, 10 files added.
	constexpr uint32 materialCount = 300u;
	runGit({ "init", "-q" });
	for (uint32 i = 0u; i < materialCount; ++i)
	{
		WriteText(content / FString::Format("Materials/Матеріал {:03}.frtmat.yml", i).ToPath(), FString::Format("Roughness: 0.{}\n", i));
	}
	for (uint32 i = 0u; i < 10u; ++i)
	{
		WriteText(content / FString::Format("Старий {:02}.yml", i).ToPath(), "old\n");
	}
	runGit({ "add", "-A" });
	runGit({ "commit", "-q", "-m", "main" });
	runGit({ "checkout", "-q", "-b", "feature" });
	for (uint32 i = 0u; i < materialCount; ++i)
	{
		WriteText(content / FString::Format("Materials/Матеріал {:03}.frtmat.yml", i).ToPath(), FString::Format("Roughness: 0.{}5\n", i));
	}
	for (uint32 i = 0u; i < 10u; ++i)
	{
		ASSERT_TRUE(platform::RemoveFile(content / FString::Format("Старий {:02}.yml", i).ToPath()));
		WriteText(content / FString::Format("Новий {:02}.yml", i).ToPath(), "new\n");
	}
	runGit({ "add", "-A" });
	runGit({ "commit", "-q", "-m", "feature" });
	runGit({ "checkout", "-q", "main" });

	Service = std::make_unique<assets::CFileWatcherService>();
	Service->Subscribe(content, {}, Recorder.MakeCallback());
	ASSERT_EQ(Service->GetSettings().VcsSettleMs, 1500u) << "the default";

	// The checkout runs while the service is pumped as a frame loop would.
	test::CTestProcess checkout;
	const std::vector<FString> arguments = { "-C", FString::FromPath(repository), "-c", "core.autocrlf=false", "checkout", "-q", "feature" };
	FString error;
	ASSERT_TRUE(checkout.Start(*git, arguments, false, repository, &error)) << error.CStr();
	checkout.CloseInput();
	while (checkout.IsRunning())
	{
		Service->Pump();
		std::this_thread::sleep_for(5ms);
	}
	const auto gitDone = std::chrono::steady_clock::now();
	ASSERT_EQ(checkout.Wait(), 0) << checkout.ReadErrors().CStr();
	EXPECT_TRUE(Recorder.Calls.empty()) << "a callback ran while git was checking out\n" << Recorder.Describe().CStr();

	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Calls.empty(); }, 10s)) << Recorder.Describe().CStr();
	// Pump a while longer: nothing more may come.
	PumpUntil([] { return false; }, 500ms);

	ASSERT_EQ(Recorder.Calls.size(), 1u) << "one batch\n" << Recorder.Describe().CStr();
	EXPECT_GE(Service->GetStats().Bursts, 1u);
	EXPECT_GE(Recorder.Calls[0].Time - gitDone, 1000ms) << "held for [AssetsEditor] VcsSettleMs of quiet";
	EXPECT_EQ(Recorder.Changes.size(), materialCount + 20u) << Recorder.Describe().CStr();
	for (uint32 i = 0u; i < materialCount; ++i)
	{
		EXPECT_TRUE(Recorder.HasOnly(FString::Format("Materials/Матеріал {:03}.frtmat.yml", i), assets::EFileChange::Modified))
			<< "G1: unlink and write again is one Modified (" << i << ")";
	}
	for (uint32 i = 0u; i < 10u; ++i)
	{
		EXPECT_TRUE(Recorder.HasOnly(FString::Format("Старий {:02}.yml", i), assets::EFileChange::Removed)) << Recorder.Describe().CStr();
		EXPECT_TRUE(Recorder.HasOnly(FString::Format("Новий {:02}.yml", i), assets::EFileChange::Added)) << Recorder.Describe().CStr();
	}
}

// ---------------- Subscriptions ----------------

TEST_F(FileWatcherServiceTest, FiltersSelectByPrefixAndExtensionAndShareOneRoot)
{
	std::error_code ec;
	std::filesystem::create_directories(File("Materials"), ec);
	std::filesystem::create_directories(File("Input"), ec);
	Start();

	SRecorder materials;
	assets::SWatchFilter materialFilter;
	materialFilter.Extensions = { ".FRTMAT.yml" };   // compared ignoring case
	Service->Subscribe(File("Materials"), materialFilter, materials.MakeCallback());
	SRecorder ini;
	assets::SWatchFilter iniFilter;
	iniFilter.Prefix = "Input/EditorInput.ini";
	Service->Subscribe(Content, iniFilter, ini.MakeCallback());
	EXPECT_EQ(Service->GetStats().Roots, 1u) << "subscriptions inside an armed root share it";
	EXPECT_EQ(Service->GetStats().Subscriptions, 3u);

	WriteText(File("Materials/Цегла.frtmat.yml"), "a");
	WriteText(File("Materials/Цегла.png"), "b");
	WriteText(File("Input/EditorInput.ini"), "c");
	WriteText(File("Input/Other.ini"), "d");
	Flush();
	EXPECT_EQ(materials.Changes.size(), 1u) << materials.Describe().CStr();
	EXPECT_TRUE(materials.HasOnly("Цегла.frtmat.yml", assets::EFileChange::Added)) << "relative to the subscription's directory\n"
		<< materials.Describe().CStr();
	EXPECT_EQ(ini.Changes.size(), 1u) << ini.Describe().CStr();
	EXPECT_TRUE(ini.HasOnly("Input/EditorInput.ini", assets::EFileChange::Added)) << ini.Describe().CStr();
	EXPECT_EQ(Recorder.Changes.size(), 4u) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, ACallbackMayUnsubscribeItselfAndOthersAndTheLastOneDisarmsTheRoot)
{
	Service = std::make_unique<assets::CFileWatcherService>();
	SDelegateHandle first;
	SDelegateHandle second;
	uint32 firstCalls = 0u;
	uint32 secondCalls = 0u;
	first = Service->Subscribe(Content, {}, [&] (std::span<const assets::SFileChange>)
	{
		++firstCalls;
		EXPECT_TRUE(Service->Unsubscribe(first));
		EXPECT_TRUE(Service->Unsubscribe(second));   // not called in this delivery any more
	});
	second = Service->Subscribe(Content, {}, [&] (std::span<const assets::SFileChange>) { ++secondCalls; });
	ASSERT_EQ(Service->GetStats().Roots, 1u);

	WriteText(File("x.txt"), "x");
	ASSERT_TRUE(Service->Flush(Content));
	EXPECT_EQ(firstCalls, 1u);
	EXPECT_EQ(secondCalls, 0u);
	EXPECT_EQ(Service->GetStats().Subscriptions, 0u);
	EXPECT_EQ(Service->GetStats().Roots, 0u) << "the last subscription took its root with it";
	EXPECT_FALSE(Service->Unsubscribe(first));
}

TEST_F(FileWatcherServiceTest, ADirectoryThatDoesNotExistYetIsWatchedOnceItAppears)
{
	assets::SFileWatcherSettings settings;
	settings.RootPollMs = 50u;
	Service = std::make_unique<assets::CFileWatcherService>(settings);
	const std::filesystem::path later = Dir / FStringView("Пізніше").ToPath();
	Service->Subscribe(later, {}, Recorder.MakeCallback());
	EXPECT_EQ(Service->GetStats().Roots, 0u);
	EXPECT_FALSE(Service->Flush(later));

	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directories(later, ec));
	WriteText(later / "a.txt", "a");
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty(); })) << Recorder.Describe().CStr();
	EXPECT_EQ(Recorder.Changes[0].Kind, assets::EFileChange::Rescan) << "whatever is in it is news\n" << Recorder.Describe().CStr();
	EXPECT_EQ(Service->GetStats().Roots, 1u);

	Recorder.Clear();
	WriteText(later / "b.txt", "b");
	ASSERT_TRUE(Service->Flush(later));
	EXPECT_TRUE(Recorder.HasOnly("b.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
}

TEST_F(FileWatcherServiceTest, WhileTheVcsIsBusyNothingIsTold)
{
	assets::SFileWatcherSettings settings;
	settings.VcsSettleMs = 200u;
	Start(settings);
	bool bBusy = true;
	Service->SetVcsBusyQuery([&bBusy] { return bBusy; });

	WriteText(File("a.txt"), "a");
	PumpUntil([] { return false; }, 700ms);
	EXPECT_TRUE(Recorder.Changes.empty()) << Recorder.Describe().CStr();
	EXPECT_TRUE(Service->GetStats().bHolding);

	bBusy = false;
	const auto cleared = std::chrono::steady_clock::now();
	ASSERT_TRUE(PumpUntil([this] { return !Recorder.Changes.empty(); })) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.HasOnly("a.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_LT(std::chrono::steady_clock::now() - cleared, 2s);
}

TEST_F(FileWatcherServiceTest, ADisabledServiceWatchesNothingUntilItIsEnabled)
{
	assets::SFileWatcherSettings settings;
	settings.bEnabled = false;
	Service = std::make_unique<assets::CFileWatcherService>(settings);
	Service->Subscribe(Content, {}, Recorder.MakeCallback());
	EXPECT_EQ(Service->GetStats().Roots, 0u);
	WriteText(File("a.txt"), "a");
	EXPECT_FALSE(Service->Flush(Content));
	PumpUntil([] { return false; }, 300ms);
	EXPECT_TRUE(Recorder.Changes.empty());

	Service->SetEnabled(true);
	EXPECT_EQ(Service->GetStats().Roots, 1u);
	WriteText(File("b.txt"), "b");
	ASSERT_TRUE(Service->Flush(Content));
	EXPECT_TRUE(Recorder.HasOnly("b.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();
	EXPECT_TRUE(Recorder.Find("a.txt").empty()) << "watching starts when it is enabled\n" << Recorder.Describe().CStr();

	Service->SetEnabled(false);
	EXPECT_EQ(Service->GetStats().Roots, 0u);
	EXPECT_TRUE(Service->IsIdle());
}

TEST_F(FileWatcherServiceTest, EnabledForSomeDirectoriesOnlyTheRootsInOrAboveThemAreWatched)
{
	// A scripted step in the engine checkout watches its own directory, never the checkout's shaders
	// or materials that a developer may be editing meanwhile.
	assets::SFileWatcherSettings settings;
	settings.bEnabled = false;
	Service = std::make_unique<assets::CFileWatcherService>(settings);
	Service->Subscribe(Content, {}, Recorder.MakeCallback());
	SRecorder outside;
	Service->Subscribe(Outside, {}, outside.MakeCallback());
	EXPECT_EQ(Service->GetStats().Roots, 0u);

	const std::filesystem::path onlyContent[] = { File("Підкаталог") };   // a root above it counts
	Service->SetEnabled(true, onlyContent);
	EXPECT_TRUE(Service->IsEnabled());
	EXPECT_EQ(Service->GetStats().Roots, 1u);
	WriteText(File("a.txt"), "a");
	WriteText(Outside / "b.txt", "b");
	ASSERT_TRUE(Service->Flush(Content));
	EXPECT_FALSE(Service->Flush(Outside)) << "not watched";
	EXPECT_TRUE(Recorder.HasOnly("a.txt", assets::EFileChange::Added)) << Recorder.Describe().CStr();

	// A later subscription outside the scope is not watched either.
	const std::filesystem::path later = Dir / FStringView("Пізніше").ToPath();
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directories(later, ec));
	Service->Subscribe(later, {}, [] (std::span<const assets::SFileChange>) {});
	EXPECT_EQ(Service->GetStats().Roots, 1u);

	// Without a scope everything is watched, from now on; the root that was in scope kept watching.
	Recorder.Clear();
	WriteText(File("c.txt"), "c");
	Service->SetEnabled(true);
	EXPECT_EQ(Service->GetStats().Roots, 3u);
	WriteText(Outside / "d.txt", "d");
	ASSERT_TRUE(Service->Flush(Outside));
	EXPECT_TRUE(outside.HasOnly("d.txt", assets::EFileChange::Added)) << outside.Describe().CStr();
	EXPECT_TRUE(outside.Find("b.txt").empty()) << outside.Describe().CStr();
	ASSERT_TRUE(Service->Flush(Content));
	EXPECT_TRUE(Recorder.HasOnly("c.txt", assets::EFileChange::Added)) << "kept, not re-armed\n" << Recorder.Describe().CStr();

	// Narrowed again: the roots that left the scope are disarmed.
	const std::filesystem::path onlyOutside[] = { Outside };
	Service->SetEnabled(true, onlyOutside);
	EXPECT_EQ(Service->GetStats().Roots, 1u);
	EXPECT_FALSE(Service->Flush(Content));
	EXPECT_TRUE(Service->Flush(Outside));
}

TEST_F(FileWatcherServiceTest, SettingsComeFromTheConfig)
{
	config::CConfig config;
	EXPECT_TRUE(assets::SFileWatcherSettings::FromConfig(config).bEnabled) << "[Assets] Watch defaults to 1";
	EXPECT_EQ(assets::SFileWatcherSettings::FromConfig(config).VcsSettleMs, 1500u);

	config.SetOverride("Assets", "Watch", "0");
	config.SetOverride("AssetsEditor", "VcsSettleMs", "2500");
	EXPECT_FALSE(assets::SFileWatcherSettings::FromConfig(config).bEnabled);
	EXPECT_EQ(assets::SFileWatcherSettings::FromConfig(config).VcsSettleMs, 2500u);
}
