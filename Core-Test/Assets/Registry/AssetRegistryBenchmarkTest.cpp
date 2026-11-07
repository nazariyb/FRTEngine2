// §5.11's start-up rows, end to end through the registry service (Assets/AssetRegistry.h): the Demo template's warm
// start, a warm start of 20k files with the snapshot load, the same after a mount-rule edit (0 Analyze, 0 hashes; the
// row as revised at M3), a cold full scan with Analyze at 10k files on 16 threads, the owner-thread cost of one
// watcher-delivered edit (its commits and journal appends), and the longest frame of a watcher-delivered mount-rule
// edit. A warm start is a reader's whole start - snapshots read and adopted on a worker, every mount stat-scanned
// against them, the deltas committed - after a writer's run left Engine.snapshot (an engine-owner registry, as
// FRT.bat assets scan --engine) and AssetRegistry.snapshot. The 200k-file warm start and the 100k cold scan are
// DISABLED_ (nightly: --gtest_also_run_disabled_tests), judged by §5.11 under FRT_PERF_STRICT=1 and printed otherwise.
// How a row is judged: ../BenchmarkBudget.h; Debug scales the trees down and prints only.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "../BenchmarkBudget.h"
#include "TemplateTestUtils.h"

#include "Assets/AssetId.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/FileWatcherService.h"
#include "BuildConfig.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Timestamp.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::bench;
using frt::test::rules::CRulesTree;


namespace
{
constexpr bool bOptimized = !FRT_BUILD_DEBUG;
constexpr const char* Prefix = "[registry]";

constexpr FStringLiteral EngineRulesText =
	"FormatVersion: 1\nScan:\n  Quiet: [ \"**/README.md\" ]\nPresets:\n  Texture/Color: { ColorSpace: Srgb, Mips: Box, Compression: None }\n"
	"Rules:\n  - { Match: \"**/*.{png,jpg}\", Preset: Texture/Color }\n";
constexpr FStringLiteral ProjectRulesText = "FormatVersion: 1\nScan:\n  Quiet: [ \"license.txt\" ]\n";

jobs::SWorkerPoolDesc MakePoolDesc (uint32 Threads)
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = Threads;
	desc.Name = "Registry Budget";
	return desc;
}

std::vector<uint8> MakePngHeader ()
{
	return { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 4, 0, 0, 0, 4, 8, 6, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 'I', 'D', 'A', 'T', 0, 0, 0, 0, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0 };
}

// A source file's meta as the lifecycle writes it (EmitMeta, canonical): a GUID derived from its mount path.
void WriteMeta (const std::filesystem::path& Source, FStringView MountPath)
{
	SAssetMeta meta;
	meta.Guid = MakeDerivedGuid(SGuid {}, "registry-budget-meta", MountPath);
	FString text;
	ASSERT_TRUE(EmitMeta(meta, text));
	std::filesystem::path path = Source;
	path += FStringView(MetaFileSuffix).ToPath();
	ASSERT_TRUE(platform::WriteFileBytes(path, text.AsBytes()));
}

/**
 * A synthetic project of Files files below Content/, 100 per folder: in each folder 70 textures, 10 legacy materials
 * naming one of them and 20 text files nobody claims (one RULE002 per folder). bMetas (M4, edit [63]): each texture
 * and material has its meta beside it as well, so the tree holds Files + 80% of Files files.
 */
void WriteSyntheticProject (const CRulesTree& Tree, uint32 Files, bool bMetas = false)
{
	Tree.WriteEngineRules(EngineRulesText);
	Tree.WriteProjectRules(ProjectRulesText);
	const std::vector<uint8> png = MakePngHeader();
	frt::test::rules::MakeDirectories(Tree.Tree.EngineContent() / "Textures");
	ASSERT_TRUE(platform::WriteFileBytes(Tree.Tree.EngineContent() / "Textures" / "Default.png", png));
	const std::filesystem::path content = Tree.Tree.ProjectContent();
	const uint32 folders = (Files + 99u) / 100u;
	for (uint32 folder = 0u; folder < folders; ++folder)
	{
		const FString folderPath = FString::Format("/Project/Тека {:02}/Folder {}", folder / 20u, folder);
		const std::filesystem::path directory = content / FString::Format("Тека {:02}", folder / 20u).ToPath() / FString::Format("Folder {}", folder).ToPath();
		frt::test::rules::MakeDirectories(directory);
		for (uint32 file = 0u; file < 100u && folder * 100u + file < Files; ++file)
		{
			if (file < 70u)
			{
				const FString name = FString::Format("Файл_{}.png", file);
				ASSERT_TRUE(platform::WriteFileBytes(directory / name.ToPath(), png));
				if (bMetas)
				{
					WriteMeta(directory / name.ToPath(), FString::Format("{}/{}", folderPath, name));
				}
			}
			else if (file < 80u)
			{
				const FString name = FString::Format("М_{}.frtmat.yml", file);
				const FString text = FString::Format("version: 2\nname: М_{}\n\nalbedo:\n  texture: Файл_{}.png\n", file, file - 70u);
				ASSERT_TRUE(platform::WriteFileBytes(directory / name.ToPath(), text.AsBytes()));
				if (bMetas)
				{
					WriteMeta(directory / name.ToPath(), FString::Format("{}/{}", folderPath, name));
				}
			}
			else
			{
				ASSERT_TRUE(platform::WriteFileBytes(directory / FString::Format("Нотатка_{}.txt", file).ToPath(), FStringView("notes").AsBytes()));
			}
		}
	}
}

struct SStart
{
	SInitialScanReport Report;
	SAssetRegistryCounters Counters;
};

std::shared_ptr<const CMountTable> EngineOnly (const CRulesTree& Tree)
{
	SMountRoots roots;
	roots.EngineDir = Tree.Tree.EngineDir();
	roots.EngineContentDir = Tree.Tree.EngineContent();
	roots.EngineConfigDir = Tree.Tree.EngineConfig();
	return std::make_shared<const CMountTable>(CMountTable::Resolve(roots, nullptr));
}

// One registry's start, Start to the initial scan's end, then its shut-down.
SStart StartOnce (jobs::CWorkerPool& Pool, std::shared_ptr<const CMountTable> Mounts, EAssetRegistryOwner Owner, bool bWriter)
{
	SAssetRegistryDesc desc;
	desc.Mounts = std::move(Mounts);
	desc.Pool = &Pool;
	desc.Owner = Owner;
	desc.bWriter = bWriter;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	EXPECT_TRUE(registry.WaitForInitialScan(600.0));
	SStart start { registry.GetInitialScanReport(), registry.GetCounters() };
	registry.ShutDown();
	return start;
}

// The snapshots a warm start loads: Engine.snapshot by an engine-owner writer, then AssetRegistry.snapshot.
SStart WriteSnapshots (jobs::CWorkerPool& Pool, std::shared_ptr<const CMountTable> EngineMounts, std::shared_ptr<const CMountTable> ProjectMounts)
{
	(void)StartOnce(Pool, std::move(EngineMounts), EAssetRegistryOwner::Engine, true);
	return StartOnce(Pool, std::move(ProjectMounts), EAssetRegistryOwner::Project, true);
}

// The best of Rounds warm starts of a reader (each loads the snapshots and scans, and writes nothing).
SStart BestWarmStart (jobs::CWorkerPool& Pool, const std::shared_ptr<const CMountTable>& Mounts, uint32 Rounds = 3u)
{
	SStart best;
	best.Report.TotalMs = 1e30;
	for (uint32 round = 0u; round < Rounds; ++round)
	{
		const SStart start = StartOnce(Pool, Mounts, EAssetRegistryOwner::Project, false);
		if (start.Report.TotalMs < best.Report.TotalMs)
		{
			best = start;
		}
	}
	return best;
}

void MeasureWarmStart (uint32 Files, const SBudget& Warm, const SBudget& AfterEdit, bool bWatchedEdit, bool bMetas = false)
{
	CRulesTree tree(FString::Format("Registry Budget {}{}", Files, bMetas ? " metas" : ""));
	WriteSyntheticProject(tree, Files, bMetas);
	jobs::CWorkerPool pool(MakePoolDesc(jobs::CWorkerPool::GetDefaultThreadCount()));
	const std::shared_ptr<const CMountTable> project = std::make_shared<const CMountTable>(tree.Resolve());
	const SStart cold = WriteSnapshots(pool, EngineOnly(tree), project);
	std::printf("%s cold start of %u files: %.1f ms, %llu analyzed (%u threads; not budgeted at this size)\n", Prefix, cold.Report.Files,
		cold.Report.TotalMs, static_cast<unsigned long long>(cold.Counters.Analyzed), pool.GetThreadCount());

	const uint32 assets = Files / 100u * 80u + (std::min)(Files % 100u, 80u);
	if (bMetas)
	{
		EXPECT_EQ(cold.Counters.MetasRead, assets) << "every asset's meta read once";
	}

	const SStart warm = BestWarmStart(pool, project);
	EXPECT_EQ(warm.Counters.Analyzed, 0u);
	EXPECT_EQ(warm.Counters.Hashed, 0u);
	EXPECT_EQ(warm.Counters.MetasRead, 0u);
	const FString name = bMetas ? FString::Format("WarmStart{}kAssetsWithMetasMs", assets / 1000u) : FString::Format("WarmStart{}kMs", Files / 1000u);
	const FString note = FString::Format("({} files, {} records; snapshot load {:.2f}, scan {:.2f}, convert {:.2f}, commit {:.2f}, RULE002 {:.2f} ms; best of 3)",
		warm.Report.Files, warm.Report.Records, warm.Report.LoadMs, warm.Report.ScanMs, warm.Report.ConvertMs, warm.Report.CommitMs, warm.Report.UnclaimedMs);
	CheckBudget(Prefix, name.CStr(), warm.Report.TotalMs, Warm, "ms", note.CStr());

	// A mount-rule edit with no Analyze-relevant change: every record re-evaluated in memory (glob matching), nothing
	// analyzed or hashed; the edit labels one folder, so a hundred records are committed again.
	tree.WriteProjectRules(FString(ProjectRulesText) + "Rules:\n  - { Match: \"Тека 00/Folder 0/**\", Labels: [ Budget ] }\n");
	const SStart edited = BestWarmStart(pool, project);
	EXPECT_EQ(edited.Counters.Analyzed, 0u);
	EXPECT_EQ(edited.Counters.Hashed, 0u);
	EXPECT_GT(edited.Counters.ReEvaluated, 0u);
	EXPECT_EQ(edited.Counters.MetasRead, 0u) << "a re-evaluation takes an unchanged meta from its copy";
	const FString editName = bMetas ? FString::Format("WarmStartAfterRuleEdit{}kAssetsWithMetasMs", assets / 1000u)
		: FString::Format("WarmStartAfterRuleEdit{}kMs", Files / 1000u);
	const FString editNote = FString::Format("({} records re-evaluated, 0 analyzed, 0 hashed; scan {:.2f}, convert {:.2f}, commit {:.2f} ms; best of 3)",
		edited.Counters.ReEvaluated, edited.Report.ScanMs, edited.Report.ConvertMs, edited.Report.CommitMs);
	CheckBudget(Prefix, editName.CStr(), edited.Report.TotalMs, AfterEdit, "ms", editNote.CStr());

	// The worst case, reported only: an edit that changes every texture's labels commits each of them again.
	tree.WriteProjectRules(FString(ProjectRulesText) + "Rules:\n  - { Match: \"**/*.png\", Labels: [ Budget ] }\n");
	const SStart everything = BestWarmStart(pool, project, 1u);
	EXPECT_EQ(everything.Counters.Analyzed, 0u);
	const FString allName = FString::Format("WarmStartRelabelAll{}kMs", Files / 1000u);
	const FString allNote = FString::Format("(every texture's labels change: commit {:.2f} ms of it)", everything.Report.CommitMs);
	CheckBudget(Prefix, allName.CStr(), everything.Report.TotalMs, SBudget {}, "ms", allNote.CStr(), false);

	if (!bWatchedEdit)
	{
		return;
	}
	// One material edited while a writer watches: the owner thread's cost of what it commits (scan delta, Analyze
	// delta, RULE002 if any) with the journal appends.
	SFileWatcherSettings settings;
	CFileWatcherService watcher(settings);
	for (const SMount& mount : project->GetMounts())
	{
		watcher.AddMount(mount.ContentDir);
		watcher.AddMount(mount.RulesFile.parent_path());
	}
	SAssetRegistryDesc desc;
	desc.Mounts = project;
	desc.Pool = &pool;
	desc.bWriter = true;
	desc.Watcher = &watcher;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	ASSERT_TRUE(registry.WaitForInitialScan(600.0));
	double best = 1e30;
	for (uint32 round = 0u; round < 3u; ++round)
	{
		const SAssetRegistryCounters before = registry.GetCounters();
		const FString text = FString::Format("version: 2\nname: М_70\n\nalbedo:\n  texture: Файл_{}.png\n", round + 1u);
		ASSERT_TRUE(platform::WriteFileBytes(tree.Tree.ProjectContent() / FStringView("Тека 00/Folder 0/М_70.frtmat.yml").ToPath(), text.AsBytes()));
		ASSERT_TRUE(registry.Flush(30.0));
		const SAssetRegistryCounters& after = registry.GetCounters();
		EXPECT_EQ(after.Analyzed - before.Analyzed, 1u);
		best = std::min(best, after.CommitMs - before.CommitMs);
	}

	// A mount-rule edit delivered by the watcher that relabels every texture, run frame by frame as the editor runs it:
	// the longest Pump (its commits within FrameBudgetMs, the rules reload, the pass's last delta with RULE002) and the
	// owner thread's time over all of them. Workers' scan and conversion not included.
	tree.WriteProjectRules(FString(ProjectRulesText) + "Rules:\n  - { Match: \"**/*.png\", Labels: [ Watched ] }\n");
	const uint64 revision = registry.GetRevision();
	const uint64 committed = registry.GetCounters().Committed;
	const uint64 analyzed = registry.GetCounters().Analyzed;
	double longest = 0.0;
	double total = 0.0;
	uint32 frames = 0u;
	const uint64 deadline = platform::MakeDeadline(120.0);
	while ((registry.GetRevision() == revision || !registry.IsIdle()) && platform::ReadTimestamp() < deadline)
	{
		watcher.Pump();
		pool.PumpCompletions();
		const uint64 start = platform::ReadTimestamp();
		registry.Pump();
		const double milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		longest = std::max(longest, milliseconds);
		total += milliseconds;
		++frames;
		pool.WaitForCompletions(0.002);
	}
	EXPECT_GT(registry.GetRevision(), revision) << "the edit was delivered and committed";
	EXPECT_EQ(registry.GetCounters().Analyzed, analyzed) << "re-evaluated, never analyzed";
	const uint64 deltas = registry.GetCounters().Committed - committed;
	registry.ShutDown();
	CheckBudget(Prefix, "WatchedEditCommitMs", best, SBudget { 2.0, -1.0, 2.0, 2.0 }, "ms",
		"(owner thread: the edit's commits and journal appends, settle not included; best of 3)");
	const FString ruleEditNote = FString::Format("(every texture relabelled: {} deltas over {} frames, {:.2f} ms of Pump in all)", deltas, frames, total);
	CheckBudget(Prefix, "WatchedRuleEditLongestPumpMs", longest, SBudget {}, "ms", ruleEditNote.CStr(), false);
}
}


// The metas the 100k tree is written with (WriteSyntheticProject's bMetas), on a folder of it: every asset takes its
// meta's GUID and a METAS copy (not a benchmark: 180 files).
TEST(AssetRegistryBudget, TheSyntheticProjectsMetasGiveEveryAssetItsGuid)
{
	CRulesTree tree("Registry Budget metas");
	WriteSyntheticProject(tree, 100u, true);
	jobs::CWorkerPool pool(MakePoolDesc(2u));
	SAssetRegistryDesc desc;
	desc.Mounts = std::make_shared<const CMountTable>(tree.Resolve());
	desc.Pool = &pool;
	desc.bLoadSnapshots = false;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	EXPECT_TRUE(registry.WaitForInitialScan(60.0));
	const CRegistryView& view = registry.Live();
	uint32 assets = 0u;
	uint32 fromMeta = 0u;
	view.ForEachRecord([&] (SRecordId Record)
	{
		SAssetData data;
		if (view.GetRecord(Record, data) && data.MountPath.View().StartsWith(FStringView("/Project/")))
		{
			++assets;
			fromMeta += data.Id.Guid == MakeDerivedGuid(SGuid {}, "registry-budget-meta", data.MountPath) ? 1u : 0u;
		}
	});
	EXPECT_EQ(assets, 80u);
	EXPECT_EQ(fromMeta, 80u);
	EXPECT_EQ(view.GetMetaCopyCount(), 80u);
	registry.ShutDown();
}

TEST(AssetRegistryBudget, TheDemoTemplateWarmStartsWithinItsBudget)
{
	const frt::test::registry::CTemplateCopy copy("Registry Demo Budget");
	jobs::CWorkerPool pool(MakePoolDesc(jobs::CWorkerPool::GetDefaultThreadCount()));
	const std::shared_ptr<const CMountTable> project = std::make_shared<const CMountTable>(copy.Mounts);
	const SStart cold = WriteSnapshots(pool, std::make_shared<const CMountTable>(copy.ResolveEngineOnly()), project);
	std::printf("%s Demo cold start: %u files, %llu analyzed, %.1f ms (%u threads; not budgeted)\n", Prefix, cold.Report.Files,
		static_cast<unsigned long long>(cold.Counters.Analyzed), cold.Report.TotalMs, pool.GetThreadCount());
	const SStart warm = BestWarmStart(pool, project);
	EXPECT_EQ(warm.Counters.Analyzed, 0u);
	EXPECT_EQ(warm.Counters.Hashed, 0u);
	const FString note = FString::Format("({} files; snapshot load {:.2f} ms; best of 3)", warm.Report.Files, warm.Report.LoadMs);
	CheckBudget(Prefix, "DemoWarmStartMs", warm.Report.TotalMs, SBudget { 20.0, -1.0, 20.0, 30.0 }, "ms", note.CStr());
}

TEST(AssetRegistryBudget, TwentyThousandFilesWarmStartWithinTheirBudgetAlsoAfterAMountRuleEdit)
{
	const uint32 files = bOptimized ? 20000u : 2000u;
	// After the edit: §5.11 as revised at M3 (80 ms at 20k: the load, then 16k records re-evaluated in memory).
	MeasureWarmStart(files, SBudget { 60.0, -1.0, 100.0, 150.0 }, SBudget { 80.0, -1.0, 120.0, 170.0 }, true);
}

TEST(AssetRegistryBudget, AColdFullScanWithAnalyzeOf10kFilesStaysWithinItsBudget)
{
	const uint32 files = bOptimized ? 10000u : 1000u;
	CRulesTree tree("Registry Cold Budget");
	WriteSyntheticProject(tree, files);
	jobs::CWorkerPool pool(MakePoolDesc(16u));
	SAssetRegistryDesc desc;
	desc.Mounts = std::make_shared<const CMountTable>(tree.Resolve());
	desc.Pool = &pool;
	desc.bLoadSnapshots = false;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	ASSERT_TRUE(registry.WaitForInitialScan(600.0));
	const SInitialScanReport report = registry.GetInitialScanReport();
	registry.ShutDown();
	EXPECT_EQ(report.Analyzed, report.Records);
	const FString note = FString::Format("({} files, {} records analyzed, 16 threads)", report.Files, report.Records);
	CheckBudget(Prefix, "ColdScan10kMs", report.TotalMs, SBudget { 3000.0, -1.0, 3000.0, 3000.0 }, "ms", note.CStr(), files == 10000u);
}

// Nightly (§5.11's 100k column; writes 200k files: run alone). No regression guard: FRT_PERF_STRICT=1 holds the
// proposed revision, a run without it prints. §5.11's 400 ms is for ≈ 200k files of which half are metas (100k
// assets); M3 has no metas, so this tree's 200k files are 160k records, and the load grows with records: M3
// measured 577 ms (snapshot load 531, scan 154 beside it) and 639 ms after the rules edit (Release, alone), hence
// the proposed 650 / 720 ms.
TEST(AssetRegistryBudget, DISABLED_TwoHundredThousandFilesWarmStartWithinTheirBudget)
{
	MeasureWarmStart(200000u, SBudget { 400.0, 650.0, -1.0, -1.0 }, SBudget { 400.0, 720.0, -1.0, -1.0 }, false);
}

// Nightly, edit [63]: §5.11's 100k column as it means it, 100k assets with their metas (125k source files, 100k metas:
// 225k files; run alone). The same proposed figures as above; M4 compares the two.
TEST(AssetRegistryBudget, DISABLED_OneHundredThousandAssetsWithMetasWarmStartWithinTheirBudget)
{
	MeasureWarmStart(125000u, SBudget { 400.0, 650.0, -1.0, -1.0 }, SBudget { 400.0, 720.0, -1.0, -1.0 }, false, true);
}

TEST(AssetRegistryBudget, DISABLED_AColdFullScanWithAnalyzeOf100kFilesStaysWithinItsBudget)
{
	CRulesTree tree("Registry Cold Budget 100k");
	WriteSyntheticProject(tree, 100000u);
	jobs::CWorkerPool pool(MakePoolDesc(16u));
	SAssetRegistryDesc desc;
	desc.Mounts = std::make_shared<const CMountTable>(tree.Resolve());
	desc.Pool = &pool;
	desc.bLoadSnapshots = false;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	ASSERT_TRUE(registry.WaitForInitialScan(1200.0));
	const SInitialScanReport report = registry.GetInitialScanReport();
	registry.ShutDown();
	const FString note = FString::Format("({} files, {} records analyzed, 16 threads)", report.Files, report.Records);
	CheckBudget(Prefix, "ColdScan100kMs", report.TotalMs, SBudget { 30000.0, -1.0, -1.0, -1.0 }, "ms", note.CStr());
}
