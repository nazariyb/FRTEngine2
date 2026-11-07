#pragma once

// Helpers of the asset writer and command tests: a scratch engine and project (non-ASCII, a handful of files) with their
// mount table, a recording provider per mount, a writer that holds the lock, and file helpers by mount path.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "Assets/Discovery/DiscoveryTestUtils.h"
#include "Assets/Vcs/RecordingVcsProvider.h"

#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/Meta/OpJournal.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"


namespace frt::test::meta
{
inline constexpr FStringLiteral PluginName = "Розширення";

// The mount table of a scratch engine and project as CDiscoveryTree lays them out.
inline assets::CMountTable ResolveMounts (const std::filesystem::path& EngineDir, const std::filesystem::path& EngineContent,
	const std::filesystem::path& EngineConfig, const std::filesystem::path& ProjectDir)
{
	assets::SMountRoots roots;
	roots.EngineDir = EngineDir;
	roots.EngineContentDir = EngineContent;
	roots.EngineConfigDir = EngineConfig;
	roots.ProjectDir = ProjectDir;
	return assets::CMountTable::Resolve(roots, nullptr);
}

// A minimal canonical meta.
inline FString MakeMetaText (const assets::SGuid& Guid,std::vector<FString> FormerPaths = {})
{
	assets::SAssetMeta meta;
	meta.Guid = Guid;
	meta.FormerPaths = std::move(FormerPaths);
	FString text;
	EXPECT_TRUE(assets::EmitMeta(meta, text));
	return text;
}

class CWriterTree
{
public:
	explicit CWriterTree (FStringView Name, bool bWithPlugin = false)
		: Tree(Name)
	{
		std::error_code ec;
		std::filesystem::create_directories(Tree.EngineContent(), ec);
		std::filesystem::create_directories(Tree.EngineConfig(), ec);
		std::filesystem::create_directories(Tree.ProjectContent(), ec);
		if (bWithPlugin)
		{
			std::filesystem::create_directories(Tree.ProjectDir() / "Plugins" / FStringView(PluginName).ToPath() / "Content", ec);
		}
		Mounts = ResolveMounts(Tree.EngineDir(), Tree.EngineContent(), Tree.EngineConfig(), Tree.ProjectDir());
		Writer = MakeWriter();
	}

	[[nodiscard]] std::unique_ptr<assets::CAssetWriter> MakeWriter (bool bHoldsLock = true, assets::IVcsProvider* ProjectVcs = nullptr,
		assets::IVcsProvider* PluginVcs = nullptr)
	{
		assets::SAssetWriterDesc desc;
		desc.Mounts = &Mounts;
		desc.OwnerRoot = Tree.ProjectDir();
		desc.bHoldsLock = bHoldsLock;
		desc.Providers.emplace_back(assets::ProjectMountId, ProjectVcs != nullptr ? ProjectVcs : &Vcs);
		for (const assets::SMount& mount : Mounts.GetMounts())
		{
			if (mount.Kind == assets::EMountKind::Plugin)
			{
				desc.Providers.emplace_back(mount.Id, PluginVcs != nullptr ? PluginVcs : &Vcs);
			}
		}
		return std::make_unique<assets::CAssetWriter>(std::move(desc));
	}

	// A writer whose providers are all None (null).
	[[nodiscard]] std::unique_ptr<assets::CAssetWriter> MakeNoneWriter ()
	{
		assets::SAssetWriterDesc desc;
		desc.Mounts = &Mounts;
		desc.OwnerRoot = Tree.ProjectDir();
		desc.bHoldsLock = true;
		return std::make_unique<assets::CAssetWriter>(std::move(desc));
	}

	[[nodiscard]] std::filesystem::path Abs (FStringView MountPath) const
	{
		const std::optional<std::filesystem::path> path = Mounts.FromMountPath(MountPath);
		EXPECT_TRUE(path.has_value()) << FString(MountPath).CStr();
		return path.value_or(std::filesystem::path());
	}

	// What the recording provider is given for MountPath.
	[[nodiscard]] FString VcsPath (FStringView MountPath) const
	{
		return FString::FromPath(Abs(MountPath));
	}

	void Write (FStringView MountPath, FStringView Text) const
	{
		ASSERT_TRUE(platform::WriteFileText(Abs(MountPath), Text)) << FString(MountPath).CStr();
	}

	[[nodiscard]] bool Exists (FStringView MountPath) const
	{
		const std::optional<platform::SFileStat> stat = platform::Stat(Abs(MountPath));
		return stat.has_value() && !stat->bDirectory;
	}

	// Exists in exactly this spelling in its folder's listing.
	[[nodiscard]] bool IsSpelled (FStringView MountPath) const
	{
		const std::filesystem::path path = Abs(MountPath);
		const FString leaf = FString::FromPath(path.filename());
		bool bFound = false;
		platform::EnumerateDirectory(path.parent_path(), [&] (const platform::SDirectoryEntry& Entry)
		{
			bFound = bFound || Entry.Name == leaf.View();
		});
		return bFound;
	}

	[[nodiscard]] FString Read (FStringView MountPath) const
	{
		const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Abs(MountPath));
		return bytes.has_value() ? FString(FStringView(reinterpret_cast<const char*>(bytes->data()), bytes->size())) : FString();
	}

	[[nodiscard]] std::optional<assets::SAssetMeta> ReadMeta (FStringView MountPath) const
	{
		if (!Exists(MountPath))
		{
			return std::nullopt;
		}
		assets::SMetaParseResult parsed;
		assets::ParseMeta(Read(MountPath), parsed);
		if (parsed.State != assets::EMetaParseState::Ok)
		{
			return std::nullopt;
		}
		return std::move(parsed.Meta);
	}

	[[nodiscard]] bool JournalExists () const
	{
		std::error_code ec;
		return std::filesystem::exists(Writer->GetJournalPath(), ec);
	}

	discovery::CDiscoveryTree Tree;
	assets::CMountTable Mounts;
	CRecordingVcsProvider Vcs;
	std::unique_ptr<assets::CAssetWriter> Writer;
};
}
