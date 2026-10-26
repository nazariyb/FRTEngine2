// §5.11's "folder Assets.yml edit -> subtree re-evaluated (1k files): <= 2 ms + re-Analyze of changed stamps", end
// to end over the rules engine (D3) and the store (B3): the edited folder file is parsed and bound, every record in
// the folder's pre-order interval is re-evaluated in memory (ResolveFile over the store's files under the folder),
// and one delta commits the folder's new RulesHash and only the records whose rule outputs (labels, cook rule,
// chunk) changed, rebuilt from what the view holds - the way F3 re-evaluates without touching the disk or
// re-analyzing. No M3 settings field is Analyze-relevant, so no stamp changes and nothing is re-analyzed here.
// D3's own test (Rules/RulesScanTest.cpp) times the re-evaluation alone.
//
// Timings mean something in optimized builds only: Debug prints, never judges. The check fails on a clear
// regression (twice the budget) and holds the exact budget only with FRT_PERF_STRICT=1.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "Assets/Registry/RegistryTables.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"
#include "RegistryTestFixture.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::registry_test;
using frt::test::rules::CRulesTree;
using frt::test::rules::DescribeDiagnostics;


namespace
{
constexpr FStringLiteral EditedFolder = "/Project/Environment";

// The record of Record as an upsert again (tags, sub-entries, edges, companions as the view holds them), with the
// rule outputs of Settings: what F3 commits for a record whose rule outputs changed and whose stamp did not.
SRecordUpsert MakeUpsertFromView (const CRegistryView& View, SRecordId Record, const SEffectiveFileSettings& Settings)
{
	const SRegistryTables& tables = GetTables(View);
	const uint32 row = Record.Value;
	SAssetData data;
	View.GetRecord(Record, data);
	SRecordUpsert upsert;
	upsert.FileMountPath = data.MountPath;
	upsert.Guid = data.Id.Guid;
	upsert.Type = data.Type;
	upsert.Importer = FString(View.GetImporterName(Record));
	upsert.Flags = data.Flags & ~ERecordFlags::HasProblems;
	upsert.CookRule = Settings.CookRule;
	upsert.Chunk = Settings.Chunk;
	upsert.RuleLabels = Settings.RuleLabels;
	upsert.PrimaryName = tables.Assets.PrimaryName[row] != InvalidRow ? FString(tables.Names.Get(tables.Assets.PrimaryName[row])) : FString();
	upsert.ImportStatus = tables.Assets.ImportStatus[row];
	upsert.AnalyzeStamp = tables.Assets.AnalyzeStamp[row];
	View.ForEachTag(Record, [&] (FStringView Key, const STagValue& Value) { upsert.Tags.push_back(STagDecl { FString(Key), Value }); });
	View.ForEachSubEntry(Record, [&] (SRecordId Sub)
	{
		SAssetData subData;
		View.GetRecord(Sub, subData);
		SSubEntryDecl decl;
		decl.Label = subData.Name;
		decl.SubId = subData.Id.SubId;
		decl.Type = subData.Type;
		decl.Flags = subData.Flags & ~ERecordFlags::HasProblems;
		View.ForEachTag(Sub, [&] (FStringView Key, const STagValue& Value) { decl.Tags.push_back(STagDecl { FString(Key), Value }); });
		upsert.SubEntries.push_back(std::move(decl));
	});
	View.ForEachDependency(Record, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView& Edge)
	{
		SEdgeDecl decl;
		decl.FromSub = Edge.FromSub;
		decl.TargetPath = FString(Edge.TargetPath);
		decl.Flags = Edge.Flags & ~EDepFlags::Unresolved;
		decl.FieldPath = FString(Edge.FieldPath);
		upsert.Edges.push_back(std::move(decl));
	});
	View.ForEachCompanion(Record, [&] (FStringView Companion) { upsert.Companions.push_back(FString(Companion)); });
	return upsert;
}

// Whether the record's stored rule outputs differ from Settings.
bool RuleOutputsChanged (const CRegistryView& View, SRecordId Record, const SEffectiveFileSettings& Settings)
{
	const SRegistryTables& tables = GetTables(View);
	if (tables.Assets.CookRule[Record.Value] != Settings.CookRule || tables.Assets.Chunk[Record.Value] != Settings.Chunk)
	{
		return true;
	}
	uint32 index = 0u;
	bool bChanged = false;
	View.ForEachLabel(Record, [&] (FStringView Label, bool bRule)
	{
		bChanged = bChanged || !bRule || index >= Settings.RuleLabels.size() || Settings.RuleLabels[index].View() != Label;
		++index;
	});
	return bChanged || index != Settings.RuleLabels.size();
}
}

TEST(RegistryRulesEditBenchmark, AFolderRuleEditReevaluatesAThousandFilesAndCommitsOnlyTheChangedRecords)
{
	const bool bOptimized = !FRT_BUILD_DEBUG;
	const double slack = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1" ? 1.0 : 2.0;

	CRulesTree tree("Registry Edit");
	tree.WriteEngineRules("FormatVersion: 1\nScan:\n  Exclude: [ \"/Engine/Shaders/**\" ]\n  Quiet: [ \"**/README.md\" ]\nPresets:\n"
		"  Texture/Color:  { ColorSpace: Srgb,   Mips: Box, Compression: None }\n  Texture/Normal: { ColorSpace: Normal, Mips: Box, Compression: None }\n"
		"Rules:\n  - { Match: \"/Engine/Defaults/**\", Cook: Always }\n  - { Match: \"**/*.{png,jpg,jpeg,tga,bmp,psd,kra,ora}\", Preset: Texture/Color }\n");
	tree.WriteProjectRules("FormatVersion: 1\nScan:\n  Exclude: [ \"**/Reference/**\", \"Docs/**\" ]\n  Quiet: [ \"license.txt\", \"*.md\" ]\n"
		"Presets:\n  Texture/Mask: { ColorSpace: Linear, Mips: Box, Compression: None }\nRules:\n"
		"  - { Match: [ \"*_n.*\", \"*_normal.*\" ], Preset: Texture/Normal }\n  - { Match: [ \"*_orm.*\", \"*_mr.*\" ], Preset: Texture/Mask }\n"
		"  - { Match: \"Debug/**\", Cook: DevelopmentOnly }\n");
	const CMountTable mounts = tree.Resolve();
	const CRulesEngine engine(mounts);
	std::vector<SDiagnostic> diagnostics;
	const std::shared_ptr<const SScanScope> project = engine.OpenScope("/Project", diagnostics);
	ASSERT_TRUE(diagnostics.empty()) << DescribeDiagnostics(diagnostics).CStr();

	// The folder file before and after the edit: the edit adds a label to one of the twenty subfolders.
	constexpr FStringLiteral texts[] = {
		"FormatVersion: 1\nRules:\n  - { Match: \"*.{png,jpg}\", For: Texture, Settings: { MaxSize: 1024 }, Labels: [ Foliage ] }\n"
		"  - { Match: \"Тека 0*/**\", Chunk: 1 }\n",
		"FormatVersion: 1\nRules:\n  - { Match: \"*.{png,jpg}\", For: Texture, Settings: { MaxSize: 1024 }, Labels: [ Foliage ] }\n"
		"  - { Match: \"Тека 0*/**\", Chunk: 1 }\n  - { Match: \"Тека 05/**\", Labels: [ Hero ] }\n",
	};

	// 1000 files in 20 folders below the edited one: textures, glTF models with sub-entries and a buffer, legacy
	// materials with texture edges, and unclaimed text files (no record).
	constexpr FStringLiteral Extensions[] = { ".png", "_n.png", "_orm.png", ".jpg", ".gltf", ".frtmat.yml", ".txt", ".md" };
	std::vector<FString> files;
	for (uint32 i = 0u; i < 1000u; ++i)
	{
		files.push_back(FString::Format("{}/Тека {:02}/Файл_{}{}", EditedFolder, i / 50u, i, Extensions[i % std::size(Extensions)]));
	}
	CRegistryStore store;
	{
		uint64 rulesHash = 0u;
		std::vector<SDiagnostic> bind;
		const std::shared_ptr<const SScanScope> scope = engine.EnterFolderFromText(project, EditedFolder, texts[0].View(), rulesHash, bind);
		ASSERT_TRUE(bind.empty()) << DescribeDiagnostics(bind).CStr();
		SRegistryDelta delta = MakeMountsDelta();
		delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", FString(EditedFolder), rulesHash });
		for (uint32 i = 0u; i < files.size(); ++i)
		{
			const FString& path = files[i];
			const SEffectiveFileSettings settings = engine.ResolveFile(*scope, path, {}, false);
			delta.FileUpserts.push_back(MakeFile(path, 1000u + i, settings.Class));
			if (settings.Importer == nullptr)
			{
				continue;
			}
			const FStringView extension = Extensions[i % std::size(Extensions)];
			SRecordUpsert record = MakeRecord(path, settings.Importer->GetMainType(path), settings.Importer->Name);
			record.CookRule = settings.CookRule;
			record.Chunk = settings.Chunk;
			record.RuleLabels = settings.RuleLabels;
			if (extension == ".gltf")
			{
				const FString bin = FString::Format("{}/Тека {:02}/Файл_{}.bin", EditedFolder, i / 50u, i);
				delta.FileUpserts.push_back(MakeFile(bin, 50000u, EFileClass::Unclaimed));
				record.Tags = { MakeIntTag("MeshCount", 1), MakeIntTag("VertexCount", 3000 + i) };
				record.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 3000 + i) } });
				record.SubEntries.push_back(SSubEntryDecl { "material:Skin", 0u, asset_types::Material, ERecordFlags::None, {} });
				record.Edges.push_back(MakePathEdge(bin, EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
				record.Companions = { bin };
			}
			else if (extension == ".frtmat.yml")
			{
				for (uint32 k = 1u; k <= 3u; ++k)
				{
					record.Edges.push_back(MakePathEdge(files[i - k], EDepFlags::Build | EDepFlags::Path, "albedo.texture"));
				}
				record.Flags = ERecordFlags::LegacyReferences;
			}
			else
			{
				record.Tags = { MakeIntTag("Width", 1024), MakeIntTag("Height", 512), MakeIntTag("Channels", 4), STagDecl { "HasAlpha", STagValue::FromBool(true) } };
			}
			delta.RecordUpserts.push_back(std::move(record));
		}
		store.Commit(std::move(delta));
	}
	const std::optional<uint32> folder = store.Live().FindFolder(EditedFolder);
	ASSERT_TRUE(folder.has_value());
	const FString before = Dump(store.Live());

	// One edit: bind the new text, re-evaluate the interval, commit the changed records. Alternately to the edited
	// text and back, the best of three each way.
	const auto edit = [&] (FStringView Text, uint32& OutChanged, uint32& OutEvaluated) -> double
	{
		const uint64 start = platform::ReadTimestamp();
		uint64 rulesHash = 0u;
		std::vector<SDiagnostic> bind;
		const std::shared_ptr<const SScanScope> scope = engine.EnterFolderFromText(project, EditedFolder, Text, rulesHash, bind);
		EXPECT_TRUE(bind.empty());
		const CRegistryView& live = store.Live();
		const SRegistryTables& tables = GetTables(live);
		SRegistryDelta delta;
		delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", FString(EditedFolder), rulesHash });
		OutChanged = 0u;
		OutEvaluated = 0u;
		live.ForEachFileUnder(*folder, [&] (uint32 FileRow)
		{
			SFileData file;
			live.GetFile(FileRow, file);
			if (!file.Record.IsValid())
			{
				return;   // no record: an unclaimed file (RULE002 is F3's, after companions) or a companion
			}
			SFileFacts facts;
			facts.Type = tables.Assets.Type[file.Record.Value];
			const SEffectiveFileSettings settings = engine.ResolveFile(*scope, file.MountPath, facts, false);
			++OutEvaluated;
			if (RuleOutputsChanged(live, file.Record, settings))
			{
				delta.RecordUpserts.push_back(MakeUpsertFromView(live, file.Record, settings));
				++OutChanged;
			}
		});
		store.Commit(std::move(delta));
		return platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	};
	double best = 1e30;
	uint32 changed = 0u;
	uint32 evaluated = 0u;
	for (uint32 round = 0u; round < 6u; ++round)
	{
		uint32 roundChanged = 0u;
		const double ms = edit(texts[(round + 1u) % 2u].View(), roundChanged, evaluated);
		best = (std::min)(best, ms);
		changed = (std::max)(changed, roundChanged);
		EXPECT_GT(roundChanged, 0u);
		EXPECT_LT(roundChanged, 100u) << "only the subfolder's records change";
	}
	EXPECT_EQ(Dump(store.Live()), before) << "back at the first text, the registry is as it was";
	EXPECT_GE(evaluated, 700u);
	std::printf("[registry] folder Assets.yml edit: %u records re-evaluated, %u committed, %.3f ms (budget 2.000 ms; best of 6)\n", evaluated,
		changed, best);
	RecordProperty("FolderEditCommitMs", FString::Format("{:.3f}", best).CStr());
	if (bOptimized)
	{
		EXPECT_LE(best, 2.0 * slack) << "FolderEditCommitMs";
	}
}
