// The M3 pipeline end to end over the Demo template, the contract F3 builds on: a temp copy of the engine's content,
// Core/Config/Assets.yml and Templates/Demo goes through CMountTable -> CRulesEngine -> CMountScanner -> (each file's
// committed meta, M4: its GUID, values and hash) -> ResolveFile ->
// AnalyzeFile -> AppendRecordUpsert -> CRegistryStore::Commit, with the companions known before RULE002 is computed,
// as F3 will run it. Then: no RULE001 or RULE002, no error, no unresolved edge; the records per type are what the
// files on disk say; two scans (different thread counts) dump identically; the engine and project snapshots written
// and loaded back dump the same; and text queries answer what the files say.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <vector>

#include "../Discovery/DiscoveryTestUtils.h"
#include "TemplateTestUtils.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTypes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Discovery/MountScanner.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/Importers/AssetImporter.h"
#include "Assets/Pipeline/Rules.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/RegistryStore.h"
#include "Assets/Registry/RegistryView.h"
#include "Assets/Registry/Snapshot.h"
#include "Containers/String.h"
#include "EnginePaths.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Timestamp.h"

using namespace frt;
using namespace frt::assets;
using frt::test::discovery::CDiscoveryTree;


namespace
{
using frt::test::registry::CTemplateCopy;

struct SPipelineRun
{
	std::unique_ptr<CRegistryStore> Store = std::make_unique<CRegistryStore>();
	std::unique_ptr<CRulesEngine> Rules;
	std::vector<SDiagnostic> Diagnostics;       // everything the producers reported, for the checks
	uint32 Files = 0u;
	uint32 Analyzed = 0u;
	double Milliseconds = 0.0;
};

// Groups diagnostics by Subject into one SProblemsSet each, under Source.
void AddProblems (SRegistryDelta& Delta, EProblemSource Source, const std::vector<SDiagnostic>& Diagnostics)
{
	std::map<FString, std::vector<SDiagnostic>, std::less<>> bySubject;
	for (const SDiagnostic& diagnostic : Diagnostics)
	{
		bySubject[diagnostic.Subject].push_back(diagnostic);
	}
	for (auto& [subject, list] : bySubject)
	{
		Delta.Problems.push_back(SProblemsSet { subject, Source, std::move(list) });
	}
}

/**
 * One cold run of the M3 pipeline as F3 runs it: rules, a scan per mount, per file the effective settings and
 * Analyze (Types filter applied), one commit, then RULE002 for the files still Unclaimed once companions are known.
 */
SPipelineRun RunPipeline (const CMountTable& Mounts, uint32 Threads)
{
	SPipelineRun run;
	const uint64 start = platform::ReadTimestamp();
	run.Rules = std::make_unique<CRulesEngine>(Mounts);
	CRulesEngine& rules = *run.Rules;
	rules.LoadMountRules(&run.Diagnostics);

	jobs::SWorkerPoolDesc poolDesc;
	poolDesc.ThreadCount = Threads;
	poolDesc.Name = "Demo Pipeline";
	jobs::CWorkerPool pool(poolDesc);

	SRegistryDelta delta;
	for (const SMount& mount : Mounts.GetMounts())
	{
		delta.Mounts.push_back(SMountUpsert { mount.Name, mount.Volume.bCaseSensitive, mount.Id });
		if (const uint64 hash = rules.GetMountRulesHash(mount.Name); hash != 0u)
		{
			delta.RuleFileUpserts.push_back(SRuleFileUpsert { mount.Name, FString(), hash });
		}
	}
	std::vector<std::pair<FString, bool>> unclaimedCandidates;   // path, quiet
	for (const SMount& mount : Mounts.GetMounts())
	{
		const SScanBatch batch = CMountScanner::ScanBlocking(pool, mount, mount.GetRootMountPath(), rules, nullptr);
		run.Diagnostics.insert(run.Diagnostics.end(), batch.Diagnostics.begin(), batch.Diagnostics.end());
		run.Diagnostics.insert(run.Diagnostics.end(), batch.RuleDiagnostics.begin(), batch.RuleDiagnostics.end());
		EXPECT_TRUE(batch.FailedFolders.empty()) << mount.Name.CStr();
		AddProblems(delta, EProblemSource::Scan, batch.Diagnostics);
		AddProblems(delta, EProblemSource::Rules, batch.RuleDiagnostics);
		for (const SScanFolder& folder : batch.Folders)
		{
			delta.FolderUpserts.push_back(SFolderUpsert { folder.MountPath });
			if (folder.bHasRuleFile)
			{
				delta.RuleFileUpserts.push_back(SRuleFileUpsert { mount.Name, folder.MountPath, folder.RulesHash });
			}
		}
		// Each file's meta (M4): its own FILES row; its GUID, values and hash go to its source's record and row. A meta
		// no file takes is an orphan's row.
		std::vector<uint32> metaOfFile(batch.Files.size(), ~0u);
		for (uint32 m = 0u; m < batch.Metas.size(); ++m)
		{
			const SScanMeta& meta = batch.Metas[m];
			const FString metaPath = batch.Folders[meta.Folder].MountPath + "/" + meta.Name;
			const bool bTaken = meta.SourceFile != ~0u && metaOfFile[meta.SourceFile] == ~0u;
			if (bTaken)
			{
				metaOfFile[meta.SourceFile] = m;
			}
			SFileUpsert row;
			row.MountPath = metaPath;
			row.Size = meta.Size;
			row.MTime = meta.MTime;
			row.Identity = meta.Identity;
			row.Class = EFileClass::Meta;
			row.Meta = SFileMetaState { bTaken ? EMetaState::Paired : EMetaState::Orphan, 0u };
			delta.FileUpserts.push_back(std::move(row));
		}
		for (uint32 index = 0u; index < batch.Files.size(); ++index)
		{
			const SScanFile& file = batch.Files[index];
			const SScanFolder& folder = batch.Folders[file.Folder];
			const FString path = folder.MountPath + "/" + file.Name;
			std::optional<SMetaParseResult> meta;
			uint64 metaHash = 0u;
			if (metaOfFile[index] != ~0u)
			{
				const SScanMeta& scanned = batch.Metas[metaOfFile[index]];
				const FString metaPath = batch.Folders[scanned.Folder].MountPath + "/" + scanned.Name;
				FStringView mountName;
				FStringView relative;
				EXPECT_TRUE(SplitMountPath(metaPath, mountName, relative)) << metaPath.CStr();
				const std::optional<std::filesystem::path> absolute = mount.ToAbsolute(relative);
				const std::optional<std::vector<uint8>> bytes = absolute ? platform::ReadFileBytesUncached(*absolute) : std::nullopt;
				EXPECT_TRUE(bytes.has_value()) << metaPath.CStr();
				if (bytes.has_value())
				{
					const FStringView text(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size()));
					ParseMeta(text, meta.emplace());
					metaHash = HashMetaBytes(text);
					EXPECT_EQ(meta->State, EMetaParseState::Ok) << metaPath.CStr();
					for (SFileUpsert& row : delta.FileUpserts)
					{
						if (row.MountPath == metaPath)
						{
							row.Meta = SFileMetaState { meta->State == EMetaParseState::Ok ? EMetaState::Paired : EMetaState::Broken, metaHash };
						}
					}
				}
			}
			const SAssetMeta* metaValues = meta.has_value() && meta->State == EMetaParseState::Ok ? &meta->Meta : nullptr;
			const SEffectiveFileSettings settings = rules.ResolveFile(*folder.Scope, path, SFileFacts { reflect::InvalidTypeId, metaValues }, false);
			SFileUpsert row;
			row.MountPath = path;
			row.Size = file.Size;
			row.MTime = file.MTime;
			row.Identity = file.Identity;
			row.Class = settings.Class;
			row.ContentHash = hash::SContentHash {};   // changed, not hashed (F3 hashes lazily)
			if (meta.has_value())
			{
				row.Meta = SFileMetaState { metaValues != nullptr ? EMetaState::Paired : EMetaState::Broken, metaHash };
			}
			++run.Files;
			if (settings.Importer != nullptr && settings.Class != EFileClass::Excluded)
			{
				const std::optional<SAnalyzeInput> input = SAnalyzeInput::FromMount(mount, path);
				EXPECT_TRUE(input.has_value()) << path.CStr();
				if (input.has_value())
				{
					SAnalyzeResult result;
					(void)AnalyzeFile(*input, settings, result);
					run.Analyzed += result.Status == EImportStatus::Analyzed ? 1u : 0u;
					if (result.ContentHash.has_value())
					{
						row.ContentHash = result.ContentHash;
					}
					RemoveSubEntries(result, path, [&] (reflect::TypeId Type) { return rules.IsTypeIncluded(*folder.Scope, Type); });
					run.Diagnostics.insert(run.Diagnostics.end(), result.Diagnostics.begin(), result.Diagnostics.end());
					AppendRecordUpsert(std::move(result), settings, path, delta, metaValues != nullptr ? metaValues->Guid : SGuid {}, metaValues);
				}
			}
			else if (settings.Class == EFileClass::Unclaimed)
			{
				unclaimedCandidates.emplace_back(path, file.Verdict.bQuiet);
			}
			delta.FileUpserts.push_back(std::move(row));
		}
	}
	run.Store->Commit(std::move(delta));

	// RULE002 once companions are known: the candidates the commit left Unclaimed (a glTF's .bin is a Companion now).
	const CRegistryView& view = run.Store->Live();
	std::vector<SUnclaimedFile> unclaimed;
	for (const auto& [path, bQuiet] : unclaimedCandidates)
	{
		SFileData data;
		if (const std::optional<uint32> row = view.FindFileExact(path); row && view.GetFile(*row, data) && data.Class == EFileClass::Unclaimed)
		{
			unclaimed.push_back(SUnclaimedFile { path.View(), bQuiet });
		}
	}
	const std::vector<SDiagnostic> rule002 = ReportUnclaimedFiles(unclaimed);
	run.Diagnostics.insert(run.Diagnostics.end(), rule002.begin(), rule002.end());
	SRegistryDelta unclaimedDelta;
	AddProblems(unclaimedDelta, EProblemSource::Unclaimed, rule002);
	run.Store->Commit(std::move(unclaimedDelta));
	run.Milliseconds = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	return run;
}

FString DumpOf (const CRegistryView& View)
{
	FString text;
	View.DumpText(text);
	return text;
}

FString Describe (const std::vector<SDiagnostic>& Diagnostics)
{
	FString text;
	for (const SDiagnostic& diagnostic : Diagnostics)
	{
		text.AppendFormat("\n  {} {} {}: {}", FormatDiagnosticCode(diagnostic.Code), GetSeverityName(diagnostic.Severity), diagnostic.Subject,
			diagnostic.Message);
	}
	return text;
}

// What the copied files say: per type, how many files claim it by extension (the Demo has no rule that changes an
// importer), and how many glTF buffers there are.
struct SExpected
{
	std::map<reflect::TypeId, uint32> Records;
	uint32 Buffers = 0u;
};

SExpected CountByExtension (const CTemplateCopy& Copy)
{
	SExpected expected;
	const auto count = [&] (const std::filesystem::path& Root, bool bEngine)
	{
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(Root, ec), end; it != end; it.increment(ec))
		{
			const FString relative = FString::FromPath(std::filesystem::relative(it->path(), Root, ec), EPathStyle::Generic);
			if (bEngine && relative.StartsWith("Shaders"))
			{
				it.disable_recursion_pending();   // Core/Config/Assets.yml excludes /Engine/Shaders/**
				continue;
			}
			if (it->is_directory(ec))
			{
				continue;
			}
			const FString name = FString::FromPath(it->path().filename(), EPathStyle::Generic);
			if (name.View().EndsWith<ESearchCase::IgnoreCase>(".bin"))
			{
				++expected.Buffers;
				continue;
			}
			if (bEngine && relative.StartsWith("Fonts/") && (name.View().EndsWith<ESearchCase::IgnoreCase>(".ttf")
				|| name.View().EndsWith<ESearchCase::IgnoreCase>(".otf")))
			{
				++expected.Records[asset_types::Raw];   // Scan.Raw in the engine file
				continue;
			}
			if (const SImporterDesc* importer = FindImporterForFile(name); importer != nullptr)
			{
				++expected.Records[importer->GetMainType(name)];
			}
		}
	};
	count(Copy.Tree.EngineContent(), true);
	count(Copy.Tree.ProjectContent(), false);
	return expected;
}

std::set<FString> FilesOf (const CRegistryView& View, const std::vector<SRecordId>& Records)
{
	std::set<FString> files;
	for (const SRecordId record : Records)
	{
		SAssetData data;
		if (View.GetRecord(record, data))
		{
			const uint32 hash = data.MountPath.View().Find('#');
			files.insert(FString(hash == FStringView::NotFound ? data.MountPath.View() : data.MountPath.View().Left(hash)));
		}
	}
	return files;
}
}


TEST(DemoPipelineTest, TheDemoTemplateGoesThroughEveryM3PieceAsTheFilesSay)
{
	const CTemplateCopy copy("DemoPipeline");
	ASSERT_EQ(copy.Mounts.GetMounts().size(), 2u) << Describe(copy.MountDiagnostics).CStr();
	EXPECT_TRUE(copy.MountDiagnostics.empty()) << Describe(copy.MountDiagnostics).CStr();

	SPipelineRun run = RunPipeline(copy.Mounts, 4u);
	const CRegistryView& view = run.Store->Live();
	std::printf("[ PERF     ] Demo pipeline, cold: %u files, %u analyzed, %.1f ms (4 threads)\n", run.Files, run.Analyzed, run.Milliseconds);

	// No rule problem, nothing unclaimed that is not quiet, no error anywhere.
	std::vector<SDiagnostic> bad;
	for (const SDiagnostic& diagnostic : run.Diagnostics)
	{
		if (diagnostic.Code == Rule001 || diagnostic.Code == Rule002 || diagnostic.Severity == ESeverity::Error)
		{
			bad.push_back(diagnostic);
		}
	}
	EXPECT_TRUE(bad.empty()) << Describe(bad).CStr();
	view.ForEachProblem([&] (FStringView Subject, EProblemSource Source, const SDiagnostic& Diagnostic)
	{
		EXPECT_NE(Source, EProblemSource::Registry) << Subject.GetData() << ": " << Diagnostic.Message.CStr();
		EXPECT_NE(Diagnostic.Code, Rule001);
		EXPECT_NE(Diagnostic.Code, Rule002);
	});

	// Every edge resolves (a glTF's buffer is a companion, never an edge).
	std::vector<FString> unresolved;
	std::map<reflect::TypeId, uint32> records;
	view.ForEachRecord([&] (SRecordId Record)
	{
		SAssetData data;
		ASSERT_TRUE(view.GetRecord(Record, data));
		++records[data.Type];
		view.ForEachDependency(Record, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge)
		{
			if ((Edge.Flags & EDepFlags::Unresolved) != EDepFlags {})
			{
				unresolved.push_back(data.MountPath + " -> " + Edge.TargetPath);
			}
		});
	});
	EXPECT_TRUE(unresolved.empty()) << unresolved.size() << " unresolved, e.g. " << (unresolved.empty() ? "" : unresolved[0].CStr());

	// The records per type are the files that claim it, and every buffer is a companion.
	const SExpected expected = CountByExtension(copy);
	for (const reflect::TypeId type : { asset_types::Texture, asset_types::Model, asset_types::Material, asset_types::InputActionMap,
		asset_types::InputAction, asset_types::Raw })
	{
		EXPECT_EQ(records[type], expected.Records.contains(type) ? expected.Records.at(type) : 0u) << CAssetTypeRegistry::Get(type).Name.GetData();
	}
	EXPECT_GT(records[asset_types::Texture], 0u);
	EXPECT_EQ(records[asset_types::Model], 6u);
	uint32 total = 0u;
	for (const auto& [type, count] : expected.Records)
	{
		total += count;
	}
	EXPECT_EQ(view.GetRecordCount(), total);
	uint32 companions = 0u;
	view.ForEachFile([&] (uint32 Row)
	{
		SFileData data;
		companions += view.GetFile(Row, data) && data.Class == EFileClass::Companion ? 1u : 0u;
	});
	EXPECT_EQ(companions, expected.Buffers);
	EXPECT_EQ(companions, 6u);

	// A second scan, on another thread count, dumps byte for byte the same.
	const FString dump = DumpOf(view);
	EXPECT_TRUE(dump.View().IsValidUtf8());
	{
		SPipelineRun again = RunPipeline(copy.Mounts, 1u);
		EXPECT_EQ(DumpOf(again.Store->Live()), dump);
	}

	// The engine's and the project's snapshots, written and loaded back together, dump the same.
	const std::filesystem::path engineSnapshot = copy.Tree.EngineDir() / "Local" / "Registry" / "Engine.snapshot";
	const std::filesystem::path projectSnapshot = copy.Tree.ProjectDir() / "Local" / "Assets" / "AssetRegistry.snapshot";
	const SGuid engineId = SGuid::NewRandom();
	const SGuid projectId = SGuid::NewRandom();
	const MountId engineMounts[] = { EngineMountId };
	const MountId projectMounts[] = { ProjectMountId };
	FString error;
	ASSERT_TRUE(WriteSnapshot(view, engineMounts, engineSnapshot, engineId, run.Rules->GetRulesBasis(false), &error)) << error.CStr();
	ASSERT_TRUE(WriteSnapshot(view, projectMounts, projectSnapshot, projectId, run.Rules->GetRulesBasis(true), &error)) << error.CStr();
	std::vector<SMountUpsert> mountUpserts;
	for (const SMount& mount : copy.Mounts.GetMounts())
	{
		mountUpserts.push_back(SMountUpsert { mount.Name, mount.Volume.bCaseSensitive, mount.Id });
	}
	const SSnapshotSource sources[] = {
		SSnapshotSource { engineSnapshot, engineId, false, run.Rules->GetRulesBasis(false) },
		SSnapshotSource { projectSnapshot, projectId, false, run.Rules->GetRulesBasis(true) },
	};
	CRegistryStore loaded;
	const SSnapshotLoadResult load = LoadSnapshots(loaded, sources, mountUpserts);
	ASSERT_EQ(load.Files.size(), 2u);
	for (const SSnapshotFileReport& file : load.Files)
	{
		EXPECT_EQ(file.State, ESnapshotFileState::Loaded) << file.Message.CStr();
		EXPECT_TRUE(file.DroppedSections.empty()) << file.Message.CStr();
		EXPECT_FALSE(file.bRuleFilesDropped);
		EXPECT_EQ(file.StaleRecords, 0u);
	}
	EXPECT_EQ(DumpOf(loaded.Live()), dump);

	// Text queries answer what the files say.
	std::vector<SRecordId> found;
	SDiagnostic queryError;
	EXPECT_EQ(view.RunText("type:Texture", found, &queryError), std::optional<uint32>(records[asset_types::Texture])) << queryError.Message.CStr();
	EXPECT_EQ(view.RunText("type:Model", found, &queryError), std::optional<uint32>(6u)) << queryError.Message.CStr();
	EXPECT_EQ(FilesOf(view, found).size(), 6u);
	EXPECT_EQ(view.RunText("type:InputActionMap", found, &queryError), std::optional<uint32>(records[asset_types::InputActionMap]));

	// ref: a texture - exactly the sources whose text names it (the glTF's image URI, the legacy material's texture).
	constexpr FStringLiteral Texture = "/Project/Models/Duck/DuckCM.png";
	std::set<FString> naming;
	{
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(copy.Tree.ProjectContent(), ec), end; it != end; it.increment(ec))
		{
			const FString name = FString::FromPath(it->path().filename(), EPathStyle::Generic);
			if (!name.View().EndsWith<ESearchCase::IgnoreCase>(".gltf") && !name.View().EndsWith<ESearchCase::IgnoreCase>(".frtmat.yml"))
			{
				continue;
			}
			const std::optional<FString> text = platform::ReadFileText(it->path());
			if (text.has_value() && text->View().Contains<ESearchCase::IgnoreCase>(FStringView("DuckCM.png")))
			{
				naming.insert(*copy.Mounts.ToMountPath(it->path()));
			}
		}
	}
	ASSERT_FALSE(naming.empty());
	EXPECT_TRUE(view.RunText(FString::Format("ref:{}", Texture).View(), found, &queryError).has_value()) << queryError.Message.CStr();
	EXPECT_EQ(FilesOf(view, found), naming);
	ASSERT_TRUE(view.RunText("dep:/Project/Models/Duck/Duck.gltf", found, &queryError).has_value()) << queryError.Message.CStr();
	EXPECT_TRUE(FilesOf(view, found).contains(FString(Texture)));
}

// The registry service (Assets/AssetRegistry.h) runs this pipeline with Analyze on workers, per-batch commits and
// RULE002 per folder after the companions: a cold start of it commits exactly what the reference sequence above does.
TEST(DemoPipelineTest, TheRegistryServiceCommitsWhatTheReferencePipelineCommits)
{
	const CTemplateCopy copy("DemoService");
	const SPipelineRun reference = RunPipeline(copy.Mounts, 4u);

	jobs::SWorkerPoolDesc poolDesc;
	poolDesc.ThreadCount = 4u;
	poolDesc.Name = "Demo Service";
	jobs::CWorkerPool pool(poolDesc);
	SAssetRegistryDesc desc;
	desc.Mounts = std::make_shared<const CMountTable>(copy.Mounts);
	desc.Pool = &pool;
	CAssetRegistry registry(std::move(desc));
	registry.Start();
	ASSERT_TRUE(registry.WaitForInitialScan(120.0));
	EXPECT_EQ(DumpOf(registry.Live()), DumpOf(reference.Store->Live()));
	EXPECT_EQ(registry.GetCounters().Analyzed, reference.Analyzed);
	registry.ShutDown();
	// A reader writes nothing: no Local/ appears in the project or the engine copy.
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(copy.Tree.ProjectDir() / "Local", ec));
	EXPECT_FALSE(std::filesystem::exists(copy.Tree.EngineDir() / "Local", ec));
}
