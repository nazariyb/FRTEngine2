#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "Project/Project.h"
#include "Project/UserRegistry.h"

namespace project = frt::project;


namespace
{
// Every test gets its own directory tree and its own per-user registry (FRT_USER_DATA_DIR), and
// no engine override: what the user has registered, or set, never leaks in.
class ProjectTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
		Root = std::filesystem::temp_directory_path() / "FrtProjectTest" / test->name();
		std::error_code ec;
		std::filesystem::remove_all(Root, ec);
		std::filesystem::create_directories(Root);
		SavedUserDataDir = project::GetEnvironmentPath(L"FRT_USER_DATA_DIR");
		SavedEngineDir = project::GetEnvironmentPath(L"FRT_ENGINE_DIR");
		SetEnvironment(L"FRT_USER_DATA_DIR", Root / "User");
		SetEnvironment(L"FRT_ENGINE_DIR", {});
	}

	void TearDown () override
	{
		SetEnvironment(L"FRT_USER_DATA_DIR", SavedUserDataDir);
		SetEnvironment(L"FRT_ENGINE_DIR", SavedEngineDir);
		std::error_code ec;
		std::filesystem::remove_all(Root, ec);
	}

	static void SetEnvironment (const wchar_t* Name, const std::filesystem::path& Value)
	{
		_wputenv_s(Name, Value.c_str());
	}

	static void Write (const std::filesystem::path& File, const std::string& Text)
	{
		std::filesystem::create_directories(File.parent_path());
		std::ofstream(File, std::ios::binary | std::ios::trunc) << Text;
	}

	std::filesystem::path MakeEngine (const char* Dir, const char* Name)
	{
		const std::filesystem::path dir = Root / Dir;
		Write(dir / project::EngineFileName, std::string("[Engine]\nName=") + Name + "\nVersion=1.2\n");
		return std::filesystem::canonical(dir);
	}

	std::filesystem::path MakeProject (const char* Dir, const char* Name, const std::string& Engine)
	{
		const std::filesystem::path file = Root / Dir / (std::string(Name) + std::string(project::ProjectFileExtension));
		Write(file, std::string("; a comment\n[Project]\nName=") + Name + (Engine.empty() ? "" : "\nEngine=" + Engine) + "\n");
		return file;
	}

	static project::SProjectDescriptor Load (const std::filesystem::path& File)
	{
		project::SProjectDescriptor descriptor;
		std::string error;
		EXPECT_TRUE(project::SProjectDescriptor::Load(File, descriptor, &error)) << error;
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
	std::string error;
	EXPECT_FALSE(project::SProjectDescriptor::Load(file, descriptor, &error));
	EXPECT_NE(error.find("Name"), std::string::npos);
	EXPECT_FALSE(project::SProjectDescriptor::Load(Root / "Missing.frtproject", descriptor, &error));
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
	std::ifstream in(file);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	EXPECT_NE(text.find("SharedModules=../Shared"), std::string::npos);
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
	SetEnvironment(L"FRT_ENGINE_DIR", other);

	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "Engine"));
	EXPECT_TRUE(IsSame(project::ResolveEngineDir(game), other));

	// An override that is no engine is an error, not a fallback.
	SetEnvironment(L"FRT_ENGINE_DIR", Root / "Nowhere");
	std::string error;
	EXPECT_TRUE(project::ResolveEngineDir(game, &error).empty());
	EXPECT_NE(error.find("FRT_ENGINE_DIR"), std::string::npos);
}

TEST_F(ProjectTest, UnknownEngineSaysWhichAndHow)
{
	const project::SProjectDescriptor game = Load(MakeProject("Game", "Game", "Unregistered"));
	std::string error;
	EXPECT_TRUE(project::ResolveEngineDir(game, &error).empty());
	EXPECT_NE(error.find("Unregistered"), std::string::npos);
	EXPECT_NE(error.find("setup"), std::string::npos);
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
	EXPECT_FALSE(project::IsValidProjectName(std::string(65, 'a')));
	EXPECT_TRUE(project::IsValidProjectName(std::string(64, 'a')));

	std::string error;
	EXPECT_FALSE(project::IsValidProjectName("editor", &error));
	EXPECT_NE(error.find("engine"), std::string::npos);
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
	Write(project::GetUserDataDir() / "Projects.ini",
		"[Recent]\n" + project::PathToUtf8(a) + "=100\n" + project::PathToUtf8(b) + "=200\n");
	const std::vector<project::SRecentProject> recent = project::GetRecentProjects();
	ASSERT_EQ(recent.size(), 2u);
	EXPECT_TRUE(IsSame(recent[0].File, b));
	EXPECT_EQ(recent[0].LastOpened, 200);

	project::RemoveRecentProject(b);
	ASSERT_EQ(project::GetRecentProjects().size(), 1u);
	EXPECT_TRUE(IsSame(project::GetRecentProjects()[0].File, a));
}
