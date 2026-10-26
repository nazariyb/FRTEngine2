// The registry service (Assets/AssetRegistry.h) over generated mounts, the M3 acceptance cases of design §16.2: a
// restart after a Core.dll rebuild, a mount-rule edit, an engine-rule edit or another rules semantics hash analyzes and
// hashes nothing on unchanged files; a folder Assets.yml edit re-analyzes exactly the files whose AnalyzeStamp changed
// and re-evaluates nothing outside its folder, offline and through the watcher; a plugin's rules never reach /Project
// and back; a torn snapshot section or journal tail costs a rescan, never a wrong registry; a watcher sequence
// (create, modify, rename, folder move, a VCS-sized burst) ends in the dump of a fresh scan; a process without the
// writer lock writes nothing; Validation: reaches the scan's and RULE002's severities; the tri-state Exists and the
// change notification; a pass cut short after its conversion (a clean exit while Analyze is pending, a crash) persists
// nothing that claims work it did not do; a broken journal is mended by the full write its break triggers, and loses no
// commit; a mount-rule edit over many files is committed in deltas of bounded size. Every tree has non-ASCII names and
// lives in the temp directory.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "../Snapshot/JournalTestAccess.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTypes.h"
#include "Assets/FileWatcherService.h"
#include "Assets/MountPaths.h"
#include "Assets/OwnerLock.h"
#include "Assets/Registry/Journal.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/Snapshot.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Process.h"
#include "Platform/Timestamp.h"

using namespace frt;
using namespace frt::assets;
using frt::test::rules::CRulesTree;
using frt::test::rules::DescribeDiagnostics;


namespace frt::assets
{
// CAssetRegistry's test seam (a friend): its journal, to break it as a failed write would.
struct SAssetRegistryTestAccess
{
	static CRegistryJournal* GetJournal (CAssetRegistry& Registry) { return Registry.GetJournal(); }
};
}


namespace
{
constexpr FStringLiteral EngineRulesText =
	"FormatVersion: 1\n"
	"Scan:\n"
	"  Quiet: [ \"**/README.md\" ]\n"
	"Presets:\n"
	"  Texture/Color:  { ColorSpace: Srgb,   Mips: Box, Compression: None }\n"
	"  Texture/Normal: { ColorSpace: Normal, Mips: Box, Compression: None }\n"
	"Rules:\n"
	"  - { Match: \"/Engine/Defaults/**\", Cook: Always }\n"
	"  - { Match: \"**/*.{png,jpg,jpeg,tga,bmp,psd,kra,ora}\", Preset: Texture/Color }\n";

constexpr FStringLiteral ProjectRulesText = "FormatVersion: 1\nScan:\n  Quiet: [ \"license.txt\" ]\n";
constexpr FStringLiteral Plugin = "Рослини";
constexpr FStringLiteral Foliage = "Environment/Листя";

// A PNG's header as stb_image's info reads it: signature, IHDR (8-bit RGBA), an empty IDAT, IEND.
std::vector<uint8> MakePng (uint32 Width, uint32 Height)
{
	std::vector<uint8> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	const auto u32 = [&] (uint32 Value)
	{
		png.insert(png.end(), { uint8(Value >> 24u), uint8(Value >> 16u), uint8(Value >> 8u), uint8(Value) });
	};
	const auto chunk = [&] (const char (&Type)[5], std::initializer_list<uint8> Data, std::span<const uint32> Words = {})
	{
		u32(static_cast<uint32>(Data.size() + Words.size() * 4u));
		png.insert(png.end(), Type, Type + 4);
		for (const uint32 word : Words)
		{
			u32(word);
		}
		png.insert(png.end(), Data);
		u32(0u);   // stb never checks the CRC
	};
	const uint32 size[] = { Width, Height };
	chunk("IHDR", { 8u, 6u, 0u, 0u, 0u }, size);
	chunk("IDAT", {});
	chunk("IEND", {});
	return png;
}

/**
 * /Engine with textures and a quiet README; /Project with textures, a legacy material naming one, a glTF with its
 * buffer, an input action, an unclaimed note, and a folder with two leaves for the folder-rule cases; the plugin
 * /Plugin.Рослини, whose rules label its textures.
 */
class CServiceTree : public CRulesTree
{
public:
	explicit CServiceTree (FStringView Name)
		: CRulesTree(Name)
	{
		WriteEngineRules(EngineRulesText);
		WriteProjectRules(ProjectRulesText);
		WriteEnginePng("Textures/Типовий.png");
		WriteEnginePng("Defaults/Білий.png");
		WriteEngineContent("README.md", "# engine\n");
		WriteProjectPng("Textures/Камінь.png");
		WriteProjectPng("Textures/Камінь_n.png");
		WriteProjectPng("Textures/Трава.png");
		WriteProjectContent("Materials/Камінь.frtmat.yml", "version: 2\nname: Камінь\n\nalbedo:\n  texture: ../Textures/Камінь.png\n");
		WriteProjectContent("Models/Box/Box.gltf", "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"Box.bin\",\"byteLength\":4}]}");
		WriteProjectContent("Models/Box/Box.bin", "abcd");
		WriteProjectContent("Input/IA_Стрибок.frtinputaction", "version: 1\naction: IA_Стрибок\ntype: Button\n");
		WriteProjectContent("Notes/нотатки.txt", "notes");
		WriteProjectPng(FString::Format("{}/Лист.png", Foliage));
		WriteProjectPng(FString::Format("{}/Кора.png", Foliage));
		WriteProjectPng("Environment/Дерева/Дуб.png");
		WritePluginRules(Plugin, "FormatVersion: 1\nRules:\n  - { Match: \"**/*.png\", Labels: [ PluginLabel ] }\n");
		WritePluginPng("Textures/Значок.png");
	}

	void WriteEnginePng (FStringView Relative, uint32 Size = 4u) const { WriteBytes(Tree.EngineContent() / Relative.ToPath(), Size); }
	void WriteProjectPng (FStringView Relative, uint32 Size = 4u) const { WriteBytes(Tree.ProjectContent() / Relative.ToPath(), Size); }
	void WritePluginPng (FStringView Relative) const { WriteBytes(PluginDir(Plugin) / "Content" / Relative.ToPath(), 4u); }

	[[nodiscard]] std::filesystem::path ProjectContent (FStringView Relative) const { return Tree.ProjectContent() / Relative.ToPath(); }
	[[nodiscard]] std::filesystem::path FolderRules () const { return ProjectContent(Foliage) / "Assets.yml"; }

	[[nodiscard]] std::shared_ptr<const CMountTable> Project () const { return std::make_shared<const CMountTable>(Resolve()); }
	[[nodiscard]] std::shared_ptr<const CMountTable> EngineOnly () const
	{
		SMountRoots roots;
		roots.EngineDir = Tree.EngineDir();
		roots.EngineContentDir = Tree.EngineContent();
		roots.EngineConfigDir = Tree.EngineConfig();
		return std::make_shared<const CMountTable>(CMountTable::Resolve(roots, nullptr));
	}

private:
	static void WriteBytes (const std::filesystem::path& Path, uint32 Size)
	{
		frt::test::rules::MakeDirectories(Path.parent_path());
		const std::vector<uint8> png = MakePng(Size, Size);
		ASSERT_TRUE(platform::WriteFileBytes(Path, png)) << FString::FromPath(Path).CStr();
	}
};

struct SOpen
{
	EAssetRegistryOwner Owner = EAssetRegistryOwner::Project;
	bool bWriter = false;
	bool bLoadSnapshots = true;
	bool bSnapshotAtShutDown = true;
	CFileWatcherService* Watcher = nullptr;
	FString Target;
};

// What one run of a registry left: its dump and counters, and what its start-up loaded.
struct SRun
{
	FString Dump;
	SAssetRegistryCounters Counters;
	std::vector<SSnapshotFileReport> Reports;
};

FString DumpOf (const CRegistryView& View)
{
	FString text;
	View.DumpText(text);
	return text;
}

jobs::SWorkerPoolDesc MakePoolDesc (uint32 Threads)
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = Threads;
	desc.Name = "Registry Service Test";
	return desc;
}

std::unique_ptr<CAssetRegistry> Open (jobs::CWorkerPool& Pool, std::shared_ptr<const CMountTable> Mounts, const SOpen& Options = {})
{
	SAssetRegistryDesc desc;
	desc.Mounts = std::move(Mounts);
	desc.Pool = &Pool;
	desc.Owner = Options.Owner;
	desc.bWriter = Options.bWriter;
	desc.bLoadSnapshots = Options.bLoadSnapshots;
	desc.bSnapshotAtShutDown = Options.bSnapshotAtShutDown;
	desc.Watcher = Options.Watcher;
	desc.RulesOptions.Target = Options.Target;
	auto registry = std::make_unique<CAssetRegistry>(std::move(desc));
	registry->Start();
	EXPECT_TRUE(registry->WaitForInitialScan(120.0));
	return registry;
}

SRun Close (std::unique_ptr<CAssetRegistry>& Registry)
{
	SRun run;
	run.Dump = DumpOf(Registry->Live());
	run.Counters = Registry->GetCounters();
	run.Reports.assign(Registry->GetSnapshotReports().begin(), Registry->GetSnapshotReports().end());
	Registry->ShutDown();
	Registry.reset();
	return run;
}

SRun RunOnce (jobs::CWorkerPool& Pool, std::shared_ptr<const CMountTable> Mounts, const SOpen& Options = {})
{
	std::unique_ptr<CAssetRegistry> registry = Open(Pool, std::move(Mounts), Options);
	return Close(registry);
}

// A cold scan by a reader: the dump every warm or incremental registry must equal.
FString FreshDump (jobs::CWorkerPool& Pool, std::shared_ptr<const CMountTable> Mounts, FStringView Target = {})
{
	SOpen options;
	options.bLoadSnapshots = false;
	options.Target = FString(Target);
	return RunOnce(Pool, std::move(Mounts), options).Dump;
}

// Engine.snapshot through an engine-owner registry, then AssetRegistry.snapshot through the project's writer: the
// state every warm-start case starts from.
SRun WriteBothSnapshots (jobs::CWorkerPool& Pool, const CServiceTree& Tree)
{
	SOpen engine;
	engine.Owner = EAssetRegistryOwner::Engine;
	engine.bWriter = true;
	(void)RunOnce(Pool, Tree.EngineOnly(), engine);
	SOpen project;
	project.bWriter = true;
	return RunOnce(Pool, Tree.Project(), project);
}

std::vector<SDiagnostic> ProblemsOf (const CRegistryView& View, uint32 Code)
{
	std::vector<SDiagnostic> found;
	View.ForEachProblem([&] (FStringView, EProblemSource, const SDiagnostic& Diagnostic)
	{
		if (Diagnostic.Code == Code)
		{
			found.push_back(Diagnostic);
		}
	});
	return found;
}

const SSnapshotFileReport* FindReport (const SRun& Run, FStringView FileName)
{
	for (const SSnapshotFileReport& report : Run.Reports)
	{
		if (FString::FromPath(report.Path.filename()) == FileName)
		{
			return &report;
		}
	}
	return nullptr;
}

std::set<FString> LabelsOf (const CRegistryView& View, FStringView MountPath)
{
	std::set<FString> labels;
	if (const std::optional<SRecordId> record = View.FindByPath(MountPath); record.has_value())
	{
		View.ForEachLabel(*record, [&] (FStringView Label, bool) { labels.insert(FString(Label)); });
	}
	return labels;
}

reflect::TypeId TypeOf (const CRegistryView& View, FStringView MountPath)
{
	SAssetData data;
	const std::optional<SRecordId> record = View.FindByPath(MountPath);
	return record.has_value() && View.GetRecord(*record, data) ? data.Type : reflect::InvalidTypeId;
}

// Frames as a host runs them (worker completions, then the registry's Pump with its commit budget) until Done holds;
// false when TimeoutSeconds ran out first.
template <typename F>
bool PumpFramesUntil (jobs::CWorkerPool& Pool, CAssetRegistry& Registry, F&& Done, double TimeoutSeconds)
{
	const uint64 deadline = platform::MakeDeadline(TimeoutSeconds);
	while (!Done())
	{
		if (platform::ReadTimestamp() >= deadline)
		{
			return false;
		}
		Pool.PumpCompletions();
		Registry.Pump();
		if (Done())
		{
			break;
		}
		Pool.WaitForCompletions(0.01);
	}
	return true;
}

// Every file below Directory, relative and '/' separated.
std::set<FString> ListFiles (const std::filesystem::path& Directory)
{
	std::set<FString> files;
	std::error_code ec;
	for (std::filesystem::recursive_directory_iterator it(Directory, ec), end; it != end; it.increment(ec))
	{
		if (!it->is_directory(ec))
		{
			files.insert(FString::FromPath(std::filesystem::relative(it->path(), Directory, ec), EPathStyle::Generic));
		}
	}
	return files;
}
}


TEST(AssetRegistry, AColdScanCommitsWhatTheFilesSayAndARestartAnalyzesAndHashesNothing)
{
	const CServiceTree tree("Restart");
	jobs::CWorkerPool pool(MakePoolDesc(4u));

	SOpen engine;
	engine.Owner = EAssetRegistryOwner::Engine;
	engine.bWriter = true;
	const SRun engineRun = RunOnce(pool, tree.EngineOnly(), engine);
	EXPECT_GT(engineRun.Counters.Analyzed, 0u);
	std::error_code ec;
	EXPECT_TRUE(std::filesystem::exists(GetEngineSnapshotPath(tree.Tree.EngineDir()), ec));
	EXPECT_TRUE(std::filesystem::exists(GetProjectIdPath(tree.Tree.EngineDir()), ec));
	EXPECT_TRUE(std::filesystem::exists(GetRulesCachePath(tree.Tree.EngineDir()), ec));

	SOpen writer;
	writer.bWriter = true;
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project(), writer);
	{
		const CRegistryView& view = registry->Live();
		EXPECT_EQ(TypeOf(view, "/Project/Textures/Камінь.png"), asset_types::Texture);
		EXPECT_EQ(TypeOf(view, "/Project/Models/Box/Box.gltf"), asset_types::Model);
		EXPECT_EQ(TypeOf(view, "/Project/Materials/Камінь.frtmat.yml"), asset_types::Material);
		EXPECT_EQ(TypeOf(view, "/Project/Input/IA_Стрибок.frtinputaction"), asset_types::InputAction);
		std::vector<SRecordId> found;
		SDiagnostic error;
		EXPECT_EQ(view.RunText("ref:/Project/Textures/Камінь.png", found, &error), std::optional<uint32>(1u)) << error.Message.CStr();
		EXPECT_TRUE(ProblemsOf(view, Rule001).empty()) << DescribeDiagnostics(ProblemsOf(view, Rule001)).CStr();
		const std::vector<SDiagnostic> rule002 = ProblemsOf(view, Rule002);
		ASSERT_EQ(rule002.size(), 1u) << DescribeDiagnostics(rule002).CStr();
		EXPECT_EQ(rule002[0].Subject, "/Project/Notes");
		const std::optional<uint32> buffer = view.FindFileExact("/Project/Models/Box/Box.bin");
		SFileData data;
		ASSERT_TRUE(buffer.has_value() && view.GetFile(*buffer, data));
		EXPECT_EQ(data.Class, EFileClass::Companion);
	}
	const SRun cold = Close(registry);
	EXPECT_GT(cold.Counters.Analyzed, 0u);
	EXPECT_TRUE(std::filesystem::exists(GetRegistrySnapshotPath(tree.Tree.ProjectDir()), ec));
	EXPECT_TRUE(std::filesystem::exists(GetProjectIdPath(tree.Tree.ProjectDir()), ec));
	EXPECT_TRUE(std::filesystem::exists(GetRulesCachePath(tree.Tree.ProjectDir()), ec));

	// (a) The restart after a Core.dll rebuild: the same rules basis and files: nothing analyzed, hashed or re-evaluated.
	const SRun warm = RunOnce(pool, tree.Project(), writer);
	EXPECT_EQ(warm.Counters.Analyzed, 0u);
	EXPECT_EQ(warm.Counters.Hashed, 0u);
	EXPECT_EQ(warm.Counters.ReEvaluated, 0u);
	EXPECT_EQ(warm.Dump, cold.Dump);
	const SSnapshotFileReport* engineReport = FindReport(warm, "Engine.snapshot");
	const SSnapshotFileReport* projectReport = FindReport(warm, "AssetRegistry.snapshot");
	ASSERT_TRUE(engineReport != nullptr && projectReport != nullptr);
	EXPECT_EQ(engineReport->State, ESnapshotFileState::Loaded) << engineReport->Message.CStr();
	EXPECT_EQ(projectReport->State, ESnapshotFileState::Loaded) << projectReport->Message.CStr();
	// A reader warm-starts the same way, and a fresh scan agrees with both.
	const SRun reader = RunOnce(pool, tree.Project());
	EXPECT_EQ(reader.Counters.Analyzed, 0u);
	EXPECT_EQ(reader.Dump, cold.Dump);
	EXPECT_EQ(FreshDump(pool, tree.Project()), cold.Dump);
}

TEST(AssetRegistry, RuleEditsAndAnotherSemanticsHashReEvaluateWithoutAnalyzeOrHashes)
{
	const CServiceTree tree("Rule Edits");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	(void)WriteBothSnapshots(pool, tree);
	SOpen writer;
	writer.bWriter = true;

	// (b) A mount-rule edit with no Analyze-relevant change: every record of /Project re-evaluated, labels changed.
	tree.WriteProjectRules(FString(ProjectRulesText) + "Rules:\n  - { Match: \"Textures/**\", Labels: [ Герой ] }\n");
	const SRun mountEdit = RunOnce(pool, tree.Project(), writer);
	EXPECT_EQ(mountEdit.Counters.Analyzed, 0u);
	EXPECT_EQ(mountEdit.Counters.Hashed, 0u);
	EXPECT_GT(mountEdit.Counters.ReEvaluated, 0u);
	EXPECT_EQ(mountEdit.Dump, FreshDump(pool, tree.Project()));
	EXPECT_NE(mountEdit.Dump.View().Find(FStringView("Герой")), FStringView::NotFound);
	// The restart after it re-evaluates nothing: the new RULEFILES row went in with the records it covers.
	EXPECT_EQ(RunOnce(pool, tree.Project(), writer).Counters.ReEvaluated, 0u);

	// (c) A non-Analyze-relevant edit of the engine's rules: the project's RulesBasis and /Engine's row change.
	tree.WriteEngineRules(FString(EngineRulesText) + "  - { Match: \"**/*.txt\", Cook: Never }\n");
	const SRun engineEdit = RunOnce(pool, tree.Project(), writer);
	EXPECT_EQ(engineEdit.Counters.Analyzed, 0u);
	EXPECT_EQ(engineEdit.Counters.Hashed, 0u);
	EXPECT_GT(engineEdit.Counters.ReEvaluated, 0u);
	const SSnapshotFileReport* report = FindReport(engineEdit, "AssetRegistry.snapshot");
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(report->bRuleFilesDropped);
	EXPECT_TRUE(report->bRulesBasisChanged);
	EXPECT_EQ(engineEdit.Dump, FreshDump(pool, tree.Project()));

	// (d) Another rules semantics hash (a rebuilt Core.dll whose importer table or rules engine changed; here a Target):
	// everything re-evaluated, nothing analyzed or hashed.
	SOpen target = writer;
	target.Target = "Win64-Headless";
	const SRun semantics = RunOnce(pool, tree.Project(), target);
	EXPECT_EQ(semantics.Counters.Analyzed, 0u);
	EXPECT_EQ(semantics.Counters.Hashed, 0u);
	EXPECT_GT(semantics.Counters.ReEvaluated, 0u);
	EXPECT_EQ(semantics.Dump, FreshDump(pool, tree.Project(), "Win64-Headless"));
}

TEST(AssetRegistry, AFolderRuleEditReAnalyzesOnlyChangedStampsAndReEvaluatesOnlyItsFolder)
{
	const CServiceTree tree("Folder Edit");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	(void)WriteBothSnapshots(pool, tree);
	SOpen writer;
	writer.bWriter = true;

	// A settings edit (ColorSpace is not Analyze-relevant in M3): the folder's two textures re-evaluated, none analyzed.
	frt::test::rules::WriteFile(tree.FolderRules(), "FormatVersion: 1\nRules:\n  - { Match: \"*.png\", For: Texture, Settings: { ColorSpace: Linear } }\n");
	const SRun colorSpace = RunOnce(pool, tree.Project(), writer);
	EXPECT_EQ(colorSpace.Counters.Analyzed, 0u);
	EXPECT_EQ(colorSpace.Counters.Hashed, 0u);
	EXPECT_EQ(colorSpace.Counters.ReEvaluated, 2u) << "only the records in the folder's interval";

	// An importer switch changes the AnalyzeStamp: exactly those two PNGs analyzed again, now frt.Raw.
	frt::test::rules::WriteFile(tree.FolderRules(), "FormatVersion: 1\nRules:\n  - { Match: \"*.png\", Importer: Raw }\n");
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project(), writer);
	EXPECT_EQ(registry->GetCounters().Analyzed, 2u);
	EXPECT_EQ(registry->GetCounters().ReEvaluated, 0u);
	EXPECT_EQ(TypeOf(registry->Live(), FString::Format("/Project/{}/Лист.png", Foliage)), asset_types::Raw);
	EXPECT_EQ(TypeOf(registry->Live(), "/Project/Environment/Дерева/Дуб.png"), asset_types::Texture);
	const SRun raw = Close(registry);
	EXPECT_EQ(raw.Dump, FreshDump(pool, tree.Project()));

	// The folder file removed offline: a change too (absent = 0), back to textures.
	std::error_code ec;
	std::filesystem::remove(tree.FolderRules(), ec);
	const SRun removed = RunOnce(pool, tree.Project(), writer);
	EXPECT_EQ(removed.Counters.Analyzed, 2u);
	EXPECT_EQ(removed.Dump, FreshDump(pool, tree.Project()));
}

TEST(AssetRegistry, PluginRulesReachOnlyThePluginAndProjectRulesOnlyTheProject)
{
	const CServiceTree tree("Plugin Rules");
	tree.WriteProjectRules(FString(ProjectRulesText) + "Rules:\n  - { Match: \"**/*.png\", Labels: [ ProjectLabel ] }\n");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project());
	const CRegistryView& view = registry->Live();
	const FString pluginTexture = FString::Format("/Plugin.{}/Textures/Значок.png", Plugin);
	EXPECT_TRUE(LabelsOf(view, pluginTexture) == (std::set<FString> { "PluginLabel" }));
	EXPECT_TRUE(LabelsOf(view, "/Project/Textures/Камінь.png") == (std::set<FString> { "ProjectLabel" }));

	// Explain shows the chains: the plugin's file and the engine fallback for the plugin's texture, never /Project's.
	std::vector<SDiagnostic> diagnostics;
	const CRulesEngine& rules = registry->GetRules();
	const std::shared_ptr<const SScanScope> pluginScope = rules.OpenFolderScope(FString(GetMountPathParent(pluginTexture)), diagnostics);
	const SRuleExplanation pluginExplanation = rules.Explain(*pluginScope, pluginTexture);
	ASSERT_FALSE(pluginExplanation.Chain.empty());
	for (const SRuleChainEntry& entry : pluginExplanation.Chain)
	{
		EXPECT_NE(entry.Origin.MountName, "Project") << entry.Origin.File.CStr();
	}
	const std::shared_ptr<const SScanScope> projectScope = rules.OpenFolderScope("/Project/Textures", diagnostics);
	for (const SRuleChainEntry& entry : rules.Explain(*projectScope, "/Project/Textures/Камінь.png").Chain)
	{
		EXPECT_EQ(entry.Origin.MountName.View().Find(FStringView("Plugin.")), FStringView::NotFound) << entry.Origin.File.CStr();
	}
	(void)Close(registry);
}

TEST(AssetRegistry, ATornSnapshotSectionOrJournalTailCostsARescanNeverAWrongRegistry)
{
	const CServiceTree tree("Torn Files");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	const SRun first = WriteBothSnapshots(pool, tree);
	const std::filesystem::path snapshot = GetRegistrySnapshotPath(tree.Tree.ProjectDir());
	const std::filesystem::path journal = GetJournalPath(snapshot);

	// A run that commits changes and leaves them in the journal alone (no exit snapshot, as after a crash).
	tree.WriteProjectPng("Textures/Трава.png", 8u);
	tree.WriteProjectPng("Textures/Нова.png");
	SOpen crash;
	crash.bWriter = true;
	crash.bSnapshotAtShutDown = false;
	(void)RunOnce(pool, tree.Project(), crash);
	const SJournalContents contents = ReadJournal(journal, false);
	ASSERT_TRUE(contents.bHeaderValid) << contents.Message.CStr();
	EXPECT_GT(contents.ValidBytes, uint64(JournalHeaderBytes)) << "the commits are in the journal";
	const FString expected = FreshDump(pool, tree.Project());

	// The journal replays: a warm start with nothing to analyze.
	SOpen reader;
	const SRun replayed = RunOnce(pool, tree.Project(), reader);
	EXPECT_EQ(replayed.Dump, expected);
	EXPECT_EQ(replayed.Counters.Analyzed, 0u);
	const SSnapshotFileReport* report = FindReport(replayed, "AssetRegistry.snapshot");
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(report->bJournalUsed) << report->JournalMessage.CStr();

	// A torn tail (half a frame appended) is dropped; the rest replays.
	{
		std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(journal);
		ASSERT_TRUE(bytes.has_value());
		bytes->insert(bytes->end(), { 0x10u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u });
		ASSERT_TRUE(platform::WriteFileBytes(journal, *bytes));
	}
	const SRun torn = RunOnce(pool, tree.Project(), reader);
	EXPECT_EQ(torn.Dump, expected);
	report = FindReport(torn, "AssetRegistry.snapshot");
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(report->bJournalTornTail);

	// A damaged section (bytes flipped in the middle of the file): dropped alone or the file rejected; the scan and
	// Analyze rebuild what it held.
	{
		std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(snapshot);
		ASSERT_TRUE(bytes.has_value() && bytes->size() > 256u);
		for (size_t i = bytes->size() / 2u; i < bytes->size() / 2u + 16u; ++i)
		{
			(*bytes)[i] ^= 0x5Au;
		}
		ASSERT_TRUE(platform::WriteFileBytes(snapshot, *bytes));
	}
	const SRun damaged = RunOnce(pool, tree.Project(), reader);
	EXPECT_EQ(damaged.Dump, expected);
	report = FindReport(damaged, "AssetRegistry.snapshot");
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(!report->DroppedSections.empty() || !report->DroppedTypeTables.empty() || report->State == ESnapshotFileState::Rejected)
		<< report->Message.CStr();
	(void)first;

	// One bad section of Analyze outputs (DEPS: edges and companions) costs only the records that had them: they are
	// flagged StaleTags at load and analyzed again, nothing else.
	SOpen writer;
	writer.bWriter = true;
	(void)RunOnce(pool, tree.Project(), writer);
	{
		std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(snapshot);
		ASSERT_TRUE(bytes.has_value() && bytes->size() > SnapshotHeaderBytes);
		uint32 sections = 0u;
		std::memcpy(&sections, bytes->data() + 12u, sizeof(sections));
		bool bFlipped = false;
		for (uint32 i = 0u; i < sections; ++i)
		{
			const uint8* entry = bytes->data() + SnapshotHeaderBytes + i * SnapshotSectionEntryBytes;
			uint32 id = 0u;
			uint64 offset = 0u;
			uint64 size = 0u;
			std::memcpy(&id, entry, sizeof(id));
			std::memcpy(&offset, entry + 8u, sizeof(offset));
			std::memcpy(&size, entry + 16u, sizeof(size));
			if (id == static_cast<uint32>(ESnapshotSection::Deps) && size > 0u)
			{
				(*bytes)[offset + size / 2u] ^= 0xA5u;
				bFlipped = true;
			}
		}
		ASSERT_TRUE(bFlipped);
		ASSERT_TRUE(platform::WriteFileBytes(snapshot, *bytes));
	}
	const SRun stale = RunOnce(pool, tree.Project(), reader);
	EXPECT_EQ(stale.Dump, expected);
	report = FindReport(stale, "AssetRegistry.snapshot");
	ASSERT_NE(report, nullptr);
	EXPECT_EQ(report->State, ESnapshotFileState::Loaded) << report->Message.CStr();
	EXPECT_NE(std::find(report->DroppedSections.begin(), report->DroppedSections.end(), ESnapshotSection::Deps), report->DroppedSections.end());
	EXPECT_GT(report->StaleRecords, 0u);
	EXPECT_EQ(stale.Counters.Analyzed, report->StaleRecords) << "exactly the records that lost their edges";
}

TEST(AssetRegistry, AWatcherSequenceEndsInTheDumpOfAFreshScan)
{
	const CServiceTree tree("Watcher Sequence");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	SFileWatcherSettings settings;
	settings.SettleMs = 50u;
	settings.RemoveGraceMs = 100u;
	settings.VcsSettleMs = 200u;
	CFileWatcherService watcher(settings);
	{
		const std::shared_ptr<const CMountTable> mounts = tree.Project();
		for (const SMount& mount : mounts->GetMounts())
		{
			watcher.AddMount(mount.ContentDir);
			watcher.AddMount(mount.RulesFile.parent_path());
		}
	}
	SOpen writer;
	writer.bWriter = true;
	writer.Watcher = &watcher;
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project(), writer);
	uint32 notifications = 0u;
	const SDelegateHandle handle = registry->OnAssetsChanged().Add([&] (const SAssetChangeSet&) { ++notifications; });
	const auto settle = [&] (const char* Step)
	{
		ASSERT_TRUE(registry->Flush(30.0)) << Step;
	};

	tree.WriteProjectPng("Textures/Нова.png");                                  // create
	settle("create");
	EXPECT_EQ(TypeOf(registry->Live(), "/Project/Textures/Нова.png"), asset_types::Texture);
	tree.WriteProjectPng("Textures/Камінь.png", 16u);                           // modify
	settle("modify");
	std::error_code ec;
	std::filesystem::rename(tree.ProjectContent("Textures/Трава.png"), tree.ProjectContent("Textures/Трава2.png"), ec);   // rename
	ASSERT_FALSE(ec);
	settle("rename");
	EXPECT_FALSE(registry->Live().FindByPath("/Project/Textures/Трава.png").has_value());
	std::filesystem::rename(tree.ProjectContent("Environment/Дерева"), tree.ProjectContent(FString::Format("{}/Дерева", Foliage)), ec);   // folder move
	ASSERT_FALSE(ec);
	settle("folder move");
	for (uint32 i = 0u; i < 300u; ++i)                                            // a VCS-sized burst
	{
		tree.WriteProjectPng(FString::Format("Пакет/Файл_{:03}.png", i));
	}
	settle("burst");
	const uint64 analyzed = registry->GetCounters().Analyzed;
	frt::test::rules::WriteFile(tree.FolderRules(),                             // a folder rule that changes no stamp
		"FormatVersion: 1\nRules:\n  - { Match: \"*.png\", For: Texture, Settings: { ColorSpace: Linear } }\n");
	settle("folder settings");
	EXPECT_EQ(registry->GetCounters().Analyzed, analyzed);
	frt::test::rules::WriteFile(tree.FolderRules(), "FormatVersion: 1\nRules:\n  - { Match: \"*.png\", Importer: Raw }\n");   // one that does
	settle("folder rule");
	// "*.png" floats (gitignore): the folder's two PNGs and the one in the subfolder moved into it, nothing else.
	EXPECT_EQ(registry->GetCounters().Analyzed - analyzed, 3u);
	EXPECT_EQ(TypeOf(registry->Live(), FString::Format("/Project/{}/Дерева/Дуб.png", Foliage)), asset_types::Raw);
	EXPECT_EQ(TypeOf(registry->Live(), FString::Format("/Project/{}/Кора.png", Foliage)), asset_types::Raw);
	tree.WriteProjectRules("FormatVersion: 1\nScan:\n  Quiet: [ \"license.txt\" ]\n  Exclude: [ \"Notes/**\" ]\n");   // a mount rule (membership)
	settle("mount rule");

	EXPECT_GT(notifications, 0u);
	registry->OnAssetsChanged().Remove(handle);
	const SRun live = Close(registry);
	EXPECT_EQ(live.Dump, FreshDump(pool, tree.Project()));
	EXPECT_EQ(live.Dump.View().Find(FStringView("нотатки")), FStringView::NotFound) << "excluded by the mount rule";
	EXPECT_NE(live.Dump.View().Find(FStringView("Файл_299.png")), FStringView::NotFound);
}

TEST(AssetRegistry, AProcessWithoutTheWriterLockWritesNothing)
{
	const CServiceTree tree("No Lock");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	// Another process's lock (taken here): the host would not set bWriter.
	FString owner;
	std::optional<platform::CExclusiveFileLock> lock = TryTakeOwnerLock(tree.Tree.ProjectDir(), &owner);
	ASSERT_TRUE(lock.has_value());
	const std::set<FString> before = ListFiles(tree.Tree.ProjectDir() / "Local");
	EXPECT_TRUE(before == (std::set<FString> { "Assets/Owner.lock" }));

	SFileWatcherSettings settings;
	CFileWatcherService watcher(settings);
	SOpen reader;
	reader.Watcher = &watcher;
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project(), reader);
	EXPECT_FALSE(registry->IsWriter());
	tree.WriteProjectPng("Textures/Нова.png");
	const FString added[] = { "/Project/Textures/Нова.png" };
	ASSERT_TRUE(registry->ScanPathsSynchronous(added, 30.0));
	EXPECT_FALSE(registry->WriteSnapshotNow());
	(void)Close(registry);

	EXPECT_TRUE(ListFiles(tree.Tree.ProjectDir() / "Local") == before);
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(tree.Tree.EngineDir() / "Local", ec));
	EXPECT_EQ(ListFiles(tree.Tree.ProjectContent()).count("Textures/Нова.png"), 1u);
	for (const FString& file : ListFiles(tree.Tree.ProjectContent()))
	{
		EXPECT_FALSE(file.View().Contains(FStringView(".frt-fence"))) << file.CStr();
	}
}

TEST(AssetRegistry, ValidationSeveritiesReachTheScanAndUnclaimedProblemsUnderTheirFolderOnly)
{
	const CServiceTree tree("Validation");
	frt::test::rules::WriteFile(tree.ProjectContent("Notes/Assets.yml"), "FormatVersion: 1\nValidation: { PATH003: Error, RULE002: Info }\n");
	// PATH003: a relative path over 180 bytes, under Notes and outside it.
	FString longName;
	for (uint32 i = 0u; i < 18u; ++i)
	{
		longName += "Довга";   // 2 UTF-8 bytes a letter: over 180 bytes, short of MAX_PATH in UTF-16
	}
	longName += ".txt";
	tree.WriteProjectContent(FString::Format("Notes/{}", longName), "x");
	tree.WriteProjectContent(FString::Format("Other/{}", longName), "x");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project());
	const CRegistryView& view = registry->Live();
	for (const SDiagnostic& diagnostic : ProblemsOf(view, Path003))
	{
		const bool bNotes = diagnostic.Subject.StartsWith("/Project/Notes/");
		EXPECT_EQ(diagnostic.Severity, bNotes ? ESeverity::Error : ESeverity::Warning) << diagnostic.Subject.CStr();
	}
	EXPECT_EQ(ProblemsOf(view, Path003).size(), 2u);
	const std::vector<SDiagnostic> rule002 = ProblemsOf(view, Rule002);
	ASSERT_EQ(rule002.size(), 2u) << DescribeDiagnostics(rule002).CStr();
	for (const SDiagnostic& diagnostic : rule002)
	{
		EXPECT_EQ(diagnostic.Severity, diagnostic.Subject == "/Project/Notes" ? ESeverity::Info : ESeverity::Warning) << diagnostic.Subject.CStr();
	}
	(void)Close(registry);
}

TEST(AssetRegistry, ExistsIsUnknownUntilTheInitialScanAndTheChangeSetSaysItCompleted)
{
	const CServiceTree tree("Exists");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	SAssetRegistryDesc desc;
	desc.Mounts = tree.Project();
	desc.Pool = &pool;
	CAssetRegistry registry(std::move(desc));
	const SAssetId rock { MakeProvisionalGuid("/Project/Textures/Камінь.png"), 0u };
	EXPECT_EQ(registry.Exists(rock), EExists::Unknown);
	bool bCompleted = false;
	const SDelegateHandle handle = registry.OnAssetsChanged().Add([&] (const SAssetChangeSet& Changes) { bCompleted = bCompleted || Changes.bScanCompleted; });
	registry.Start();
	EXPECT_EQ(registry.Exists(rock), EExists::Unknown);
	EXPECT_EQ(registry.GetScanState() == EScanState::Ready, false);
	ASSERT_TRUE(registry.WaitForInitialScan(60.0));
	EXPECT_TRUE(bCompleted);
	EXPECT_EQ(registry.Exists(rock), EExists::Yes);
	EXPECT_EQ(registry.Exists(SAssetId { MakeProvisionalGuid("/Project/Textures/Немає.png"), 0u }), EExists::No);
	const SInitialScanReport& report = registry.GetInitialScanReport();
	EXPECT_TRUE(report.bComplete);
	EXPECT_EQ(report.Records, registry.Live().GetRecordCount());
	registry.OnAssetsChanged().Remove(handle);
	registry.ShutDown();
}

TEST(AssetRegistry, CompactionKeepsTheDumpAndTheJournalReplay)
{
	const CServiceTree tree("Compaction");
	// Ten models of fifty meshes, renamed every round: every re-analysis leaves fifty dead sub-entry rows behind.
	const auto writeModels = [&] (uint32 Round)
	{
		for (uint32 model = 0u; model < 10u; ++model)
		{
			FString meshes;
			for (uint32 mesh = 0u; mesh < 50u; ++mesh)
			{
				meshes.AppendFormat("{}{{\"name\":\"Сітка_{}_{}\",\"primitives\":[]}}", mesh == 0u ? "" : ",", Round, mesh);
			}
			const FString text = FString::Format("{{\"asset\":{{\"version\":\"2.0\"}},\"meshes\":[{}]}}{:>{}}", meshes, "", Round);
			tree.WriteProjectContent(FString::Format("Models/Many/Модель_{}.gltf", model), text);
		}
	};
	writeModels(0u);
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	(void)WriteBothSnapshots(pool, tree);

	SOpen writer;
	writer.bWriter = true;
	writer.bSnapshotAtShutDown = false;   // what it committed stays in the journal: the replay is checked below
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project(), writer);
	const FString folder[] = { "/Project/Models/Many" };
	for (uint32 round = 1u; round <= 12u && registry->GetCounters().Compactions == 0u; ++round)
	{
		writeModels(round);
		ASSERT_TRUE(registry->ScanPathsSynchronous(folder, 60.0));
	}
	EXPECT_GT(registry->GetCounters().Compactions, 0u);
	const SRun live = Close(registry);
	const FString fresh = FreshDump(pool, tree.Project());
	EXPECT_EQ(live.Dump, fresh);
	const SRun replayed = RunOnce(pool, tree.Project());
	EXPECT_EQ(replayed.Dump, fresh);
	EXPECT_EQ(replayed.Counters.Analyzed, 0u);
}

TEST(AssetRegistry, AWriterWritesItsSnapshotAfterABatchAndWhenIdle)
{
	const CServiceTree tree("Snapshot Triggers");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	for (const bool bBatch : { true, false })
	{
		SAssetRegistryDesc desc;
		desc.Mounts = tree.Project();
		desc.Pool = &pool;
		desc.bWriter = true;
		desc.bLoadSnapshots = false;
		desc.SnapshotBatchRecords = bBatch ? 1u : 1000000u;
		desc.SnapshotIdleSeconds = bBatch ? 3600.0 : 0.0;
		CAssetRegistry registry(std::move(desc));
		registry.Start();
		ASSERT_TRUE(registry.WaitForInitialScan(60.0));
		const uint64 deadline = platform::MakeDeadline(30.0);
		while (registry.GetCounters().SnapshotsWritten == 0u && platform::ReadTimestamp() < deadline)
		{
			pool.PumpCompletions();
			registry.Pump();
			pool.WaitForCompletions(0.05);
		}
		EXPECT_GT(registry.GetCounters().SnapshotsWritten, 0u) << (bBatch ? "after a batch" : "when idle");
		registry.ShutDown();
		std::error_code ec;
		EXPECT_TRUE(std::filesystem::exists(GetRegistrySnapshotPath(tree.Tree.ProjectDir()), ec));
		std::filesystem::remove(GetRegistrySnapshotPath(tree.Tree.ProjectDir()), ec);
	}
}

TEST(AssetRegistry, APassCutShortAfterItsConversionLeavesTheNextStartEverythingToRedo)
{
	const CServiceTree tree("Cut Short");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	(void)WriteBothSnapshots(pool, tree);
	// Changed files (Analyze), a new one, and a folder rule that switches the importer (another AnalyzeStamp).
	tree.WriteProjectPng("Textures/Камінь.png", 16u);
	tree.WriteProjectPng("Textures/Трава.png", 8u);
	tree.WriteProjectPng("Textures/Нова.png");
	frt::test::rules::WriteFile(tree.FolderRules(), "FormatVersion: 1\nRules:\n  - { Match: \"*.png\", Importer: Raw }\n");
	{
		SAssetRegistryDesc desc;
		desc.Mounts = tree.Project();
		desc.Pool = &pool;
		desc.bWriter = true;
		desc.AnalyzeBatchFiles = 1u;
		CAssetRegistry registry(std::move(desc));
		registry.Start();
		ASSERT_TRUE(PumpFramesUntil(pool, registry, [&] { return registry.GetScanState() != EScanState::LoadingSnapshots; }, 60.0));
		// The first commit after the snapshots' adoption is the project's conversion; its Analyze tasks were submitted
		// in the same frame, and ShutDown cancels them before any of their completions runs (a clean exit, or a crash
		// once the journal holds the conversion).
		const uint64 adopted = registry.GetCounters().Committed;
		ASSERT_TRUE(PumpFramesUntil(pool, registry, [&] { return registry.GetCounters().Committed > adopted; }, 60.0));
		EXPECT_FALSE(registry.IsInitialScanComplete());
		registry.ShutDown();
	}
	// The next start finds every file and rule edit still to do: what the half pass persisted claims nothing it did not
	// do.
	const FString fresh = FreshDump(pool, tree.Project());
	const SRun next = RunOnce(pool, tree.Project());
	EXPECT_EQ(next.Dump, fresh);
	SOpen writer;
	writer.bWriter = true;
	std::unique_ptr<CAssetRegistry> registry = Open(pool, tree.Project(), writer);
	EXPECT_EQ(TypeOf(registry->Live(), FString::Format("/Project/{}/Лист.png", Foliage)), asset_types::Raw);
	EXPECT_EQ(TypeOf(registry->Live(), "/Project/Textures/Нова.png"), asset_types::Texture);
	EXPECT_EQ(Close(registry).Dump, fresh);
}

TEST(AssetRegistry, ABrokenJournalIsRestartedByTheFullWriteItTriggersAndLosesNoCommit)
{
	const CServiceTree tree("Broken Journal");
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	(void)WriteBothSnapshots(pool, tree);
	SAssetRegistryDesc desc;
	desc.Mounts = tree.Project();
	desc.Pool = &pool;
	desc.bWriter = true;
	desc.bSnapshotAtShutDown = false;   // what follows the full write stays in the journal alone
	desc.SnapshotBatchRecords = 1000000u;
	desc.SnapshotIdleSeconds = 3600.0;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	ASSERT_TRUE(registry.WaitForInitialScan(60.0));
	CRegistryJournal* journal = SAssetRegistryTestAccess::GetJournal(registry);
	ASSERT_NE(journal, nullptr);

	// The next append is cut short: the journal breaks and a full write starts. A second commit lands while it runs or
	// right after it, a third once the journal is back.
	SRegistryJournalTestAccess::FailNextAppendAfter(*journal, 20u);
	tree.WriteProjectPng("Textures/Нова.png");
	const FString added[] = { "/Project/Textures/Нова.png" };
	ASSERT_TRUE(registry.ScanPathsSynchronous(added, 30.0));
	tree.WriteProjectPng("Textures/Трава.png", 8u);
	const FString modified[] = { "/Project/Textures/Трава.png" };
	ASSERT_TRUE(registry.ScanPathsSynchronous(modified, 30.0));
	EXPECT_TRUE(PumpFramesUntil(pool, registry, [&] { return registry.GetCounters().SnapshotsWritten > 0u && !journal->IsBroken(); }, 10.0))
		<< "the full write restarts the broken journal";
	tree.WriteProjectPng("Textures/Камінь.png", 16u);
	const FString later[] = { "/Project/Textures/Камінь.png" };
	ASSERT_TRUE(registry.ScanPathsSynchronous(later, 30.0));
	EXPECT_FALSE(journal->IsBroken());
	const FString live = DumpOf(registry.Live());
	registry.ShutDown();

	// The snapshot and the restarted journal hold every commit: a warm start with nothing to analyze.
	const SRun replayed = RunOnce(pool, tree.Project());
	EXPECT_EQ(replayed.Dump, live);
	EXPECT_EQ(replayed.Dump, FreshDump(pool, tree.Project()));
	EXPECT_EQ(replayed.Counters.Analyzed, 0u);
	const SSnapshotFileReport* report = FindReport(replayed, "AssetRegistry.snapshot");
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(report->bJournalUsed) << report->JournalMessage.CStr();
	EXPECT_GT(report->JournalCommits, 0u);
}

TEST(AssetRegistry, AMountRuleEditOverManyFilesCommitsInDeltasOfBoundedSize)
{
	const CServiceTree tree("Bounded Deltas");
	for (uint32 i = 0u; i < 700u; ++i)
	{
		tree.WriteProjectPng(FString::Format("Багато/Файл_{:03}.png", i));
	}
	jobs::CWorkerPool pool(MakePoolDesc(4u));
	(void)WriteBothSnapshots(pool, tree);
	// Every texture's labels change: every record of the pass is committed again, none analyzed.
	tree.WriteProjectRules(FString(ProjectRulesText) + "Rules:\n  - { Match: \"**/*.png\", Labels: [ Усі ] }\n");
	SAssetRegistryDesc desc;
	desc.Mounts = tree.Project();
	desc.Pool = &pool;
	desc.FrameBudgetMs = 0.0;   // one delta per Pump: each change set below is one delta's
	CAssetRegistry registry(std::move(desc));
	size_t largest = 0u;
	size_t changed = 0u;
	const SDelegateHandle handle = registry.OnAssetsChanged().Add([&] (const SAssetChangeSet& Changes)
	{
		if (!Changes.bRefreshAll)
		{
			const size_t size = Changes.Added.size() + Changes.Removed.size() + Changes.Modified.size();
			largest = std::max(largest, size);
			changed += size;
		}
	});
	registry.Start();
	ASSERT_TRUE(PumpFramesUntil(pool, registry, [&] { return registry.IsInitialScanComplete(); }, 120.0));
	registry.OnAssetsChanged().Remove(handle);
	EXPECT_EQ(registry.GetCounters().Analyzed, 0u);
	EXPECT_GE(changed, 700u);
	EXPECT_LE(largest, 256u) << "a frame commits a bounded delta, never a whole pass";
	const FString dump = DumpOf(registry.Live());
	registry.ShutDown();
	EXPECT_EQ(dump, FreshDump(pool, tree.Project()));
}
