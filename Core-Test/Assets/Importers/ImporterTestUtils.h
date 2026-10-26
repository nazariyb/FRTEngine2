#pragma once

// Helpers of the importers' Analyze tests (E3): a per-test tree with the engine and project layout (the discovery
// tests' CDiscoveryTree), writers for content files, Analyze of one file the way the registry service runs it
// (SAnalyzeInput::FromMount, the importer the file's extension claims, AnalyzeFile), and small queries over a
// result. The Demo template's own content is read in place, never written.

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

#include "../Discovery/DiscoveryTestUtils.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/Importers/AssetImporter.h"
#include "Assets/Pipeline/RuleTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "EnginePaths.h"
#include "Platform/FileSystem.h"


namespace frt::test::importers
{
using namespace frt::assets;

inline void WriteBytes (const std::filesystem::path& Path, std::span<const uint8> Bytes)
{
	std::error_code ec;
	std::filesystem::create_directories(Path.parent_path(), ec);
	ASSERT_TRUE(platform::WriteFileBytes(Path, Bytes)) << FString::FromPath(Path).CStr();
}

inline void WriteText (const std::filesystem::path& Path, FStringView Text)
{
	WriteBytes(Path, Text.AsBytes());
}

// The settings the rules engine gives a file its importer claims with no rule file: that importer, defaults.
inline SEffectiveFileSettings MakeSettings (const SImporterDesc* Importer)
{
	SEffectiveFileSettings settings;
	settings.Importer = Importer;
	settings.Class = Importer == nullptr ? EFileClass::Unclaimed : Importer == FindImporter("Raw") ? EFileClass::Raw : EFileClass::Asset;
	if (Importer != nullptr && Importer->Settings != nullptr)
	{
		settings.Settings.Table = Importer->Settings;
		for (uint32 i = 0u; i < Importer->Settings->Fields.size(); ++i)
		{
			settings.Settings.Values.push_back(SSettingValue { static_cast<uint16>(i), FString(Importer->Settings->Fields[i].Default), {} });
		}
	}
	return settings;
}

// Analyze of the file at MountPath in Mounts, by Importer (default: the one its extension claims).
inline SAnalyzeResult AnalyzeAt (const CMountTable& Mounts, FStringView MountPath, const SImporterDesc* Importer = nullptr)
{
	SAnalyzeResult result;
	FStringView mountName;
	FStringView relative;
	const SMount* mount = SplitMountPath(MountPath, mountName, relative) ? Mounts.Find(mountName) : nullptr;
	EXPECT_NE(mount, nullptr) << MountPath.ToStdString();
	if (mount == nullptr)
	{
		return result;
	}
	const std::optional<SAnalyzeInput> input = SAnalyzeInput::FromMount(*mount, MountPath);
	EXPECT_TRUE(input.has_value()) << MountPath.ToStdString();
	if (!input)
	{
		return result;
	}
	(void)AnalyzeFile(*input, MakeSettings(Importer != nullptr ? Importer : FindImporterForFile(GetMountPathLeaf(MountPath))), result);
	return result;
}

/**
 * <temp>/.../<Name> with Engine/{Content,Config} and Проєкт 项目/Content; files written below the project's
 * content (mount path /Project/<relative>) and analyzed there.
 */
class CImporterTree
{
public:
	explicit CImporterTree (FStringView Name)
		: Tree(Name)
	{
		std::error_code ec;
		std::filesystem::create_directories(Tree.EngineContent(), ec);
		std::filesystem::create_directories(Tree.EngineConfig(), ec);
		std::filesystem::create_directories(Tree.ProjectContent(), ec);
	}

	[[nodiscard]] std::filesystem::path File (FStringView Relative) const { return Tree.ProjectContent() / Relative.ToPath(); }
	[[nodiscard]] static FString MountPath (FStringView Relative) { return JoinMountPath("Project", Relative); }

	void Write (FStringView Relative, FStringView Text) const { WriteText(File(Relative), Text); }
	void WriteBinary (FStringView Relative, std::span<const uint8> Bytes) const { WriteBytes(File(Relative), Bytes); }

	[[nodiscard]] CMountTable Resolve () const
	{
		return CMountTable::Resolve(Tree.EngineContent(), Tree.EngineConfig(), Tree.ProjectDir(), nullptr);
	}

	// Analyze of /Project/<Relative>.
	[[nodiscard]] SAnalyzeResult Analyze (FStringView Relative, const SImporterDesc* Importer = nullptr) const
	{
		return AnalyzeAt(Resolve(), MountPath(Relative), Importer);
	}

	discovery::CDiscoveryTree Tree;
};

// The mounts of this engine with the Demo template as the project: /Project is Templates/Demo/Content.
inline CMountTable ResolveDemoMounts ()
{
	return CMountTable::Resolve(paths::GetEngineContentDir(), paths::GetEngineConfigPath().parent_path(),
		paths::GetEngineDir() / "Templates" / "Demo", nullptr);
}


// ---- Queries ----

inline const STagDecl* FindTag (std::span<const STagDecl> Tags, FStringView Key)
{
	for (const STagDecl& tag : Tags)
	{
		if (tag.Key == Key)
		{
			return &tag;
		}
	}
	return nullptr;
}

inline std::optional<int64> GetIntTag (std::span<const STagDecl> Tags, FStringView Key)
{
	const STagDecl* tag = FindTag(Tags, Key);
	if (tag == nullptr || tag->Value.Type != ETagType::Int64)
	{
		return std::nullopt;
	}
	return tag->Value.AsInt64();
}

inline const SSubEntryDecl* FindSubEntry (const SAnalyzeResult& Result, FStringView Label)
{
	for (const SSubEntryDecl& sub : Result.SubEntries)
	{
		if (sub.Label == Label)
		{
			return &sub;
		}
	}
	return nullptr;
}

inline const SEdgeDecl* FindEdgeTo (const SAnalyzeResult& Result, FStringView TargetPath)
{
	for (const SEdgeDecl& edge : Result.Edges)
	{
		if (edge.TargetPath == TargetPath)
		{
			return &edge;
		}
	}
	return nullptr;
}

inline uint32 CountDiagnostics (const SAnalyzeResult& Result, ESeverity Severity, uint32 Code = ~0u)
{
	uint32 count = 0u;
	for (const SDiagnostic& diagnostic : Result.Diagnostics)
	{
		count += diagnostic.Severity == Severity && (Code == ~0u || diagnostic.Code == Code) ? 1u : 0u;
	}
	return count;
}

// The first diagnostic whose message contains Text.
inline const SDiagnostic* FindDiagnostic (const SAnalyzeResult& Result, FStringView Text)
{
	for (const SDiagnostic& diagnostic : Result.Diagnostics)
	{
		if (diagnostic.Message.View().Contains(Text))
		{
			return &diagnostic;
		}
	}
	return nullptr;
}

inline FString DescribeDiagnostics (const SAnalyzeResult& Result)
{
	FString text;
	for (const SDiagnostic& diagnostic : Result.Diagnostics)
	{
		text.AppendFormat("[{} {}] {} ({}:{}:{}) {}\n", GetSeverityName(diagnostic.Severity), FormatDiagnosticCode(diagnostic.Code), diagnostic.Subject,
			diagnostic.File, diagnostic.Line, diagnostic.Column, diagnostic.Message);
	}
	return text;
}
}
