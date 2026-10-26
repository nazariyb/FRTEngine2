// The registry's text dump (design §5.3): equal content dumps identically whatever the commit order, the
// batching, a compaction or an image round trip, and nothing machine-specific (times, identities, content
// hashes, revisions, the volume's case rule) reaches it. One small registry is pinned to its exact text so
// a format change is a deliberate edit here. A mount filter dumps exactly those mounts' sections, and the JSON
// lines carry the text dump's content in its order.
#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "Assets/AssetDiagnosticCodes.h"
#include "Reflect/DataTreeJson.h"
#include "RegistryTestFixture.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::registry_test;


namespace
{
// One delta per file: the content of a small project, in a fixed order.
std::vector<SRegistryDelta> MakeContent ()
{
	std::vector<SRegistryDelta> deltas;
	const auto add = [&] (auto&& Fill)
	{
		SRegistryDelta delta;
		Fill(delta);
		deltas.push_back(std::move(delta));
	};
	add([] (SRegistryDelta& D) { AddTexture(D, "/Project/Textures/Rock.png", 512, 512); });
	add([] (SRegistryDelta& D) { AddTexture(D, "/Project/Textures/rock_n.png", 512, 512); });
	add([] (SRegistryDelta& D) { AddTexture(D, "/Engine/Textures/White.png", 1, 1); });
	add([] (SRegistryDelta& D) { AddMaterial(D, "/Project/Materials/Rock.frtmat.yml", { "/Project/Textures/Rock.png", "/Project/textures/rock_n.png" }); });
	add([] (SRegistryDelta& D) { AddMaterial(D, "/Project/Materials/Missing.frtmat.yml", { "/Project/Textures/None.png" }); });
	add([] (SRegistryDelta& D)
	{
		D.FileUpserts.push_back(MakeFile("/Project/Models/Hero.gltf", 2000u));
		SRecordUpsert model = MakeRecord("/Project/Models/Hero.gltf", asset_types::Model, "Model.Gltf");
		model.Tags = { MakeIntTag("MeshCount", 2), MakeIntTag("TriangleCount", 1000) };
		model.SubEntries.push_back(SSubEntryDecl { "mesh:Legs", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 20) } });
		model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 10) } });
		model.SubEntries.push_back(SSubEntryDecl { "image:#0", 0u, asset_types::Texture, ERecordFlags::None, {} });
		SEdgeDecl image = MakePathEdge("/Project/Textures/Rock.png", EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "images[0].uri");
		image.FromSub = MakeSubId("image:#0");
		model.Edges = { MakePathEdge("/Project/Models/Hero.bin", EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"), image };
		model.Companions = { "/Project/Models/Hero.bin" };
		model.RuleLabels = { "Characters" };
		model.AnalyzeStamp = 0xABCDEFu;
		D.RecordUpserts.push_back(std::move(model));
	});
	add([] (SRegistryDelta& D) { D.FileUpserts.push_back(MakeFile("/Project/Models/Hero.bin", 4000u, EFileClass::Unclaimed)); });
	add([] (SRegistryDelta& D)
	{
		D.FileUpserts.push_back(MakeFile("/Project/Input/Player.frtinputmap"));
		SRecordUpsert map = MakeRecord("/Project/Input/Player.frtinputmap", asset_types::InputActionMap, "Input.Legacy");
		map.Tags = { STagDecl { "Actions", STagValue::FromName("Move") }, STagDecl { "Actions", STagValue::FromName("Jump") } };
		map.Flags = ERecordFlags::Primary | ERecordFlags::LegacyReferences;
		map.PrimaryName = "Player";
		map.Edges = { MakePathEdge("/Project/Input/Move.frtinputaction", EDepFlags::Path), MakePathEdge("/Project/Input/Jump.frtinputaction", EDepFlags::Path) };
		D.RecordUpserts.push_back(std::move(map));
	});
	add([] (SRegistryDelta& D)
	{
		D.FileUpserts.push_back(MakeFile("/Project/Input/Jump.frtinputaction"));
		SRecordUpsert action = MakeRecord("/Project/Input/Jump.frtinputaction", asset_types::InputAction, "Input.Legacy");
		action.Tags = { STagDecl { "ValueType", STagValue::FromName("Button") } };
		D.RecordUpserts.push_back(std::move(action));
	});
	add([] (SRegistryDelta& D) { D.FileUpserts.push_back(MakeFile("/Project/readme.txt", 7u, EFileClass::Unclaimed)); });
	add([] (SRegistryDelta& D)
	{
		D.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "", 0x1111u });
		D.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/Textures", 0x2222u });
		D.FolderUpserts.push_back(SFolderUpsert { "/Project/Empty" });
	});
	add([] (SRegistryDelta& D)
	{
		SDiagnostic warning;
		warning.Severity = ESeverity::Warning;
		warning.Code = Rule002;
		warning.Subject = "/Project/readme.txt";
		warning.Message = "unclaimed file '.txt' is not in Scan.Quiet";
		D.Problems.push_back(SProblemsSet { "/Project/readme.txt", EProblemSource::Rules, { warning } });
	});
	return deltas;
}

FString DumpOf (const std::vector<SRegistryDelta>& Content, std::span<const uint32> Order, bool bOneDelta)
{
	CRegistryStore store;
	store.Commit(MakeMountsDelta());
	if (bOneDelta)
	{
		SRegistryDelta all;
		for (const uint32 index : Order)
		{
			SRegistryDelta part = Content[index];
			all.FileUpserts.insert(all.FileUpserts.end(), part.FileUpserts.begin(), part.FileUpserts.end());
			all.FolderUpserts.insert(all.FolderUpserts.end(), part.FolderUpserts.begin(), part.FolderUpserts.end());
			all.RuleFileUpserts.insert(all.RuleFileUpserts.end(), part.RuleFileUpserts.begin(), part.RuleFileUpserts.end());
			all.RecordUpserts.insert(all.RecordUpserts.end(), part.RecordUpserts.begin(), part.RecordUpserts.end());
			all.Problems.insert(all.Problems.end(), part.Problems.begin(), part.Problems.end());
		}
		store.Commit(std::move(all));
	}
	else
	{
		for (const uint32 index : Order)
		{
			store.Commit(SRegistryDelta(Content[index]));
		}
	}
	return Dump(store.Live());
}
}

TEST(RegistryDumpTest, EqualContentDumpsIdenticallyInAnyCommitOrder)
{
	const std::vector<SRegistryDelta> content = MakeContent();
	std::vector<uint32> order(content.size());
	for (uint32 i = 0u; i < order.size(); ++i)
	{
		order[i] = i;
	}
	const FString reference = DumpOf(content, order, false);
	EXPECT_EQ(DumpOf(content, order, true), reference);
	std::reverse(order.begin(), order.end());
	EXPECT_EQ(DumpOf(content, order, false), reference);
	std::mt19937 random(1234u);
	for (uint32 attempt = 0u; attempt < 8u; ++attempt)
	{
		std::shuffle(order.begin(), order.end(), random);
		EXPECT_EQ(DumpOf(content, order, false), reference) << "attempt " << attempt;
		EXPECT_EQ(DumpOf(content, order, true), reference) << "attempt " << attempt;
	}
}

TEST(RegistryDumpTest, MachineSpecificColumnsNeverReachTheDump)
{
	const auto build = [] (int64 MTime, uint8 IdentityByte, bool bCaseSensitive, uint32 ExtraCommits)
	{
		CRegistryStore store;
		SRegistryDelta mounts;
		mounts.Mounts.push_back(MakeMount("Engine", EngineMountId, bCaseSensitive));
		mounts.Mounts.push_back(MakeMount("Project", ProjectMountId, bCaseSensitive));
		store.Commit(std::move(mounts));
		for (uint32 i = 0u; i < ExtraCommits; ++i)
		{
			store.Commit(SRegistryDelta {});
		}
		SRegistryDelta delta;
		SFileUpsert file = MakeFile("/Project/a.png", 10u);
		file.MTime = MTime;
		file.Identity.Bytes[0] = IdentityByte;
		file.ContentHash = hash::HashContent(std::span<const uint8>(&IdentityByte, 1u));
		file.MetaHash = IdentityByte;
		delta.FileUpserts.push_back(file);
		delta.RecordUpserts.push_back(MakeRecord("/Project/a.png"));
		store.Commit(std::move(delta));
		return Dump(store.Live());
	};
	EXPECT_EQ(build(1, 1u, false, 0u), build(999, 2u, true, 5u));
}

TEST(RegistryDumpTest, ANameWithAnUnpairedSurrogateIsStoredAsSpelledAndShownAsText)
{
	// The scan's PATH002 names the file by its on-disk spelling (WTF-8, U+DC00 = ED B0 80), so the problem reaches
	// the file's record; the dump shows that spelling as U+FFFD and stays UTF-8.
	const FString path = FString("/Project/Lone") + FStringView("\xED\xB0\x80", 3u) + ".png";
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, path);
	SDiagnostic path002;
	path002.Severity = ESeverity::Warning;
	path002.Code = Path002;
	path002.Subject = path;
	path002.Message = "'Lone\xEF\xBF\xBD.png' is not a portable name";
	delta.Problems.push_back(SProblemsSet { path, EProblemSource::Scan, { path002 } });
	store.Commit(std::move(delta));

	uint32 attached = 0u;
	store.Live().ForEachDiagnostic(FindRecord(store.Live(), path), [&] (const SDiagnostic&) { ++attached; });
	EXPECT_EQ(attached, 1u);
	const FString dump = Dump(store.Live());
	EXPECT_TRUE(dump.View().IsValidUtf8());
	EXPECT_TRUE(dump.Contains("file /Project/Lone\xEF\xBF\xBD.png ")) << dump.CStr();
	EXPECT_TRUE(dump.Contains("Scan /Project/Lone\xEF\xBF\xBD.png: ")) << dump.CStr();
}

TEST(RegistryDumpTest, TheDumpFormatIsPinned)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/b.png", 2, 1);
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Project/t/B.png", "/Project/T/gone.png" });
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "", 0xFEu });
	delta.FileUpserts.push_back(MakeFile("/Project/T/x.bin", 3u, EFileClass::Unclaimed));
	store.Commit(std::move(delta));
	const FString expected =
		"mount /Engine\n"
		"mount /Project\n"
		"  rules Config/Assets.yml 00000000000000fe\n"
		"  folder /Project/M\n"
		"  folder /Project/T\n"
		"  file /Project/M/m.frtmat.yml Asset size=100\n"
		"    record type=frt.Material importer=Material.Legacy status=Analyzed stamp=0000000000000000 cook=Auto chunk=0 flags=HasProblems\n"
		"    edge /Project/t/B.png sub=0000000000000000 flags=Build|Path field=albedo.texture Resolved\n"
		"    edge /Project/T/gone.png sub=0000000000000000 flags=Build|Path field=albedo.texture Unresolved\n"
		"  file /Project/T/b.png Asset size=100\n"
		"    record type=frt.Texture importer=Texture status=Analyzed stamp=0000000000000000 cook=Auto chunk=0 flags=-\n"
		"    tag Height = 1\n"
		"    tag Width = 2\n"
		"  file /Project/T/x.bin Unclaimed size=3\n"
		"  problem REF006 info Registry /Project/M/m.frtmat.yml field albedo.texture: reference '/Project/t/B.png' resolves to "
		"'/Project/T/b.png', which is spelled differently on disk\n";
	EXPECT_EQ(Dump(store.Live()), expected) << Dump(store.Live()).CStr();
}

TEST(RegistryDumpTest, AMountFilterDumpsExactlyThoseMountsSections)
{
	const std::vector<SRegistryDelta> content = MakeContent();
	std::vector<uint32> order(content.size());
	for (uint32 i = 0u; i < order.size(); ++i)
	{
		order[i] = i;
	}
	CRegistryStore store;
	store.Commit(MakeMountsDelta());
	for (const uint32 index : order)
	{
		store.Commit(SRegistryDelta(content[index]));
	}
	const FString all = Dump(store.Live());
	const uint32 projectAt = all.View().Find("mount /Project\n");
	ASSERT_NE(projectAt, FStringView::NotFound);

	FString engine;
	const MountId engineOnly[] = { EngineMountId };
	store.Live().DumpText(engine, EDumpFlags::None, engineOnly);
	EXPECT_EQ(engine, all.View().Left(projectAt)) << engine.CStr();

	FString project;
	const MountId projectOnly[] = { ProjectMountId };
	store.Live().DumpText(project, EDumpFlags::None, projectOnly);
	EXPECT_EQ(project, all.View().RightChop(projectAt)) << project.CStr();

	FString both;
	const MountId bothMounts[] = { ProjectMountId, EngineMountId };
	store.Live().DumpText(both, EDumpFlags::None, bothMounts);
	EXPECT_EQ(both, all);
}

TEST(RegistryDumpTest, TheJsonLinesCarryTheTextDumpsContentInItsOrder)
{
	CRegistryStore store;
	store.Commit(MakeMountsDelta());
	for (const SRegistryDelta& delta : MakeContent())
	{
		store.Commit(SRegistryDelta(delta));
	}
	const FString text = Dump(store.Live());

	// What the text says, line by line: the kind of each top-level line and the path it names.
	std::vector<std::pair<FString, FString>> expected;
	for (const FStringView line : text.View().Split('\n'))
	{
		const auto take = [&] (FStringView Prefix, FStringView Kind)
		{
			if (!line.StartsWith(Prefix))
			{
				return false;
			}
			FStringView rest = line.RightChop(Prefix.Len());
			const uint32 space = rest.Find(' ');
			expected.emplace_back(FString(Kind), FString(space == FStringView::NotFound ? rest : rest.Left(space)));
			return true;
		};
		take("mount ", "mount") || take("  rules ", "rules") || take("  folder ", "folder") || take("  file ", "file")
			|| take("    record ", "record") || take("  problem ", "problem");
	}

	std::vector<std::pair<FString, FString>> actual;
	uint32 lines = 0u;
	FString lastFile;
	store.Live().DumpJson([&] (FStringView Line)
	{
		++lines;
		reflect::CDataTree tree;
		SDiagnostic error;
		ASSERT_TRUE(reflect::ParseJson(Line, tree, &error)) << error.Message.CStr() << ": " << FString(Line).CStr();
		const FString kind(tree.Find("kind").AsText().value_or(FStringView()));
		FString key;
		if (kind == "mount")
		{
			key = FString(tree.Find("mount").AsText().value_or(FStringView()));
		}
		else if (kind == "rules")
		{
			key = FString(tree.Find("file").AsText().value_or(FStringView()));
		}
		else if (kind == "problem")
		{
			key = FString(tree.Find("code").AsText().value_or(FStringView()));
		}
		else if (kind == "record")
		{
			// The record follows its file, with the record's own columns.
			key = "type=" + FString(tree.Find("type").AsText().value_or(FStringView()));
			EXPECT_EQ(FString(tree.Find("path").AsText().value_or(FStringView())), lastFile);
		}
		else
		{
			key = FString(tree.Find("path").AsText().value_or(FStringView()));
			if (kind == "file")
			{
				lastFile = key;
			}
		}
		actual.emplace_back(kind, key);

		if (kind == "record" && lastFile == "/Project/Models/Hero.gltf")
		{
			EXPECT_EQ(tree.Find("labels.0").AsText().value_or(FStringView()), "rule:Characters");
			EXPECT_EQ(tree.Find("subEntries.0.label").AsText().value_or(FStringView()), "image:#0");
			EXPECT_EQ(tree.Find("subEntries.1.tags.0.key").AsText().value_or(FStringView()), "VertexCount");
			EXPECT_EQ(tree.Find("subEntries.1.tags.0.value").AsText().value_or(FStringView()), "10");
			EXPECT_EQ(tree.Find("edges.1.field").AsText().value_or(FStringView()), "images[0].uri");
			EXPECT_EQ(tree.Find("edges.1.from").AsText().value_or(FStringView()), "image:#0");
			EXPECT_EQ(tree.Find("companions.0").AsText().value_or(FStringView()), "/Project/Models/Hero.bin");
			EXPECT_EQ(tree.Find("stamp").AsText().value_or(FStringView()), "0000000000abcdef");
		}
	});
	ASSERT_EQ(actual.size(), expected.size());
	for (size_t i = 0u; i < actual.size(); ++i)
	{
		EXPECT_EQ(actual[i].first, expected[i].first) << i;
		EXPECT_EQ(actual[i].second, expected[i].second) << i;
	}
	EXPECT_GT(lines, 10u);
}
