// The rules engine as the mount scanner's oracle (Assets/Pipeline/Rules.h over Discovery/MountScanner.h, design §5.4
// steps 3-4): excluded folders are not descended and their rule files never read, every folder's scope and RulesHash
// land in the batch, a malformed folder file is one RULE001 in the batch, per-file settings come from the folder's
// scope, and RULE002 is reported for what stays unclaimed. The committed engine and template rule files raise no
// RULE001 and, once glTF buffers count as companions, no RULE002. And §5.11's budget for a folder Assets.yml edit:
// re-evaluating a 1k-file interval in memory in at most 2 ms.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "RulesTestUtils.h"

#include "EnginePaths.h"
#include "Assets/Discovery/MountScanner.h"
#include "Jobs/WorkerPool.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"

using namespace frt;
using namespace frt::assets;
using frt::test::rules::CountCode;
using frt::test::rules::CRulesTree;
using frt::test::rules::DescribeDiagnostics;


namespace
{
std::unique_ptr<jobs::CWorkerPool> MakePool (uint32 Threads = 2u)
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = Threads;
	desc.Name = "Rules Test";
	return std::make_unique<jobs::CWorkerPool>(desc);
}

const SScanFile* FindFile (const SScanBatch& Batch, FStringView MountPath)
{
	for (const SScanFile& file : Batch.Files)
	{
		if (MakeChildMountPath(Batch.Folders[file.Folder].MountPath, file.Name) == MountPath)
		{
			return &file;
		}
	}
	return nullptr;
}

const SScanFolder* FindFolder (const SScanBatch& Batch, FStringView MountPath)
{
	for (const SScanFolder& folder : Batch.Folders)
	{
		if (folder.MountPath == MountPath)
		{
			return &folder;
		}
	}
	return nullptr;
}

/**
 * RULE002 for a batch as the registry service will compute it: the files still Unclaimed once companions are known.
 * Companions here are the files a glTF beside them names (its buffer URIs), as the glTF importer will report them.
 */
std::vector<SDiagnostic> ReportUnclaimed (const SScanBatch& Batch, const CMountTable& Mounts)
{
	std::vector<FString> paths;
	std::vector<bool> quiet;
	for (const SScanFile& file : Batch.Files)
	{
		if (file.Verdict.Class != EFileClass::Unclaimed)
		{
			continue;
		}
		const FStringView folder = Batch.Folders[file.Folder].MountPath;
		bool bCompanion = false;
		for (const SScanFile& other : Batch.Files)
		{
			if (other.Folder == file.Folder && other.Name.EndsWith<ESearchCase::IgnoreCase>(".gltf"))
			{
				const std::optional<std::filesystem::path> gltf = Mounts.FromMountPath(MakeChildMountPath(folder, other.Name));
				const std::optional<FString> text = gltf.has_value() ? platform::ReadFileText(*gltf) : std::nullopt;
				bCompanion = bCompanion || (text.has_value() && text->Contains(FString::Format("\"{}\"", file.Name)));
			}
		}
		if (!bCompanion)
		{
			paths.push_back(MakeChildMountPath(folder, file.Name));
			quiet.push_back(file.Verdict.bQuiet);
		}
	}
	std::vector<SUnclaimedFile> files;
	for (uint32 i = 0u; i < paths.size(); ++i)
	{
		files.push_back({ paths[i], quiet[i] });
	}
	return ReportUnclaimedFiles(files);
}
}


TEST(RulesScan, TheScannerFollowsTheRulesEngineFolderByFolder)
{
	CRulesTree tree("Scan");
	tree.WriteProjectRules("FormatVersion: 1\nScan:\n  Exclude: [ \"Attic/**\" ]\n  Quiet: [ \"*.md\" ]\n");
	tree.WriteProjectContent("Art/Rock.png");
	tree.WriteProjectContent("Art/notes.md");
	tree.WriteProjectContent("Art/Assets.yml", "FormatVersion: 1\nRules: [ { Match: \"*.png\", Cook: Never, Labels: [ Art ] } ]\n");
	tree.WriteProjectContent("Art/Broken/Assets.yml", "FormatVersion: 1\nRules: [ { Match: \"*.png\", Kook: Never } ]\n");
	tree.WriteProjectContent("Art/Broken/A.png");
	tree.WriteProjectContent("Attic/Assets.yml", "not: [ valid");
	tree.WriteProjectContent("Attic/Old.png");
	tree.WriteProjectContent("Data/blob.xyz");
	tree.WriteProjectContent("Data/other.XYZ");
	const CMountTable mounts = tree.Resolve();
	const CRulesEngine engine(mounts);
	const auto pool = MakePool();

	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *mounts.Find("Project"), "/Project", engine, nullptr);
	EXPECT_EQ(FindFolder(batch, "/Project/Attic"), nullptr) << "an excluded folder is not descended";
	ASSERT_EQ(CountCode(batch.Diagnostics, Rule001), 1u) << "the Attic's broken file is never read" << DescribeDiagnostics(batch.Diagnostics).CStr();
	const auto rule001 = std::find_if(batch.Diagnostics.begin(), batch.Diagnostics.end(), [] (const SDiagnostic& Diagnostic) { return Diagnostic.Code == Rule001; });
	EXPECT_EQ(rule001->Subject, "/Project/Art/Broken");
	EXPECT_EQ(rule001->File, "Content/Art/Broken/Assets.yml");
	EXPECT_EQ(rule001->Line, 2u);

	const SScanFolder* art = FindFolder(batch, "/Project/Art");
	ASSERT_NE(art, nullptr);
	EXPECT_TRUE(art->bHasRuleFile);
	EXPECT_NE(art->RulesHash, 0u);
	const SScanFolder* broken = FindFolder(batch, "/Project/Art/Broken");
	ASSERT_NE(broken, nullptr);
	EXPECT_TRUE(broken->bHasRuleFile);
	EXPECT_EQ(broken->RulesHash, 0u) << "a cold malformed file reports no hash";
	EXPECT_EQ(broken->Scope, art->Scope) << "and contributes nothing: the parent's scope";
	EXPECT_EQ(FindFolder(batch, "/Project")->RulesHash, 0u);

	const SScanFile* rock = FindFile(batch, "/Project/Art/Rock.png");
	ASSERT_NE(rock, nullptr);
	EXPECT_EQ(rock->Verdict, (SFileVerdict { EFileClass::Asset, false }));
	const SEffectiveFileSettings rockSettings = engine.ResolveFile(*batch.Folders[rock->Folder].Scope, "/Project/Art/Rock.png");
	EXPECT_EQ(rockSettings.CookRule, ECookRule::Never);
	EXPECT_EQ(rockSettings.RuleLabels, (std::vector<FString> { "Art" }));
	const SScanFile* brokenPng = FindFile(batch, "/Project/Art/Broken/A.png");
	ASSERT_NE(brokenPng, nullptr);
	EXPECT_EQ(engine.ResolveFile(*batch.Folders[brokenPng->Folder].Scope, "/Project/Art/Broken/A.png").CookRule, ECookRule::Never);
	EXPECT_EQ(FindFile(batch, "/Project/Art/notes.md")->Verdict, (SFileVerdict { EFileClass::Unclaimed, true }));
	EXPECT_EQ(FindFile(batch, "/Project/Data/blob.xyz")->Verdict, (SFileVerdict { EFileClass::Unclaimed, false }));

	const std::vector<SDiagnostic> unclaimed = ReportUnclaimed(batch, mounts);
	ASSERT_EQ(unclaimed.size(), 1u) << DescribeDiagnostics(unclaimed).CStr();
	EXPECT_EQ(unclaimed[0].Subject, "/Project/Data");
	EXPECT_EQ(unclaimed[0].Code, Rule002);

	// A rescan finds the same rules without parsing again, and a fix of the broken file lands.
	tree.WriteProjectContent("Art/Broken/Assets.yml", "FormatVersion: 1\nRules: [ { Match: \"*.png\", Cook: Always } ]\n");
	const SScanBatch fixed = CMountScanner::ScanBlocking(*pool, *mounts.Find("Project"), "/Project/Art", engine, nullptr);
	EXPECT_EQ(CountCode(fixed.Diagnostics, Rule001), 0u) << DescribeDiagnostics(fixed.Diagnostics).CStr();
	const SScanFolder* fixedBroken = FindFolder(fixed, "/Project/Art/Broken");
	ASSERT_NE(fixedBroken, nullptr);
	EXPECT_NE(fixedBroken->RulesHash, 0u);
	EXPECT_EQ(engine.ResolveFile(*fixedBroken->Scope, "/Project/Art/Broken/A.png").CookRule, ECookRule::Always);
	EXPECT_EQ(FindFolder(fixed, "/Project/Art")->RulesHash, art->RulesHash);
}

TEST(RulesScan, VerdictsScopesAndHashesDoNotDependOnTheThreadCount)
{
	CRulesTree tree("Threads");
	tree.WriteProjectRules("FormatVersion: 1\nScan: { Exclude: [ \"**/Skip/**\" ], Quiet: [ \"*.txt\" ] }\n"
		"Rules: [ { Match: \"**/*.png\", Labels: [ Png ] } ]\n");
	for (uint32 folder = 0u; folder < 12u; ++folder)
	{
		const FString base = FString::Format("Тека {}/Під {}", folder % 3u, folder);
		if (folder % 4u == 0u)
		{
			tree.WriteProjectContent(base + "/Assets.yml", FString::Format("FormatVersion: 1\nRules: [ {{ Match: \"*\", Chunk: {} }} ]\n", folder));
		}
		for (uint32 file = 0u; file < 8u; ++file)
		{
			constexpr FStringLiteral Extensions[] = { ".png", ".txt", ".gltf", ".xyz" };
			tree.WriteProjectContent(FString::Format("{}/Файл {}{}", base, file, Extensions[file % 4u]));
		}
		tree.WriteProjectContent(base + "/Skip/Hidden.png");
	}
	const CMountTable mounts = tree.Resolve();
	const SMount& project = *mounts.Find("Project");

	const auto describe = [&] (uint32 Threads)
	{
		const CRulesEngine engine(mounts);
		const auto pool = MakePool(Threads);
		const SScanBatch batch = CMountScanner::ScanBlocking(*pool, project, "/Project", engine, nullptr);
		FString text;
		for (const SScanFolder& folder : batch.Folders)
		{
			text += FString::Format("{} {:016x}\n", folder.MountPath, folder.RulesHash);
		}
		for (const SScanFile& file : batch.Files)
		{
			const FString path = MakeChildMountPath(batch.Folders[file.Folder].MountPath, file.Name);
			const SEffectiveFileSettings settings = engine.ResolveFile(*batch.Folders[file.Folder].Scope, path);
			text += FString::Format("{} {} {} {} {}\n", path, enum_::ToString(file.Verdict.Class), file.Verdict.bQuiet, settings.Chunk,
				settings.RuleLabels.size());
		}
		return text;
	};
	const FString one = describe(1u);
	EXPECT_FALSE(one.Contains("Skip"));
	EXPECT_TRUE(one.Contains(" Unclaimed true 0 0"));
	EXPECT_EQ(describe(4u), one);
}

TEST(RulesScan, TheCommittedEngineAndTemplateRulesRaiseNoRule001AndNoRule002)
{
	const std::filesystem::path engineDir = paths::GetEngineDir();
	const auto pool = MakePool();
	constexpr FStringLiteral Templates[] = { "Demo", "Blank" };
	constexpr FStringLiteral MountNames[] = { "Engine", "Project" };
	for (const FStringLiteral templateName : Templates)
	{
		const std::filesystem::path projectDir = engineDir / "Templates" / FStringView(templateName).ToPath();
		std::vector<SDiagnostic> diagnostics;
		const CMountTable mounts = CMountTable::Resolve(paths::GetEngineContentDir(), paths::GetEngineConfigPath().parent_path(), projectDir, &diagnostics);
		CRulesEngine engine(mounts);
		engine.LoadMountRules(&diagnostics);
		EXPECT_EQ(engine.GetMountRulesHash("Engine") != 0u, true) << "Core/Config/Assets.yml";
		EXPECT_EQ(engine.GetMountRulesHash("Project") != 0u, true) << templateName.GetData() << "/Config/Assets.yml";
		uint32 files = 0u;
		for (const FStringLiteral mountName : MountNames)
		{
			const SMount* mount = mounts.Find(mountName);
			ASSERT_NE(mount, nullptr);
			const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *mount, mount->GetRootMountPath(), engine, nullptr);
			diagnostics.insert(diagnostics.end(), batch.Diagnostics.begin(), batch.Diagnostics.end());
			const std::vector<SDiagnostic> unclaimed = ReportUnclaimed(batch, mounts);
			diagnostics.insert(diagnostics.end(), unclaimed.begin(), unclaimed.end());
			files += static_cast<uint32>(batch.Files.size());
			EXPECT_EQ(FindFolder(batch, "/Engine/Shaders"), nullptr) << "engine shaders are excluded";
		}
		EXPECT_GT(files, 6u) << templateName.GetData();
		EXPECT_EQ(CountCode(diagnostics, Rule001), 0u) << templateName.GetData() << DescribeDiagnostics(diagnostics).CStr();
		EXPECT_EQ(CountCode(diagnostics, Rule002), 0u) << templateName.GetData() << DescribeDiagnostics(diagnostics).CStr();
	}
}

TEST(RulesScan, AFolderRuleEditReevaluatesAThousandFilesWithinTheBudget)
{
	constexpr bool bOptimized = !FRT_BUILD_DEBUG;
	const bool bStrict = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1";
	const double slack = bStrict ? 1.0 : 2.0;

	CRulesTree tree("Budget");
	tree.WriteEngineRules("FormatVersion: 1\nScan:\n  Exclude: [ \"/Engine/Shaders/**\" ]\n  Quiet: [ \"**/README.md\" ]\nPresets:\n"
		"  Texture/Color:  { ColorSpace: Srgb,   Mips: Box, Compression: None }\n  Texture/Normal: { ColorSpace: Normal, Mips: Box, Compression: None }\n"
		"Rules:\n  - { Match: \"/Engine/Defaults/**\", Cook: Always }\n  - { Match: \"**/*.{png,jpg,jpeg,tga,bmp,psd,kra,ora}\", Preset: Texture/Color }\n");
	tree.WriteProjectRules("FormatVersion: 1\nScan:\n  Exclude: [ \"**/Reference/**\", \"Docs/**\" ]\n  Quiet: [ \"license.txt\", \"*.md\" ]\n"
		"  Raw: [ \"Data/Raw/**\" ]\nPresets:\n  Texture/Mask: { ColorSpace: Linear, Mips: Box, Compression: None }\nRules:\n"
		"  - { Match: \"Ui/**/*.{png,tga}\", For: Texture, Settings: { MaxSize: 1024, Mips: None }, Labels: [ Ui ] }\n"
		"  - { Match: [ \"*_n.*\", \"*_normal.*\" ], Preset: Texture/Normal }\n  - { Match: [ \"*_orm.*\", \"*_mr.*\" ], Preset: Texture/Mask }\n"
		"  - { Match: \"Debug/**\", Cook: DevelopmentOnly }\n  - { Match: \"Levels/Desert/**\", Chunk: 1 }\n");
	const CMountTable mounts = tree.Resolve();
	const CRulesEngine engine(mounts);
	std::vector<SDiagnostic> diagnostics;
	const std::shared_ptr<const SScanScope> project = engine.OpenScope("/Project", diagnostics);
	ASSERT_TRUE(diagnostics.empty()) << DescribeDiagnostics(diagnostics).CStr();

	// 1000 files in 20 folders below /Project/Environment, whose Assets.yml is edited.
	constexpr FStringLiteral Extensions[] = { ".png", "_n.png", "_orm.png", ".jpg", ".gltf", ".frtmat.yml", ".txt", ".md" };
	std::vector<FString> files;
	for (uint32 i = 0u; i < 1000u; ++i)
	{
		files.push_back(FString::Format("/Project/Environment/Тека {:02}/Файл_{}{}", i / 50u, i, Extensions[i % std::size(Extensions)]));
	}
	constexpr FStringLiteral texts[] = {
		"FormatVersion: 1\nScan: { Exclude: [ \"Scans/**\" ] }\nRules:\n  - { Match: \"*.{png,psd}\", For: Texture, Settings: { MaxSize: 1024 }, Labels: [ Foliage ] }\n"
		"  - { Match: \"Тека 0*/**\", Chunk: 1 }\n",
		"FormatVersion: 1\nScan: { Exclude: [ \"Scans/**\" ] }\nRules:\n  - { Match: \"*.{png,psd}\", For: Texture, Settings: { MaxSize: 2048 }, Labels: [ Foliage ] }\n"
		"  - { Match: \"Тека 1*/**\", Chunk: 2 }\n",
	};

	const auto reevaluate = [&] (FStringView Text, bool bProvenance, uint64& OutChecksum)
	{
		uint64 rulesHash = 0u;
		std::vector<SDiagnostic> bindDiagnostics;
		const std::shared_ptr<const SScanScope> scope = engine.EnterFolderFromText(project, "/Project/Environment", Text, rulesHash, bindDiagnostics);
		EXPECT_TRUE(bindDiagnostics.empty());
		uint64 checksum = rulesHash;
		for (const FString& file : files)
		{
			// Membership included: an excluded file comes back as Class Excluded.
			const SEffectiveFileSettings settings = engine.ResolveFile(*scope, file, {}, bProvenance);
			checksum = checksum * 31u + static_cast<uint64>(settings.Class) + settings.Chunk + settings.Settings.Values.size() + settings.RuleLabels.size()
				+ (settings.Settings.Values.empty() ? 0u : settings.Settings.Values.back().Text.Len());
		}
		OutChecksum = checksum;
	};

	double best = 1e9;
	double bestWithProvenance = 1e9;
	uint64 checksums[2] = {};
	for (uint32 attempt = 0u; attempt < 6u; ++attempt)
	{
		uint64 checksum = 0u;
		const uint64 start = platform::ReadTimestamp();
		reevaluate(texts[attempt % 2u], attempt >= 3u, checksum);
		const double ms = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		double& slot = attempt >= 3u ? bestWithProvenance : best;
		slot = (std::min)(slot, ms);
		if (attempt < 2u)
		{
			checksums[attempt] = checksum;
		}
		else if (attempt < 3u)
		{
			EXPECT_EQ(checksum, checksums[attempt % 2u]) << "re-evaluation is deterministic";
		}
	}
	EXPECT_NE(checksums[0], checksums[1]) << "the edit changed something";
	std::printf("[ budget ] folder Assets.yml edit over a %zu-file interval: %.3f ms (budget 2 ms); with provenance %.3f ms\n", files.size(), best,
		bestWithProvenance);
	RecordProperty("FolderEditMilliseconds", FString::Format("{:.3f}", best).CStr());
	RecordProperty("FolderEditWithProvenanceMilliseconds", FString::Format("{:.3f}", bestWithProvenance).CStr());
	if (bOptimized)
	{
		EXPECT_LE(best, 2.0 * slack) << (bStrict ? "the budget" : "twice the budget");
	}
}
