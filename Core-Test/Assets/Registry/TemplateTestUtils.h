#pragma once

// Copies of the engine's content and a committed template in a temp tree of their own, for the registry tests that
// scan real content (the Demo pipeline, the registry service, its goldens and measurements): the runtime may write
// Content/, so no test scans the checkout itself. The tree has the layout CMountTable::Resolve expects (the discovery
// tests' CDiscoveryTree): Engine/{Content,Config} and Проєкт 项目/{Content,Config}.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

#include "../Discovery/DiscoveryTestUtils.h"

#include "Assets/Discovery/Mounts.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "EnginePaths.h"


namespace frt::test::registry
{
// Copies Source into Destination, every file, skipping the directories named in Skip (relative to Source).
inline void CopyTree (const std::filesystem::path& Source, const std::filesystem::path& Destination, std::span<const FStringView> Skip = {})
{
	std::error_code ec;
	std::filesystem::create_directories(Destination, ec);
	for (std::filesystem::recursive_directory_iterator it(Source, ec), end; it != end; it.increment(ec))
	{
		const std::filesystem::path relative = std::filesystem::relative(it->path(), Source, ec);
		const FString relativeText = FString::FromPath(relative, EPathStyle::Generic);
		if (it->is_directory(ec))
		{
			if (std::any_of(Skip.begin(), Skip.end(), [&] (FStringView S) { return relativeText.View() == S; }))
			{
				it.disable_recursion_pending();
				continue;
			}
			std::filesystem::create_directories(Destination / relative, ec);
			continue;
		}
		std::filesystem::copy_file(it->path(), Destination / relative, std::filesystem::copy_options::overwrite_existing, ec);
		const FString copied = FString::FromPath(it->path());
		ASSERT_FALSE(ec) << copied.CStr();
	}
}

/**
 * The engine's content (without the shader binaries), Core/Config/Assets.yml and one template's Content/ and
 * Config/Assets.yml (Templates/<Template>), mounted as F3 mounts them.
 */
class CTemplateCopy
{
public:
	explicit CTemplateCopy (FStringView Name, FStringView Template = "Demo")
		: Tree(Name)
	{
		const FStringView skipShaderBinaries[] = { "Shaders/Bin" };
		CopyTree(paths::GetEngineContentDir(), Tree.EngineContent(), skipShaderBinaries);
		std::error_code ec;
		std::filesystem::create_directories(Tree.EngineConfig(), ec);
		std::filesystem::copy_file(paths::GetEngineConfigPath().parent_path() / "Assets.yml", Tree.EngineConfig() / "Assets.yml", ec);
		const std::filesystem::path source = paths::GetEngineDir() / "Templates" / Template.ToPath();
		CopyTree(source / "Content", Tree.ProjectContent());
		std::filesystem::create_directories(Tree.ProjectDir() / "Config", ec);
		std::filesystem::copy_file(source / "Config" / "Assets.yml", Tree.ProjectDir() / "Config" / "Assets.yml", ec);
		Mounts = assets::CMountTable::Resolve(Tree.EngineContent(), Tree.EngineConfig(), Tree.ProjectDir(), &MountDiagnostics);
	}

	// The engine's mounts alone (an engine-owner run).
	[[nodiscard]] assets::CMountTable ResolveEngineOnly () const
	{
		return assets::CMountTable::Resolve(Tree.EngineContent(), Tree.EngineConfig(), std::filesystem::path(), nullptr);
	}

	discovery::CDiscoveryTree Tree;
	assets::CMountTable Mounts;
	std::vector<SDiagnostic> MountDiagnostics;
};
}
