#include <filesystem>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Platform/Platform.h"
#include "Project/Project.h"
#include "Project/UserRegistry.h"

using frt::EPathStyle;
using frt::FString;
using frt::FStringView;
namespace platform = frt::platform;
namespace project = frt::project;


namespace
{
// Every test gets its own directory tree and its own per-user registry (FRT_USER_DATA_DIR), and
// no engine override: what the user has registered, or set, never leaks in. The tree is under a
// directory with a non-ASCII name, so every path below goes through UTF-16 all the way.
class ProjectTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
		Root = std::filesystem::temp_directory_path() / FStringView("FrtProjectTest Тест 漢字 😀").ToPath()
			/ FStringView(test->name()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Root, ec);
		std::filesystem::create_directories(Root);
		SavedUserDataDir = platform::GetEnvironmentPath("FRT_USER_DATA_DIR");
		SavedEngineDir = platform::GetEnvironmentPath("FRT_ENGINE_DIR");
		platform::SetEnvironmentPath("FRT_USER_DATA_DIR", Root / "User");
		platform::SetEnvironmentPath("FRT_ENGINE_DIR", {});
	}

	void TearDown () override
	{
		platform::SetEnvironmentPath("FRT_USER_DATA_DIR", SavedUserDataDir);
		platform::SetEnvironmentPath("FRT_ENGINE_DIR", SavedEngineDir);
		std::error_code ec;
		std::filesystem::remove_all(Root, ec);
	}

	static void Write (const std::filesystem::path& File, FStringView Text)
	{
		EXPECT_TRUE(platform::WriteFileText(File, Text));   // creates the directories
	}

	std::filesystem::path MakeEngine (FStringView Dir, FStringView Name)
	{
		const std::filesystem::path dir = Root / Dir.ToPath();
		Write(dir / project::EngineFileName.ToPath(), FString::Format("[Engine]\nName={}\nVersion=1.2\n", Name));
		return std::filesystem::canonical(dir);
	}

	std::filesystem::path MakeProject (FStringView Dir, FStringView Name, FStringView Engine)
	{
		const std::filesystem::path file = Root / Dir.ToPath() / (Name + project::ProjectFileExtension).ToPath();
		FString text = FString::Format("; a comment\n[Project]\nName={}\n", Name);
		if (!Engine.IsEmpty())
		{
			text.AppendFormat("Engine={}\n", Engine);
		}
		Write(file, text);
		return file;
	}

	static project::SProjectDescriptor Load (const std::filesystem::path& File)
	{
		project::SProjectDescriptor descriptor;
		FString error;
		EXPECT_TRUE(project::SProjectDescriptor::Load(File, descriptor, &error)) << error.CStr();
		return descriptor;
	}

	static bool IsSame (const std::filesystem::path& A, const std::filesystem::path& B)
	{
		std::error_code ec;
		return std::filesystem::equivalent(A, B, ec);
	}

	std::filesystem::path Root;
	std::filesystem::path SavedUserDataDir;
	std::filesystem::path SavedEngineDir;
};
}


TEST_F(ProjectTest, DescriptorReadsNameModuleAndEngine)
{
	const std::filesystem::path file = Root / "Game" / "Game.frtproject";
	Write(file, "[Project]\nName=Game\nModule=GameCode\nEngine=Engine\n");

	const project::SProjectDescriptor descriptor = Load(file);
	EXPECT_EQ(descriptor.Name, "Game");
	EXPECT_EQ(descriptor.Module, "GameCode");
	EXPECT_EQ(descriptor.Engine, "Engine");
	EXPECT_TRUE(descriptor.File.is_absolute());
	EXPECT_TRUE(IsSame(descriptor.GetDir(), Root / "Game"));
}

TEST_F(ProjectTest, ModuleDefaultsToTheName)
{
	EXPECT_EQ(Load(MakeProject("Game", "Game", "FRTEngine2")).Module, "Game");
}

TEST_F(ProjectTest, DescriptorWithoutANameIsRejected)
{
	const std::filesystem::path file = Root / "Game" / "Game.frtproject";
	Write(file, "[Project]\nEngine=Engine\n");

	project::SProjectDescriptor descriptor;
	FString error;
	EXPECT_FALSE(project::SProjectDescriptor::Load(file, descriptor, &error));
	EXPECT_TRUE(error.Contains("Name"));
	EXPECT_FALSE(project::SProjectDescriptor::Load(Root / "Missing.frtproject", descriptor, &error));
}

TEST_F(ProjectTest, DescriptorNamingAnEngineProjectIsRejected)
{
	// Module=Core would make the generated solution's game project the engine's Core.
	const std::filesystem::path file = Root / "Game" / "Game.frtproject";
	Write(file, "[Project]\nName=Game\nModule=Core\n");
	project::SProjectDescriptor descriptor;
	FString error;
	EXPECT_FALSE(project::SProjectDescriptor::Load(file, descriptor, &error));
	EXPECT_TRUE(error.Contains("engine"));

	Write(file, "[Project]\nName=My Game\n");
	EXPECT_FALSE(project::SProjectDescriptor::Load(file, descriptor, &error));
}

TEST_F(ProjectTest, ProjectFileExtensionInAnyCase)
{
	const std::filesystem::path file = Root / "Game" / "Game.FRTPROJECT";
	Write(file, "[Project]\nName=Game\n");
	EXPECT_TRUE(IsSame(project::FindProjectFile(Root / "Game"), file));
}

TEST_F(ProjectTest, TextThatIsNotUtf8IsNoPath)
{
	// An ANSI-encoded file with a non-ASCII path: no path, and no exception.
	EXPECT_FALSE(FStringView("C:\\Users\\J\xF6rg").TryToPath().has_value());
	EXPECT_TRUE(FStringView("C:\\Users\\J\xC3\xB6rg").TryToPath().has_value());

	// Such a registry entry finds no engine, rather than one at a mangled path.
	Write(project::GetUserDataDir() / "Engines.ini", "[Engines]\nAnsi=C:\\Users\\J\xF6rg\\Engine\n");
	EXPECT_TRUE(project::FindRegisteredEngine("Ansi").empty());
}

TEST_F(ProjectTest, SaveKeepsWhatElseTheFileHolds)
{
	const std::filesystem::path file = Root / "Game" / "Game.frtproject";
	Write(file, "[Project]\nName=Game\nSharedModules=../Shared\n");

	project::SProjectDescriptor descriptor = Load(file);
	descriptor.Engine = "Engine";
	ASSERT_TRUE(descriptor.Save());

	const project::SProjectDescriptor saved = Load(file);
	EXPECT_EQ(saved.Engine, "Engine");
	const std::optional<FString> text = platform::ReadFileText(file);
	ASSERT_TRUE(text.has_value());
	EXPECT_TRUE(text->Contains("SharedModules=../Shared"));
}

TEST_F(ProjectTest, ProjectFileIsTheOnlyOneInItsDirectory)
{
	EXPECT_TRUE(project::FindProjectFile(Root).empty());

	const std::filesystem::path file = MakeProject("Game", "Game", "");
	EXPECT_TRUE(IsSame(project::FindProjectFile(Root / "Game"), file));
	EXPECT_TRUE(IsSame(project::FindProjectFileAbove(Root / "Game" / "Source" / "Deep"), file));

	// Two: ambiguous, so none - the host is told to name one.
	MakeProject("Game", "Other", "");
	EXPECT_TRUE(project::FindProjectFile(Root / "Game").empty());
}

TEST_F(ProjectTest, EngineAsAPathRelativeToTheProject)
{
	const std::filesystem::path engine = MakeEngine("Game/Engine", "Embedded");
	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "Engine"));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), engine));
}

TEST_F(ProjectTest, EngineByRegisteredName)
{
	const std::filesystem::path engine = MakeEngine("Engines/Shared", "SharedEngine");
	ASSERT_TRUE(project::RegisterEngine("SharedEngine", engine));
	EXPECT_TRUE(IsSame(project::FindRegisteredEngine("sharedengine"), engine));

	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "SharedEngine"));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), engine));
}

TEST_F(ProjectTest, APathWinsOverARegisteredName)
{
	// A directory next to the project called like a registered engine is the one used.
	const std::filesystem::path registered = MakeEngine("Engines/Shared", "Engine");
	project::RegisterEngine("Engine", registered);
	const std::filesystem::path embedded = MakeEngine("Game/Engine", "Engine");

	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "Engine"));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), embedded));
}

TEST_F(ProjectTest, ProjectInsideAnEngineUsesIt)
{
	const std::filesystem::path engine = MakeEngine("Engine", "Outer");
	const project::SProjectDescriptor game = Load(MakeProject("Engine/Templates/Game", "Game", ""));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), engine));
}

TEST_F(ProjectTest, EnvironmentOverrideWinsOverEverything)
{
	MakeEngine("Game/Engine", "Embedded");
	const std::filesystem::path other = MakeEngine("Other", "Other");
	platform::SetEnvironmentPath("FRT_ENGINE_DIR", other);

	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "Engine"));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), other));

	// An override that is no engine is an error, not a fallback.
	platform::SetEnvironmentPath("FRT_ENGINE_DIR", Root / "Nowhere");
	FString error;
	EXPECT_TRUE(project::ResolveEngineDir(game, &error).empty());
	EXPECT_TRUE(error.Contains("FRT_ENGINE_DIR"));
}

TEST_F(ProjectTest, UnknownEngineSaysWhichAndHow)
{
	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "Unregistered"));
	FString error;
	EXPECT_TRUE(project::ResolveEngineDir(game, &error).empty());
	EXPECT_TRUE(error.Contains("Unregistered"));
	EXPECT_TRUE(error.Contains("setup"));
}

TEST_F(ProjectTest, ProjectNamesAreIdentifiersButNotEngineParts)
{
	EXPECT_TRUE(project::IsValidProjectName("MyGame"));
	EXPECT_TRUE(project::IsValidProjectName("My_Game2"));
	EXPECT_FALSE(project::IsValidProjectName(""));
	EXPECT_FALSE(project::IsValidProjectName("2Fast"));
	EXPECT_FALSE(project::IsValidProjectName("_Game"));
	EXPECT_FALSE(project::IsValidProjectName("My Game"));
	EXPECT_FALSE(project::IsValidProjectName("My-Game"));
	EXPECT_FALSE(project::IsValidProjectName("Гра"));
	EXPECT_FALSE(project::IsValidProjectName(FString::Repeat("a", 65)));
	EXPECT_TRUE(project::IsValidProjectName(FString::Repeat("a", 64)));

	FString error;
	EXPECT_FALSE(project::IsValidProjectName("editor", &error));
	EXPECT_TRUE(error.Contains("engine"));
	EXPECT_FALSE(project::IsValidProjectName("Core"));
	EXPECT_FALSE(project::IsValidProjectName("ImGui"));
}

TEST_F(ProjectTest, RecentProjectsAreOneEntryPerProjectNewestFirst)
{
	const std::filesystem::path a = MakeProject("A", "A", "");
	const std::filesystem::path b = MakeProject("B", "B", "");
	project::AddRecentProject(a);
	// Another spelling of the same file: still one entry.
	project::AddRecentProject(Root / "A" / ".." / "A" / "A.frtproject");
	project::AddRecentProject(b);
	EXPECT_EQ(project::GetRecentProjects().size(), 2u);

	// Newest first, by the recorded time.
	Write(project::GetUserDataDir() / "Projects.ini", FString::Format("[Recent]\n{}=100\n{}=200\n",
		FString::FromPath(a, EPathStyle::Preferred), FString::FromPath(b, EPathStyle::Preferred)));
	const std::vector<project::SRecentProject> recent = project::GetRecentProjects();
	ASSERT_EQ(recent.size(), 2u);
	EXPECT_TRUE(IsSame(recent[0].File, b));
	EXPECT_EQ(recent[0].LastOpened, 200);

	project::RemoveRecentProject(b);
	ASSERT_EQ(project::GetRecentProjects().size(), 1u);
	EXPECT_TRUE(IsSame(project::GetRecentProjects()[0].File, a));
}

TEST_F(ProjectTest, NonAsciiDirectoriesAndEngineNames)
{
	// A project and its engine in directories named in Cyrillic, CJK and emoji; the engine
	// registered under a non-ASCII name, as the registry files store it (UTF-8).
	const std::filesystem::path engine = MakeEngine("Рушії 引擎/Спільний 😀", "Рушій");
	ASSERT_TRUE(project::RegisterEngine("Рушій", engine));
	EXPECT_TRUE(IsSame(project::FindRegisteredEngine("Рушій"), engine));
	ASSERT_EQ(project::GetRegisteredEngines().size(), 1u);
	EXPECT_EQ(project::GetRegisteredEngines()[0].first, "Рушій");
	EXPECT_TRUE(IsSame(project::GetRegisteredEngines()[0].second, engine));

	const std::filesystem::path file = MakeProject("Ігри 游戏/Гра 😀", "Game", "Рушій");
	const project::SProjectDescriptor game = Load(file);
	EXPECT_EQ(game.Engine, "Рушій");
	EXPECT_TRUE(IsSame(game.GetDir(), Root / FStringView("Ігри 游戏").ToPath() / FStringView("Гра 😀").ToPath()));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), engine));
	EXPECT_TRUE(IsSame(project::FindProjectFileAbove(game.GetDir() / "Source"), file));

	// An engine named by a path relative to the project, through non-ASCII directories.
	Write(file, "[Project]\nName=Game\nEngine=../../Рушії 引擎/Спільний 😀\n");
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(Load(file)), engine));

	project::AddRecentProject(file);
	const std::vector<project::SRecentProject> recent = project::GetRecentProjects();
	ASSERT_EQ(recent.size(), 1u);
	EXPECT_TRUE(IsSame(recent[0].File, file));

	// A message naming the file names it in UTF-8.
	project::SProjectDescriptor descriptor;
	FString error;
	EXPECT_FALSE(project::SProjectDescriptor::Load(file.parent_path() / FStringView("Відсутній.frtproject").ToPath(), descriptor, &error));
	EXPECT_TRUE(error.Contains("Відсутній.frtproject"));
}

TEST_F(ProjectTest, PackageMarkerIsTheProjectDescriptorOfItsDirectory)
{
	// As FRT.bat package writes it: the project's descriptor, plus what built it.
	const std::filesystem::path file = Root / "MyGame-Win64" / "MyGame.frtpackage";
	Write(file, "; a comment\n[Project]\nName=MyGame\nModule=MyGameCode\n\n[Package]\nEngine=FRTEngine2 0.1\nConfiguration=Release-Default\n");
	Write(Root / "MyGame-Win64" / "MyGame.frtproject", "[Project]\nName=Other\n");

	const std::filesystem::path found = project::FindPackageFile(Root / "MyGame-Win64");
	EXPECT_TRUE(IsSame(found, file));
	const project::SProjectDescriptor descriptor = Load(found);
	EXPECT_EQ(descriptor.Name, "MyGame");
	EXPECT_EQ(descriptor.Module, "MyGameCode");
	EXPECT_TRUE(IsSame(descriptor.GetDir(), Root / "MyGame-Win64"));

	// None, or more than one, is no package.
	EXPECT_TRUE(project::FindPackageFile(Root).empty());
	Write(Root / "MyGame-Win64" / "Another.frtpackage", "[Project]\nName=Another\n");
	EXPECT_TRUE(project::FindPackageFile(Root / "MyGame-Win64").empty());
}

TEST_F(ProjectTest, NothingHereRunsAsAPackage)
{
	// The tests' executable is in the engine's Binaries, where nothing is packaged.
	EXPECT_TRUE(project::FindRunningPackage().empty());
}

TEST_F(ProjectTest, GameUserDataIsPerUserUnderTheGamesName)
{
	EXPECT_EQ(project::GetGameUserDataDir("MyGame"), Root / "User" / "MyGame");
	EXPECT_EQ(project::GetGameUserDataDir("Гра 😀"), Root / "User" / FStringView("Гра 😀").ToPath());

	platform::SetEnvironmentPath("FRT_USER_DATA_DIR", {});
	const std::filesystem::path local = platform::GetEnvironmentPath("LOCALAPPDATA");
	if (!local.empty())
	{
		EXPECT_EQ(project::GetGameUserDataDir("MyGame"), local / "MyGame");
	}
}
