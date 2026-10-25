// The mount table (Assets/Discovery/Mounts.h, design §3.1, §5.4 step 1): which mounts a layout yields and
// with which ids (/Engine 0, /Project 1, plugins from 2 in folded name order, a plugin only with a Content
// folder), where each one's rule file and owner root are, and the only conversions between absolute paths
// and mount paths - the root prefix by the volume's rule, the remainder in its own spelling, and nothing
// that could leave a mount ('..', '\', a drive). Also the process-wide wrappers paths::ToMountPath and
// paths::FromMountPath over an installed table. Every tree is generated, with non-ASCII names.
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "DiscoveryTestUtils.h"

#include "EnginePaths.h"
#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/Mounts.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"

using namespace frt;
using namespace frt::assets;
using frt::test::discovery::CDiscoveryTree;


namespace
{
void MakeDirectories (const std::filesystem::path& Path)
{
	std::error_code ec;
	std::filesystem::create_directories(Path, ec);
}

SMountRoots MakeRoots (const CDiscoveryTree& Tree, bool bProject = true)
{
	SMountRoots roots;
	roots.EngineDir = Tree.EngineDir();
	roots.EngineContentDir = Tree.EngineContent();
	roots.EngineConfigDir = Tree.EngineConfig();
	roots.ProjectDir = bProject ? Tree.ProjectDir() : std::filesystem::path();
	return roots;
}

void MakeStandardLayout (const CDiscoveryTree& Tree)
{
	MakeDirectories(Tree.EngineContent());
	MakeDirectories(Tree.EngineConfig());
	MakeDirectories(Tree.ProjectContent());
	MakeDirectories(Tree.ProjectDir() / "Config");
}
}


TEST(MountsTest, ResolveMountsTheEngineAndTheProjectWithTheirRuleFilesAndOwnerRoots)
{
	CDiscoveryTree tree("Engine and project");
	MakeStandardLayout(tree);

	std::vector<SDiagnostic> diagnostics;
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), &diagnostics);
	EXPECT_TRUE(diagnostics.empty());
	ASSERT_EQ(table.GetMounts().size(), 2u);

	const SMount& engine = table.GetMounts()[0];
	EXPECT_EQ(engine.Name, "Engine");
	EXPECT_EQ(engine.Id, EngineMountId);
	EXPECT_EQ(engine.Kind, EMountKind::Engine);
	EXPECT_EQ(engine.ContentDir, platform::NormalizePath(tree.EngineContent()));
	EXPECT_EQ(engine.RulesFile, platform::NormalizePath(tree.EngineConfig() / "Assets.yml"));
	EXPECT_EQ(engine.OwnerRoot, platform::NormalizePath(tree.EngineDir()));
	EXPECT_EQ(engine.GetRootMountPath(), "/Engine");
	EXPECT_EQ(engine.Volume.FileSystemName, platform::QueryVolumeInfo(tree.EngineContent())->FileSystemName);

	const SMount& project = table.GetMounts()[1];
	EXPECT_EQ(project.Name, "Project");
	EXPECT_EQ(project.Id, ProjectMountId);
	EXPECT_EQ(project.Kind, EMountKind::Project);
	EXPECT_EQ(project.ContentDir, platform::NormalizePath(tree.ProjectContent()));
	EXPECT_EQ(project.RulesFile, platform::NormalizePath(tree.ProjectDir() / "Config" / "Assets.yml"));
	EXPECT_EQ(project.OwnerRoot, platform::NormalizePath(tree.ProjectDir()));

	EXPECT_EQ(table.Find("Engine"), &engine);
	EXPECT_EQ(table.Find("project"), &project);   // folded
	EXPECT_EQ(table.Find(ProjectMountId), &project);
	EXPECT_EQ(table.Find("Plugin.X"), nullptr);
	EXPECT_EQ(table.Find(MountId(7u)), nullptr);
}

TEST(MountsTest, WithoutAProjectOnlyTheEngineIsMounted)
{
	CDiscoveryTree tree("No project");
	MakeStandardLayout(tree);
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree, false), nullptr);
	ASSERT_EQ(table.GetMounts().size(), 1u);
	EXPECT_EQ(table.GetMounts()[0].Name, "Engine");
	EXPECT_EQ(table.Find("Project"), nullptr);
}

TEST(MountsTest, AMissingContentDirectoryStillMountsWithAWarning)
{
	CDiscoveryTree tree("Missing content");
	MakeDirectories(tree.EngineContent());
	MakeDirectories(tree.ProjectDir());

	std::vector<SDiagnostic> diagnostics;
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), &diagnostics);
	ASSERT_EQ(table.GetMounts().size(), 2u);
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_EQ(diagnostics[0].Subject, "/Project");
	EXPECT_EQ(diagnostics[0].Severity, ESeverity::Warning);
	EXPECT_EQ(diagnostics[0].Code, 0u);
	EXPECT_FALSE(table.GetMounts()[1].Volume.bCaseSensitive);
}

// A plugin mounts only with a Content folder; ids follow SStringLessIgnoreCase of the names, then bytes;
// dot-named folders are never plugins.
TEST(MountsTest, PluginsMountInFoldedNameOrderOnlyWithAContentFolder)
{
	CDiscoveryTree tree("Plugins");
	MakeStandardLayout(tree);
	const std::filesystem::path plugins = tree.ProjectDir() / "Plugins";
	for (const FStringView name : { FStringView("Zebra"), FStringView("ägypten"), FStringView("alpha"), FStringView("Beta 测试"),
		FStringView(".hidden") })
	{
		MakeDirectories(plugins / name.ToPath() / "Content");
	}
	MakeDirectories(plugins / "NoContent" / "Config");
	ASSERT_TRUE(test::discovery::WriteText(plugins / "File.txt"));

	std::vector<SDiagnostic> diagnostics;
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), &diagnostics);
	EXPECT_TRUE(diagnostics.empty());
	ASSERT_EQ(table.GetMounts().size(), 6u);
	const char* expected[] = { "Engine", "Project", "Plugin.alpha", "Plugin.Beta 测试", "Plugin.Zebra", "Plugin.ägypten" };
	for (uint32 i = 0u; i < 6u; ++i)
	{
		const SMount& mount = table.GetMounts()[i];
		EXPECT_EQ(mount.Name, expected[i]) << i;
		EXPECT_EQ(mount.Id, i);
	}
	const SMount& beta = table.GetMounts()[3];
	EXPECT_EQ(beta.Kind, EMountKind::Plugin);
	EXPECT_EQ(beta.ContentDir, platform::NormalizePath(plugins / FStringView("Beta 测试").ToPath() / "Content"));
	EXPECT_EQ(beta.RulesFile, platform::NormalizePath(plugins / FStringView("Beta 测试").ToPath() / "Config" / "Assets.yml"));
	EXPECT_EQ(beta.OwnerRoot, platform::NormalizePath(tree.ProjectDir()));
	EXPECT_EQ(beta.GetRootMountPath(), "/Plugin.Beta 测试");
	EXPECT_EQ(table.Find("plugin.BETA 测试"), &beta);
}

// A plugin folder whose name is not valid UTF-8 cannot be a mount name: PATH002, not mounted.
TEST(MountsTest, APluginFolderWithAnIllFormedNameIsNotMounted)
{
	CDiscoveryTree tree("Ill-formed plugin");
	MakeStandardLayout(tree);
	const std::filesystem::path plugin = tree.ProjectDir() / "Plugins" / FStringView("Bad\xED\xA0\x80").ToPath();   // U+D800 alone
	MakeDirectories(plugin / "Content");
	if (!platform::Stat(plugin / "Content").has_value())
	{
		GTEST_SKIP() << "this file system refuses a name with a lone surrogate";
	}

	std::vector<SDiagnostic> diagnostics;
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), &diagnostics);
	EXPECT_EQ(table.GetMounts().size(), 2u);
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_EQ(diagnostics[0].Code, Path002);
	EXPECT_EQ(diagnostics[0].Subject, "/Plugin.Bad\xEF\xBF\xBD");
}

TEST(MountsTest, ToMountPathKeepsTheSpellingBelowTheRootAndMatchesTheRootByTheVolumesRule)
{
	CDiscoveryTree tree("To mount path");
	MakeStandardLayout(tree);
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), nullptr);

	const std::filesystem::path file = tree.ProjectContent() / FStringView("Текстури/Камінь 漢字.png").ToPath();
	EXPECT_EQ(table.ToMountPath(file), std::optional<FString>("/Project/Текстури/Камінь 漢字.png"));
	EXPECT_EQ(table.ToMountPath(tree.ProjectContent()), std::optional<FString>("/Project"));
	EXPECT_EQ(table.ToMountPath(tree.EngineContent() / "Fonts" / "a.ttf"), std::optional<FString>("/Engine/Fonts/a.ttf"));

	// The root prefix in another case still matches on a case-insensitive volume; the rest keeps its own.
	const FString upperRoot = FString::FromPath(tree.ProjectContent()).ToUpper();
	const std::filesystem::path upper = upperRoot.ToPath() / FStringView("Текстури/камінь.PNG").ToPath();
	EXPECT_EQ(table.ToMountPath(upper), std::optional<FString>("/Project/Текстури/камінь.PNG"));

	// Lexically normalized first.
	EXPECT_EQ(table.ToMountPath(tree.ProjectContent() / "A" / ".." / "B" / "." / "c.png"), std::optional<FString>("/Project/B/c.png"));

	// Outside every mount, or beside one.
	EXPECT_FALSE(table.ToMountPath(tree.ProjectDir() / "Config" / "Assets.yml").has_value());
	EXPECT_FALSE(table.ToMountPath(tree.ProjectDir() / "ContentX" / "a.png").has_value());
	EXPECT_FALSE(table.ToMountPath(tree.Root).has_value());

	EXPECT_EQ(table.FindContaining(file), table.Find("Project"));
	EXPECT_EQ(table.FindContaining(tree.Root), nullptr);
}

TEST(MountsTest, FromMountPathRejectsEverythingThatCouldLeaveTheMount)
{
	CDiscoveryTree tree("From mount path");
	MakeStandardLayout(tree);
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), nullptr);

	const std::optional<std::filesystem::path> file = table.FromMountPath("/Project/Текстури/Камінь 漢字.png");
	ASSERT_TRUE(file.has_value());
	EXPECT_EQ(*file, platform::NormalizePath(tree.ProjectContent() / FStringView("Текстури/Камінь 漢字.png").ToPath()));
	EXPECT_EQ(table.FromMountPath("/Engine"), std::optional(platform::NormalizePath(tree.EngineContent())));
	EXPECT_EQ(table.FromMountPath("/project/a.png"), std::optional(platform::NormalizePath(tree.ProjectContent() / "a.png")));

	// Round trip.
	EXPECT_EQ(table.ToMountPath(*file), std::optional<FString>("/Project/Текстури/Камінь 漢字.png"));

	for (const FStringView bad : { FStringView(""), FStringView("/"), FStringView("Project/a.png"), FStringView("/Project/"),
		FStringView("/Project//a.png"), FStringView("/Project/./a.png"), FStringView("/Project/../Engine/a.png"),
		FStringView("/Project/a/.."), FStringView("/Project/..\\..\\x"), FStringView("/Project/C:x"), FStringView("/Project/a\\b"),
		FStringView("/Plugin.None/a.png"), FStringView("/Project/\xED\xA0\x80\xED\xB0\x80") })
	{
		EXPECT_FALSE(table.FromMountPath(bad).has_value()) << FString::FromUtf8Lossy(bad).CStr();
	}
}

// A case-sensitive content directory matches its root prefix byte-exact.
TEST(MountsTest, ACaseSensitiveMountMatchesItsRootByteExact)
{
	CDiscoveryTree tree("Case-sensitive mount");
	MakeStandardLayout(tree);
	if (!test::discovery::MakeCaseSensitive(tree.ProjectContent()))
	{
		GTEST_SKIP() << "cannot make a directory case-sensitive here";
	}
	const CMountTable table = CMountTable::Resolve(MakeRoots(tree), nullptr);
	const SMount& project = *table.Find("Project");
	EXPECT_TRUE(project.Volume.bCaseSensitive);
	EXPECT_FALSE(table.GetMounts()[0].Volume.bCaseSensitive);

	EXPECT_EQ(table.ToMountPath(tree.ProjectContent() / "Rock.png"), std::optional<FString>("/Project/Rock.png"));
	const FString text = FString::FromPath(tree.ProjectContent());
	const FString otherCase = FString(text.View().LeftChop(7u)) + "CONTENT";
	EXPECT_FALSE(table.ToMountPath(otherCase.ToPath() / "Rock.png").has_value());
}

// /Engine's owner root, when only its two directories are given: GetEngineDir for this engine's own
// directories, else their deepest common directory.
TEST(MountsTest, TheEngineOwnerRootFollowsTheEngineDirectories)
{
	CDiscoveryTree tree("Engine owner root");
	MakeStandardLayout(tree);
	const CMountTable generated = CMountTable::Resolve(tree.EngineContent(), tree.EngineConfig(), tree.ProjectDir(), nullptr);
	EXPECT_EQ(generated.GetMounts()[0].OwnerRoot, platform::NormalizePath(tree.EngineDir()));
	EXPECT_EQ(generated.GetMounts().size(), 2u);

	const std::filesystem::path engineDir = paths::GetEngineDir();
	const CMountTable checkout = CMountTable::Resolve(paths::GetEngineContentDir(), paths::GetEngineConfigPath().parent_path(),
		std::filesystem::path(), nullptr);
	EXPECT_EQ(checkout.GetMounts()[0].OwnerRoot, platform::NormalizePath(engineDir));
	EXPECT_EQ(checkout.GetMounts()[0].RulesFile, platform::NormalizePath(paths::GetEngineConfigPath().parent_path() / "Assets.yml"));
}

TEST(MountsTest, PathsWrappersUseTheInstalledTable)
{
	CDiscoveryTree tree("Installed table");
	MakeStandardLayout(tree);
	const std::filesystem::path file = tree.ProjectContent() / FStringView("Ґ/ї.png").ToPath();

	paths::SetMountTable(nullptr);
	EXPECT_FALSE(paths::ToMountPath(file).has_value());
	EXPECT_FALSE(paths::FromMountPath("/Project/Ґ/ї.png").has_value());

	paths::SetMountTable(std::make_shared<const CMountTable>(CMountTable::Resolve(MakeRoots(tree), nullptr)));
	EXPECT_NE(paths::GetMountTable(), nullptr);
	EXPECT_EQ(paths::ToMountPath(file), std::optional<FString>("/Project/Ґ/ї.png"));
	EXPECT_EQ(paths::FromMountPath("/Project/Ґ/ї.png"), std::optional(platform::NormalizePath(file)));

	paths::SetMountTable(nullptr);
	EXPECT_EQ(paths::GetMountTable(), nullptr);
	EXPECT_FALSE(paths::ToMountPath(file).has_value());
}
