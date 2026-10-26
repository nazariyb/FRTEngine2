#pragma once

// Helpers of the rules-engine tests: a per-test tree with the engine, project and plugin layout (the discovery
// tests' CDiscoveryTree), writers for the rule files and content, and small queries over diagnostics.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include "../Discovery/DiscoveryTestUtils.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Pipeline/Rules.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Platform/FileSystem.h"


namespace frt::assets
{
// The tests print whole verdicts (ScanTypes.h compares them).
inline void PrintTo (const SFileVerdict& Verdict, std::ostream* Out)
{
	*Out << "{ " << enum_::ToString(Verdict.Class).GetData() << (Verdict.bQuiet ? ", quiet }" : " }");
}
}


namespace frt::test::rules
{
inline void MakeDirectories (const std::filesystem::path& Path)
{
	std::error_code ec;
	std::filesystem::create_directories(Path, ec);
}

// Writes Text to Path, creating its folders.
inline void WriteFile (const std::filesystem::path& Path, FStringView Text)
{
	MakeDirectories(Path.parent_path());
	ASSERT_TRUE(platform::WriteFileBytes(Path, Text.AsBytes())) << FString::FromPath(Path).CStr();
}

/**
 * <temp>/.../<Name> with Engine/{Content,Config} and Проєкт 项目/{Content,Config}; plugins on demand. Rule files and
 * content are written with the members below; Resolve gives the mount table of the tree as it is then.
 */
class CRulesTree
{
public:
	explicit CRulesTree (FStringView Name)
		: Tree(Name)
	{
		MakeDirectories(Tree.EngineContent());
		MakeDirectories(Tree.EngineConfig());
		MakeDirectories(Tree.ProjectContent());
		MakeDirectories(Tree.ProjectDir() / "Config");
	}

	[[nodiscard]] std::filesystem::path EngineRules () const { return Tree.EngineConfig() / "Assets.yml"; }
	[[nodiscard]] std::filesystem::path ProjectRules () const { return Tree.ProjectDir() / "Config" / "Assets.yml"; }
	[[nodiscard]] std::filesystem::path PluginDir (FStringView Plugin) const { return Tree.ProjectDir() / "Plugins" / Plugin.ToPath(); }

	void WriteEngineRules (FStringView Text) const { WriteFile(EngineRules(), Text); }
	void WriteProjectRules (FStringView Text) const { WriteFile(ProjectRules(), Text); }
	void WritePluginRules (FStringView Plugin, FStringView Text) const { WriteFile(PluginDir(Plugin) / "Config" / "Assets.yml", Text); }
	// Relative: below the mount's content directory, '/' separated.
	void WriteEngineContent (FStringView Relative, FStringView Text = "x") const { WriteFile(Tree.EngineContent() / Relative.ToPath(), Text); }
	void WriteProjectContent (FStringView Relative, FStringView Text = "x") const { WriteFile(Tree.ProjectContent() / Relative.ToPath(), Text); }
	void WritePluginContent (FStringView Plugin, FStringView Relative, FStringView Text = "x") const
	{
		WriteFile(PluginDir(Plugin) / "Content" / Relative.ToPath(), Text);
	}

	[[nodiscard]] assets::CMountTable Resolve () const
	{
		assets::SMountRoots roots;
		roots.EngineDir = Tree.EngineDir();
		roots.EngineContentDir = Tree.EngineContent();
		roots.EngineConfigDir = Tree.EngineConfig();
		roots.ProjectDir = Tree.ProjectDir();
		return assets::CMountTable::Resolve(roots, nullptr);
	}

	discovery::CDiscoveryTree Tree;
};

inline uint32 CountCode (std::span<const SDiagnostic> Diagnostics, uint32 Code)
{
	return static_cast<uint32>(std::count_if(Diagnostics.begin(), Diagnostics.end(), [Code] (const SDiagnostic& Diagnostic) { return Diagnostic.Code == Code; }));
}

// "file:line: message" lines, for failure messages.
inline FString DescribeDiagnostics (std::span<const SDiagnostic> Diagnostics)
{
	FString text;
	for (const SDiagnostic& diagnostic : Diagnostics)
	{
		text += FString::Format("\n  [{}] {} {}:{}:{}: {}", FormatDiagnosticCode(diagnostic.Code), diagnostic.Subject, diagnostic.File, diagnostic.Line,
			diagnostic.Column, diagnostic.Message);
	}
	return text;
}

// The value and provenance of one effective field of a resolved file.
inline const assets::SProvenance* FindProvenance (const assets::SEffectiveFileSettings& Settings, FStringView Field)
{
	for (const auto& [name, source] : Settings.Provenance)
	{
		if (name == Field)
		{
			return &source;
		}
	}
	return nullptr;
}

inline FStringView FindSetting (const assets::SEffectiveFileSettings& Settings, FStringView Field)
{
	if (Settings.Settings.Table == nullptr)
	{
		return FStringView();
	}
	for (const assets::SSettingValue& value : Settings.Settings.Values)
	{
		if (Settings.Settings.Table->Fields[value.Field].Name == Field)
		{
			return value.Text;
		}
	}
	return FStringView();
}
}
