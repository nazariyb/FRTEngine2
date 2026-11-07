#pragma once

// The Schemas/Migrations.yml of a repository that is up to date with this engine (design §14.8): the scratch projects and
// engines of the tests that run AssetTool's writing verbs, which the MIG001 gate refuses in a root behind (edit [43]).

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <vector>

#include "Assets/Meta/Migrations.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"


namespace frt::test::meta
{
// Every step this engine has, as `migrate --all` records them: Root is not behind.
inline void WriteCurrentMigrations (const std::filesystem::path& Root)
{
	std::vector<FString> applied;
	for (const assets::SMigrationStep& step : assets::GetMigrationSteps())
	{
		if (step.bAvailable)
		{
			applied.push_back(FString(step.Name));
		}
	}
	FString text;
	ASSERT_TRUE(assets::EmitMigrations(applied, text));
	const std::span<const uint8> bytes(reinterpret_cast<const uint8*>(text.GetData()), text.Len());
	ASSERT_TRUE(platform::WriteFileBytes(assets::GetMigrationsPath(Root), bytes)) << FString::FromPath(Root).CStr();
}
}
