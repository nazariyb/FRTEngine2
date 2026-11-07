// The scripted-run rules (Engine/Automation.h): every scripted run is read-only and never writes /Engine, except the
// editor smoke test's file edits on a scratch copy, which CAutomation::IsScratchEditRun recognises with the same
// decision the smoke test's BeginFileEdits makes - SmokeTest and SmokeTestEdits on, a project and a [Render] ShaderRoot
// both outside the engine checkout as the disk resolves them. One argument or one config line short of that, the run
// stays read-only.
#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>

#include "Assets/Discovery/DiscoveryTestUtils.h"
#include "Config/Config.h"
#include "Containers/String.h"
#include "Engine/Automation.h"
#include "Platform/FileSystem.h"
#include "Platform/Process.h"
#include "Platform/Platform.h"

using namespace frt;


namespace
{
class AutomationRulesTest : public testing::Test
{
protected:
	void SetUp () override
	{
		Root = std::filesystem::temp_directory_path()
			/ FString::Format("FRT Автоматизація {}", platform::GetCurrentProcessId()).ToPath();
		test::discovery::RemoveTree(Root);
		std::error_code ec;
		for (const std::filesystem::path& dir : { Engine() / "Core" / "Content" / "Shaders", Engine() / "Templates" / "Demo",
			Outside() / "Project", Outside() / "Shaders" })
		{
			std::filesystem::create_directories(dir, ec);
			ASSERT_TRUE(std::filesystem::is_directory(dir)) << FString::FromPath(dir).CStr();
		}
	}

	void TearDown () override
	{
		test::discovery::RemoveTree(Root);
	}

	std::filesystem::path Engine () const { return Root / FStringView("Рушій").ToPath(); }
	std::filesystem::path Outside () const { return Root / FStringView("Копія 副本").ToPath(); }

	// The smoke test's scratch command line (Verify.py's smoke_scratch); ShaderRoot empty: none.
	config::CConfig ScratchConfig (const std::filesystem::path& ShaderRoot) const
	{
		config::CConfig config;
		config.SetOverride("Editor", "SmokeTest", "1");
		config.SetOverride("Editor", "SmokeTestEdits", "1");
		if (!ShaderRoot.empty())
		{
			config.SetOverride("Render", "ShaderRoot", FString::FromPath(ShaderRoot));
		}
		return config;
	}

	// ApplyScriptedRunRules on Config for a project at ProjectDir: [Assets] ReadOnly after it.
	static bool IsReadOnlyAfterRules (config::CConfig& Config, const std::filesystem::path& ProjectDir, const std::filesystem::path& EngineDir)
	{
		CAutomation::ApplyScriptedRunRules(Config, ProjectDir, EngineDir);
		EXPECT_FALSE(Config.Get("Assets", "Watch", true)) << "no scripted run reloads files behind its back";
		EXPECT_FALSE(Config.Get("Assets", "EngineWritable", true)) << "no scripted run writes /Engine";
		return Config.Get("Assets", "ReadOnly", false);
	}

	std::filesystem::path Root;
};
}

// The scratch copy's run is its writer: everything the smoke test needs to edit files lies outside the engine.
TEST_F(AutomationRulesTest, TheScratchCopysSmokeRunWithFileEditsIsTheCopysWriter)
{
	config::CConfig config = ScratchConfig(Outside() / "Shaders");
	EXPECT_TRUE(CAutomation::IsScratchEditRun(config, Outside() / "Project", Engine()));
	EXPECT_FALSE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
}

// One condition short, the run stays read-only, as the smoke test then refuses to edit anything: what makes a writer is the
// decision the smoke test makes, not SmokeTestEdits alone.
TEST_F(AutomationRulesTest, AScriptedRunOneConditionShortOfTheScratchEditsStaysReadOnly)
{
	{
		SCOPED_TRACE("no [Render] ShaderRoot: the engine's own shaders");
		config::CConfig config = ScratchConfig({});
		EXPECT_TRUE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
	}
	{
		SCOPED_TRACE("a ShaderRoot inside the engine checkout");
		config::CConfig config = ScratchConfig(Engine() / "Core" / "Content" / "Shaders");
		EXPECT_TRUE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
	}
	{
		SCOPED_TRACE("a ShaderRoot that is no directory");
		config::CConfig config = ScratchConfig(Outside() / "Missing");
		EXPECT_TRUE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
	}
	{
		SCOPED_TRACE("SmokeTestEdits without SmokeTest (a capture run with a stray config line)");
		config::CConfig config = ScratchConfig(Outside() / "Shaders");
		config.SetOverride("Editor", "SmokeTest", "0");
		config.SetOverride("Automation", "ExitFrame", "10");
		EXPECT_TRUE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
	}
	{
		SCOPED_TRACE("no SmokeTestEdits");
		config::CConfig config = ScratchConfig(Outside() / "Shaders");
		config.SetOverride("Editor", "SmokeTestEdits", "0");
		EXPECT_TRUE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
	}
	{
		SCOPED_TRACE("a project inside the engine checkout (a template, in place)");
		config::CConfig config = ScratchConfig(Outside() / "Shaders");
		EXPECT_TRUE(IsReadOnlyAfterRules(config, Engine() / "Templates" / "Demo", Engine()));
	}
	{
		SCOPED_TRACE("no project");
		config::CConfig config = ScratchConfig(Outside() / "Shaders");
		EXPECT_TRUE(IsReadOnlyAfterRules(config, {}, Engine()));
	}
	{
		SCOPED_TRACE("-Assets:EngineWritable=1 on the scratch command line: the copy's writer, never /Engine's");
		config::CConfig config = ScratchConfig(Outside() / "Shaders");
		config.SetOverride("Assets", "EngineWritable", "1");
		EXPECT_FALSE(IsReadOnlyAfterRules(config, Outside() / "Project", Engine()));
	}
}

// Inside is decided as the disk resolves the paths: a junction outside the engine that leads into it is inside.
TEST_F(AutomationRulesTest, AProjectReachedThroughAJunctionIntoTheEngineIsInsideIt)
{
	const std::filesystem::path link = Outside() / FStringView("Посилання").ToPath();
	if (!test::discovery::MakeJunction(link, Engine() / "Templates" / "Demo"))
	{
		GTEST_SKIP() << "this machine refuses a junction in the temp directory";
	}
	EXPECT_TRUE(platform::IsAtOrBelowResolved(link, Engine()));
	EXPECT_FALSE(platform::IsAtOrBelow(link, Engine())) << "lexically it is outside";
	EXPECT_TRUE(platform::IsAtOrBelowResolved(Engine() / "Templates" / ".." / "Templates" / "Demo" / "Missing", Engine()))
		<< "a path that does not exist (yet) is resolved as far as it does";
	EXPECT_FALSE(platform::IsAtOrBelowResolved(Outside() / "Project", Engine()));

	config::CConfig config = ScratchConfig(Outside() / "Shaders");
	EXPECT_FALSE(CAutomation::IsScratchEditRun(config, link, Engine()));
	EXPECT_TRUE(IsReadOnlyAfterRules(config, link, Engine()));
}
