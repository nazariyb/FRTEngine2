// The libraries that reload files in dev builds - materials and input action maps - do it through the
// file watcher service (Assets/FileWatcherService.h) instead of polling every file's time: a file they
// loaded is read again once the watcher reports it and its write time differs from the one it was read
// with, exactly that file, and nothing else (not its .meta beside it either); a removed file keeps what
// was loaded; a Rescan (an overflow) reads only the files whose time changed; and dropping the watcher
// ends every subscription.
// Also the rule they share, IsFileAffected. Everything runs in directories with non-ASCII names, on
// the real Windows watcher.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
#include <thread>

#include "Assets/FileWatcherService.h"
#include "Containers/String.h"
#include "Graphics/Render/MaterialLibrary.h"
#include "Input/InputActionLibrary.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

using namespace frt;
using namespace std::chrono_literals;


namespace
{
FString MaterialText (float Roughness)
{
	return FString::Format("version: 2\nname: Цегла\n\nalbedo:\n  color: {{ r: 0.8, g: 0.8, b: 0.8, a: 1 }}\n\nroughness: {}\n", Roughness);
}

constexpr FStringLiteral MapText =
	"version: 1\n"
	"action: IA_Стрибок\n"
	"type: Button\n"
	"bind: Key Space\n";

constexpr FStringLiteral MapTextWithTwoActions =
	"version: 1\n"
	"action: IA_Стрибок\n"
	"type: Button\n"
	"bind: Key Space\n"
	"\n"
	"action: IA_Присід\n"
	"type: Button\n"
	"bind: Key C\n";

class WatchedReloadTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Перезавантаження 重载 {}", platform::GetCurrentProcessId()).ToPath()
			/ "WatchedReload";
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

	// Writes Text to Path and moves its write time a few seconds on, so the change is seen whatever
	// the file system clock's granularity.
	static void Rewrite (const std::filesystem::path& Path, FStringView Text, int Seconds = 5)
	{
		std::error_code ec;
		const std::filesystem::file_time_type before = std::filesystem::last_write_time(Path, ec);
		ASSERT_TRUE(platform::WriteFileBytes(Path, Text.AsBytes())) << FString::FromPath(Path).CStr();
		std::filesystem::last_write_time(Path, before + std::chrono::seconds(Seconds), ec);
		ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	// Pumps Service every few milliseconds until Done() or Timeout.
	static bool PumpUntil (assets::CFileWatcherService& Service, const std::function<bool ()>& Done, std::chrono::milliseconds Timeout = 5s)
	{
		const auto deadline = std::chrono::steady_clock::now() + Timeout;
		for (;;)
		{
			Service.Pump();
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
};
}


TEST(FileChangeRule, AFileIsAffectedByAChangeOfItsPathOrOfADirectoryAboveItButNotByARemoval)
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / FStringView("Корінь").ToPath();
	const std::filesystem::path file = root / FStringView("Матеріали/Цегла.frtmat.yml").ToPath();

	assets::SFileChange modified;
	modified.Kind = assets::EFileChange::Modified;
	modified.Path = root / FStringView("матеріали/цегла.FRTMAT.yml").ToPath();
	EXPECT_TRUE(assets::IsFileAffected(modified, file)) << "names compare ignoring case";

	assets::SFileChange other = modified;
	other.Path = root / FStringView("Матеріали/Камінь.frtmat.yml").ToPath();
	EXPECT_FALSE(assets::IsFileAffected(other, file));

	// Its meta (M4) is another file: the runtime loads by path and never reads one (design §3.2).
	assets::SFileChange meta = modified;
	meta.Path = root / FStringView("Матеріали/Цегла.frtmat.yml.meta").ToPath();
	EXPECT_FALSE(assets::IsFileAffected(meta, file));

	assets::SFileChange rescan;
	rescan.Kind = assets::EFileChange::Rescan;
	rescan.bDirectory = true;
	rescan.Path = root;
	EXPECT_TRUE(assets::IsFileAffected(rescan, file)) << "a directory change stands for everything below it";

	assets::SFileChange fileAbove = modified;
	fileAbove.Path = root / FStringView("Матеріали").ToPath();
	EXPECT_FALSE(assets::IsFileAffected(fileAbove, file)) << "a file change is about that file only";

	assets::SFileChange moved;
	moved.Kind = assets::EFileChange::Moved;
	moved.Path = file;
	moved.OldPath = root / FStringView("Старе.frtmat.yml").ToPath();
	EXPECT_TRUE(assets::IsFileAffected(moved, file)) << "moved onto it";
	EXPECT_FALSE(assets::IsFileAffected(moved, moved.OldPath)) << "gone from the old path";

	assets::SFileChange removed = modified;
	removed.Kind = assets::EFileChange::Removed;
	EXPECT_FALSE(assets::IsFileAffected(removed, file));
	removed.bDirectory = true;
	removed.Path = root;
	EXPECT_FALSE(assets::IsFileAffected(removed, file));
}

TEST_F(WatchedReloadTest, AMaterialIsReadAgainWhenTheWatcherReportsItsFileChanged)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = Dir / FStringView("Цегла.frtmat.yml").ToPath();
	const std::filesystem::path other = Dir / FStringView("Камінь.frtmat.yml").ToPath();
	ASSERT_TRUE(platform::WriteFileText(path, MaterialText(0.1f)));
	ASSERT_TRUE(platform::WriteFileText(other, MaterialText(0.2f)));

	assets::CFileWatcherService service;
	graphics::CMaterialLibrary library;
	library.SetFileWatcher(&service);
	memory::TRefShared<graphics::SMaterial> material = library.LoadOrCreateMaterial(path, {});
	ASSERT_TRUE(material);
	EXPECT_FLOAT_EQ(material->Roughness, 0.1f);
	EXPECT_EQ(service.GetStats().Subscriptions, 1u) << "one subscription for the material's directory";
	const uint32 version = material->Version;
	EXPECT_FALSE(library.ReloadModifiedMaterials()) << "nothing reported, nothing read";

	// A file it never loaded is not read, nor is the loaded material's meta written beside it (M4: nothing at run time
	// reads a meta, design §3.2). The material is made to look stale for this step (its read time no longer its
	// file's), so a report that marked it for either write would read it again: only the reports decide here, not the
	// write time.
	const std::filesystem::file_time_type readTime = material->LastWriteTime;
	material->LastWriteTime = readTime - std::chrono::seconds(60);
	Rewrite(other, MaterialText(0.9f));
	ASSERT_TRUE(platform::WriteFileText(Dir / FStringView("Цегла.frtmat.yml.meta").ToPath(),
		"FormatVersion: 1\nGuid: 5d1e7a3b9c0f42d68e1a2b3c4d5e6f70\n"));
	ASSERT_TRUE(service.Flush(Dir));
	EXPECT_FALSE(library.ReloadModifiedMaterials()) << "neither the other material's report nor its meta's marks it";
	EXPECT_EQ(material->Version, version);
	material->LastWriteTime = readTime;

	Rewrite(path, MaterialText(0.6f));
	ASSERT_TRUE(service.Flush(Dir));
	EXPECT_TRUE(library.ReloadModifiedMaterials());
	EXPECT_FLOAT_EQ(material->Roughness, 0.6f);
	EXPECT_GT(material->Version, version) << "the GPU scene uploads it again";
	EXPECT_FALSE(library.ReloadModifiedMaterials()) << "read once";

	// Removed: what was loaded stays.
	const uint32 loaded = material->Version;
	ASSERT_TRUE(platform::RemoveFile(path));
	ASSERT_TRUE(service.Flush(Dir));
	EXPECT_FALSE(library.ReloadModifiedMaterials());
	EXPECT_EQ(material->Version, loaded);
	EXPECT_FLOAT_EQ(material->Roughness, 0.6f);

	library.SetFileWatcher(nullptr);
	EXPECT_EQ(service.GetStats().Subscriptions, 0u) << "no watcher, no subscriptions";
}

TEST(FileChangeRule, AnIndexOfChangesAnswersAsTheRuleDoes)
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / FStringView("Корінь").ToPath();
	std::vector<assets::SFileChange> changes(4u);
	changes[0].Kind = assets::EFileChange::Modified;
	changes[0].Path = root / FStringView("Матеріали/Цегла.frtmat.yml").ToPath();
	changes[1].Kind = assets::EFileChange::Rescan;
	changes[1].bDirectory = true;
	changes[1].Path = root / FStringView("Шейдери").ToPath();
	changes[2].Kind = assets::EFileChange::Removed;
	changes[2].Path = root / FStringView("Старе.frtmat.yml").ToPath();
	changes[3].Kind = assets::EFileChange::Moved;
	changes[3].Path = root / FStringView("s.ini").ToPath();
	changes[3].OldPath = root / FStringView("Було.ini").ToPath();
	const assets::CAffectedFiles affected(changes);

	const std::filesystem::path files[] = {
		root / FStringView("матеріали/ЦЕГЛА.frtmat.yml").ToPath(), root / FStringView("Шейдери/Common.hlsli").ToPath(),
		root / FStringView("Шейдери/Глибше/x.hlsli").ToPath(), root / FStringView("Старе.frtmat.yml").ToPath(), root / FStringView("s.ini").ToPath(),
		root / FStringView("\u017f.ini").ToPath(), root / FStringView("Було.ini").ToPath(), root / FStringView("Шейдери2/x.hlsli").ToPath(),
		root / FStringView("Матеріали/../Шейдери/y.hlsli").ToPath() };
	for (const std::filesystem::path& file : files)
	{
		bool bAny = false;
		for (const assets::SFileChange& change : changes)
		{
			bAny = bAny || assets::IsFileAffected(change, file);
		}
		EXPECT_EQ(affected.Contains(file), bAny) << FString::FromPath(file).CStr();
	}
	EXPECT_TRUE(affected.Contains(files[0]));
	EXPECT_TRUE(affected.Contains(files[2])) << "below a rescanned directory";
	EXPECT_TRUE(affected.Contains(files[8])) << "spelled with '..'";
	EXPECT_FALSE(affected.Contains(files[3])) << "a removal affects nothing loaded";
	EXPECT_FALSE(affected.Contains(files[5])) << "U+017F is not 's' on NTFS";
	EXPECT_FALSE(affected.Contains(files[7]));
	EXPECT_TRUE(assets::CAffectedFiles(std::span(changes).subspan(2u, 1u)).IsEmpty());
}

TEST_F(WatchedReloadTest, AMaterialReadFromAFileOfAnyNameIsReadAgain)
{
	// The serializers choose a reader by content, not by name; the watcher must not choose by name either.
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = Dir / FStringView("Цегла.матеріал").ToPath();
	ASSERT_TRUE(platform::WriteFileText(path, MaterialText(0.1f)));

	assets::CFileWatcherService service;
	graphics::CMaterialLibrary library;
	library.SetFileWatcher(&service);
	const memory::TRefShared<graphics::SMaterial> material = library.LoadOrCreateMaterial(path, {});
	ASSERT_TRUE(material);
	EXPECT_FLOAT_EQ(material->Roughness, 0.1f);

	Rewrite(path, MaterialText(0.7f));
	ASSERT_TRUE(service.Flush(Dir));
	EXPECT_TRUE(library.ReloadModifiedMaterials());
	EXPECT_FLOAT_EQ(material->Roughness, 0.7f);
	library.SetFileWatcher(nullptr);
}

TEST_F(WatchedReloadTest, ARescanReadsOnlyTheMaterialsWhoseFilesChanged)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path changed = Dir / FStringView("Змінений.frtmat.yml").ToPath();
	const std::filesystem::path unchanged = Dir / FStringView("Незмінний.frtmat.yml").ToPath();
	ASSERT_TRUE(platform::WriteFileText(changed, MaterialText(0.1f)));
	ASSERT_TRUE(platform::WriteFileText(unchanged, MaterialText(0.2f)));

	// A buffer too small for any record: every change overflows, and the root is told as a Rescan.
	assets::SFileWatcherSettings settings;
	settings.Watcher.BufferBytes = 64u;
	assets::CFileWatcherService service(settings);
	graphics::CMaterialLibrary library;
	library.SetFileWatcher(&service);
	const memory::TRefShared<graphics::SMaterial> a = library.LoadOrCreateMaterial(changed, {});
	const memory::TRefShared<graphics::SMaterial> b = library.LoadOrCreateMaterial(unchanged, {});
	const uint32 versionA = a->Version;
	const uint32 versionB = b->Version;

	Rewrite(changed, MaterialText(0.7f));
	ASSERT_TRUE(PumpUntil(service, [&library] { return library.ReloadModifiedMaterials(); }))
		<< "the Rescan reads the changed one (stats: " << service.GetStats().Rescans << " rescans)";
	EXPECT_GE(service.GetStats().Rescans, 1u);
	EXPECT_FLOAT_EQ(a->Roughness, 0.7f);
	EXPECT_GT(a->Version, versionA);
	EXPECT_EQ(b->Version, versionB) << "compared, unchanged, not read";
}

TEST_F(WatchedReloadTest, AnActionMapIsReadAgainInsideThePumpThatReportsItsFile)
{
	FRT_TEST_MEMORY_POOL();
	const std::filesystem::path path = Dir / FStringView("IAM_Гра.frtinputmap").ToPath();
	ASSERT_TRUE(platform::WriteFileText(path, MapText));

	assets::CFileWatcherService service;
	input::CInputActionLibrary library;
	library.SetFileWatcher(&service);
	input::SInputActionMapAsset* map = library.LoadOrCreateActionMap(path);
	ASSERT_NE(map, nullptr);
	EXPECT_NE(map->ActionMap.FindAction("IA_Стрибок"), nullptr);
	EXPECT_EQ(map->ActionMap.FindAction("IA_Присід"), nullptr);
	EXPECT_EQ(service.GetStats().Subscriptions, 1u);

	// Read right in the callback: a frame's BeginFrame pumps the watcher before it evaluates the maps.
	Rewrite(path, MapTextWithTwoActions);
	ASSERT_TRUE(service.Flush(Dir));
	EXPECT_NE(map->ActionMap.FindAction("IA_Присід"), nullptr) << "the saved binding is there";

	library.SetFileWatcher(nullptr);
	EXPECT_EQ(service.GetStats().Subscriptions, 0u);
	Rewrite(path, MapText, 10);
	EXPECT_FALSE(service.Flush(Dir)) << "nothing watches the directory any more";
	EXPECT_NE(map->ActionMap.FindAction("IA_Присід"), nullptr) << "and nothing is read";
}
