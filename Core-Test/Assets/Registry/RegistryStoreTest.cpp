// The registry store's commit semantics (design §5.2, §5.9; the delta contract in RegistryDelta.h): removes
// before upserts, records with sub-entries and typed tags, edges that resolve through the portable fold in
// either commit order (REF006 on a spelling difference, PATH001 when two records match), the companion rule,
// FOLDERS pre-order intervals, per-(subject, source) problem lists, the CSR with overflow lists and masking,
// lookups and their ambiguity, contract violations, the change ring and images. Every test builds its own
// store from deltas; nothing touches the disk.
#include <gtest/gtest.h>

#include <algorithm>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Registry/RegistryTables.h"
#include "RegistryTestFixture.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::registry_test;


namespace
{
uint32 CountWithCode (const CRegistryView& View, FStringView Subject, uint32 Code)
{
	uint32 count = 0u;
	View.ForEachDiagnosticAt(Subject, [&] (EProblemSource, const SDiagnostic& Diagnostic) { count += Diagnostic.Code == Code ? 1u : 0u; });
	return count;
}

SDiagnostic MakeScanWarning (FStringView Subject, FStringView Message)
{
	SDiagnostic diagnostic;
	diagnostic.Severity = ESeverity::Warning;
	diagnostic.Code = Path003;
	diagnostic.Subject = FString(Subject);
	diagnostic.Message = FString(Message);
	return diagnostic;
}
}


// ---------------- Records, lookups, tags ----------------

TEST(RegistryStoreTest, AFreshStorePublishesAnEmptyView)
{
	CRegistryStore store;
	const std::shared_ptr<const CRegistryView> view = store.Acquire();
	ASSERT_NE(view, nullptr);
	EXPECT_EQ(view->GetRevision(), 0u);
	EXPECT_EQ(view->GetRecordCount(), 0u);
	EXPECT_EQ(view->GetFileCount(), 0u);
	EXPECT_FALSE(view->FindByPath("/Project/a.png").has_value());
}

TEST(RegistryStoreTest, ACommitAddsFoldersFilesRecordsSubEntriesAndTags)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/Textures/Rock.png", 512, 256);
	delta.FileUpserts.push_back(MakeFile("/Project/Models/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Models/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.Tags.push_back(MakeIntTag("MeshCount", 2));
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", MakeSubId("mesh:Body"), asset_types::Mesh, ERecordFlags::None,
		{ MakeIntTag("VertexCount", 300) } });
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Armature/Head", 0u, asset_types::Mesh, ERecordFlags::None, {} });
	model.RuleLabels = { "Hero", "Characters" };
	delta.RecordUpserts.push_back(std::move(model));
	delta.FileUpserts.push_back(MakeFile("/Project/Input/Map.frtinputmap"));
	SRecordUpsert map = MakeRecord("/Project/Input/Map.frtinputmap", asset_types::InputActionMap, "Input.Legacy");
	map.Tags.push_back(STagDecl { "Actions", STagValue::FromName("Jump") });
	map.Tags.push_back(STagDecl { "Actions", STagValue::FromName("Fire") });
	map.Flags = ERecordFlags::Primary;
	map.PrimaryName = "Map";
	delta.RecordUpserts.push_back(std::move(map));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	EXPECT_EQ(view.GetRevision(), 1u);
	EXPECT_EQ(view.GetRecordCount(), 3u);
	EXPECT_EQ(view.GetSubEntryCount(), 2u);
	EXPECT_EQ(view.GetFileCount(), 3u);
	EXPECT_TRUE(view.FindFolder("/Project/Textures").has_value());
	EXPECT_TRUE(view.FindFolder("/Project").has_value());

	const SRecordId rock = FindRecord(view, "/Project/Textures/Rock.png");
	SAssetData data;
	ASSERT_TRUE(view.GetRecord(rock, data));
	EXPECT_EQ(data.Type, asset_types::Texture);
	EXPECT_EQ(data.MountPath, "/Project/Textures/Rock.png");
	EXPECT_EQ(data.Name, "Rock");
	EXPECT_EQ(data.Id.Guid, MakeProvisionalGuid("/Project/Textures/Rock.png"));
	EXPECT_EQ(data.Id64, MakeAssetId64(data.Id));
	EXPECT_EQ(data.Revision, 1u);
	STagValue width;
	ASSERT_TRUE(view.GetTag(rock, "Width", width));
	EXPECT_EQ(width.Type, ETagType::Int64);
	EXPECT_EQ(width.AsInt64(), 512);
	ASSERT_TRUE(view.GetTag(rock, "height", width));   // key names under the portable fold
	EXPECT_EQ(width.AsInt64(), 256);
	EXPECT_FALSE(view.GetTag(rock, "Channels", width));   // absent is not zero
	EXPECT_EQ(view.GetImporterName(rock), "Texture");

	// Lookups by id, by 64-bit id and by path agree; paths compare under the portable fold.
	EXPECT_EQ(view.Find(data.Id), rock);
	EXPECT_EQ(view.Find(data.Id64), rock);
	EXPECT_EQ(view.Exists(data.Id), EExists::Yes);
	EXPECT_EQ(FindRecord(view, "/project/textures/ROCK.PNG"), rock);

	// Sub-entries by "path#label" (labels may hold '/'), by SAssetId and by 64-bit id.
	const SRecordId hero = FindRecord(view, "/Project/Models/Hero.gltf");
	const SRecordId body = FindRecord(view, "/Project/Models/Hero.gltf#mesh:Body");
	const SRecordId head = FindRecord(view, "/Project/Models/Hero.gltf#mesh:Armature/Head");
	ASSERT_TRUE(body.IsValid() && body.IsSubEntry());
	ASSERT_TRUE(head.IsValid() && head.IsSubEntry());
	EXPECT_EQ(FindRecord(view, "/Project/Models/Hero.gltf#MESH:BODY"), body);   // the label under the fold
	EXPECT_EQ(view.GetOwner(body), hero);
	ASSERT_TRUE(view.GetRecord(body, data));
	EXPECT_EQ(data.MountPath, "/Project/Models/Hero.gltf#mesh:Body");
	EXPECT_EQ(data.Name, "mesh:Body");
	EXPECT_EQ(data.Type, asset_types::Mesh);
	EXPECT_EQ(view.Find(data.Id), body);
	EXPECT_EQ(view.Find(data.Id64), body);
	ASSERT_TRUE(view.GetTag(body, "VertexCount", width));
	EXPECT_EQ(width.AsInt64(), 300);
	std::vector<FString> labels;
	view.ForEachLabel(hero, [&] (FStringView Label, bool bRule)
	{
		EXPECT_TRUE(bRule);
		labels.push_back(FString(Label));
	});
	EXPECT_EQ(labels, (std::vector<FString> { "Hero", "Characters" }));

	// MultiValue Name tags keep every value.
	const SRecordId mapRecord = FindRecord(view, "/Project/Input/Map.frtinputmap");
	std::vector<FString> actions;
	view.ForEachTagValue(mapRecord, "Actions", [&] (const STagValue& Value) { actions.push_back(Value.Text); });
	EXPECT_EQ(actions, (std::vector<FString> { "Jump", "Fire" }));
	ASSERT_TRUE(view.GetRecord(mapRecord, data));
	EXPECT_EQ(data.Name, "Map");
	EXPECT_TRUE(HasAnyFlags(data.Flags, ERecordFlags::Primary));
}

TEST(RegistryStoreTest, RecordIdsFromAnotherViewAreRejected)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/a.png");
	store.Commit(std::move(delta));
	const std::shared_ptr<const CRegistryView> before = store.Acquire();
	const SRecordId record = FindRecord(*before, "/Project/a.png");
	ASSERT_TRUE(before->IsValid(record));

	SRegistryDelta second;
	AddTexture(second, "/Project/b.png");
	store.Commit(std::move(second));
	const std::shared_ptr<const CRegistryView> after = store.Acquire();
	SAssetData data;
	EXPECT_FALSE(after->IsValid(record));
	EXPECT_FALSE(after->GetRecord(record, data));
	EXPECT_TRUE(before->GetRecord(record, data));
	EXPECT_TRUE(after->GetRecord(FindRecord(*after, "/Project/a.png"), data));
}

TEST(RegistryStoreTest, RemovesApplyBeforeUpsertsInOneDelta)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/a.png", 1, 1);
	store.Commit(std::move(delta));

	// A remove and an upsert of the same file in one delta: the file stays, with the new content.
	SRegistryDelta replace;
	replace.FileRemoves.push_back("/Project/a.png");
	AddTexture(replace, "/Project/a.png", 2, 2);
	store.Commit(std::move(replace));
	STagValue width;
	ASSERT_TRUE(store.Live().GetTag(FindRecord(store.Live(), "/Project/a.png"), "Width", width));
	EXPECT_EQ(width.AsInt64(), 2);
	EXPECT_EQ(store.Live().GetRecordCount(), 1u);

	// RecordRemoves keeps the file.
	SRegistryDelta removeRecord;
	removeRecord.RecordRemoves.push_back("/Project/a.png");
	store.Commit(std::move(removeRecord));
	EXPECT_EQ(store.Live().GetRecordCount(), 0u);
	EXPECT_EQ(store.Live().GetFileCount(), 1u);
	SFileData file;
	ASSERT_TRUE(store.Live().GetFile(*store.Live().FindFileExact("/Project/a.png"), file));
	EXPECT_FALSE(file.Record.IsValid());
}

TEST(RegistryStoreTest, AFolderRemoveDropsEverythingAtOrBelowIt)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/Env/Rock.png");
	AddTexture(delta, "/Project/Env/Trees/Oak.png");
	AddTexture(delta, "/Project/Envy.png");   // a sibling that shares the prefix
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/Env/Trees", 7u });
	delta.Problems.push_back(SProblemsSet { "/Project/Env/Trees/Oak.png", EProblemSource::Scan, { MakeScanWarning("/Project/Env/Trees/Oak.png", "long") } });
	delta.Problems.push_back(SProblemsSet { "/Project/Envy.png", EProblemSource::Scan, { MakeScanWarning("/Project/Envy.png", "long") } });
	store.Commit(std::move(delta));
	ASSERT_TRUE(store.Live().FindRuleFile("Project", "/Project/Env/Trees").has_value());

	SRegistryDelta remove;
	remove.FolderRemoves.push_back("/Project/Env");
	store.Commit(std::move(remove));
	const CRegistryView& view = store.Live();
	EXPECT_EQ(view.GetRecordCount(), 1u);
	EXPECT_EQ(view.GetFileCount(), 1u);
	EXPECT_FALSE(view.FindFolder("/Project/Env").has_value());
	EXPECT_FALSE(view.FindFolder("/Project/Env/Trees").has_value());
	EXPECT_FALSE(view.FindRuleFile("Project", "/Project/Env/Trees").has_value());
	EXPECT_EQ(CountWithCode(view, "/Project/Env/Trees/Oak.png", Path003), 0u);
	EXPECT_EQ(CountWithCode(view, "/Project/Envy.png", Path003), 1u);
	EXPECT_TRUE(FindRecord(view, "/Project/Envy.png").IsValid());
}


// ---------------- Copy-on-write ----------------

TEST(RegistryStoreTest, ACommitCopiesOnlyThePagesItTouches)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	for (uint32 i = 0u; i < 2u * RegistryPageRows + 10u; ++i)
	{
		AddTexture(delta, FString::Format("/Project/T/t{}.png", i), i, i);
	}
	store.Commit(std::move(delta));
	const std::shared_ptr<const CRegistryView> before = store.Acquire();

	// Edit one texture in the last page of FILES: the first two pages stay shared, the edited one is copied.
	SRegistryDelta edit;
	SFileUpsert file = MakeFile(FString::Format("/Project/T/t{}.png", 2u * RegistryPageRows + 5u), 999u);
	edit.FileUpserts.push_back(file);
	store.Commit(std::move(edit));
	const std::shared_ptr<const CRegistryView> after = store.Acquire();

	const CPagedColumn<uint64>& sizesBefore = GetTables(*before).Files.Size;
	const CPagedColumn<uint64>& sizesAfter = GetTables(*after).Files.Size;
	ASSERT_EQ(sizesBefore.GetPageCount(), 3u);
	EXPECT_EQ(sizesBefore.GetPageAddress(0u), sizesAfter.GetPageAddress(0u));
	EXPECT_EQ(sizesBefore.GetPageAddress(1u), sizesAfter.GetPageAddress(1u));
	EXPECT_NE(sizesBefore.GetPageAddress(2u), sizesAfter.GetPageAddress(2u));
	// A column the edit never wrote shares every page.
	const CPagedColumn<uint32>& recordsBefore = GetTables(*before).Files.Record;
	const CPagedColumn<uint32>& recordsAfter = GetTables(*after).Files.Record;
	for (uint32 page = 0u; page < recordsBefore.GetPageCount(); ++page)
	{
		EXPECT_EQ(recordsBefore.GetPageAddress(page), recordsAfter.GetPageAddress(page));
	}

	// The old view still answers with the old size.
	SFileData data;
	ASSERT_TRUE(before->GetFile(*before->FindFileExact(file.MountPath), data));
	EXPECT_EQ(data.Size, 100u);
	ASSERT_TRUE(after->GetFile(*after->FindFileExact(file.MountPath), data));
	EXPECT_EQ(data.Size, 999u);
}


// ---------------- Edges ----------------

TEST(RegistryStoreTest, EdgesResolveTheSameInEitherCommitOrder)
{
	const auto build = [] (bool bTexturesFirst)
	{
		CRegistryStore store;
		store.Commit(MakeMountsDelta());
		SRegistryDelta textures;
		AddTexture(textures, "/Project/T/a.png");
		AddTexture(textures, "/Project/T/b.png");
		SRegistryDelta materials;
		AddMaterial(materials, "/Project/M/m.frtmat.yml", { "/Project/T/a.png", "/Project/T/b.png", "/Project/T/missing.png" });
		if (bTexturesFirst)
		{
			store.Commit(std::move(textures));
			store.Commit(std::move(materials));
		}
		else
		{
			store.Commit(std::move(materials));
			store.Commit(std::move(textures));
		}
		return Dump(store.Live());
	};
	const FString texturesFirst = build(true);
	const FString materialsFirst = build(false);
	EXPECT_EQ(texturesFirst, materialsFirst);
	EXPECT_TRUE(texturesFirst.View().Contains("edge /Project/T/missing.png sub=0000000000000000 flags=Build|Path field=albedo.texture Unresolved"))
		<< texturesFirst.CStr();
	EXPECT_TRUE(texturesFirst.View().Contains("edge /Project/T/a.png sub=0000000000000000 flags=Build|Path field=albedo.texture Resolved"))
		<< texturesFirst.CStr();

	CRegistryStore store;
	store.Commit(MakeMountsDelta());
	SRegistryDelta materials;
	AddMaterial(materials, "/Project/M/m.frtmat.yml", { "/Project/T/a.png" });
	store.Commit(std::move(materials));
	const SRecordId material = FindRecord(store.Live(), "/Project/M/m.frtmat.yml");
	std::vector<SDependencyView> edges = GetDependencies(store.Live(), material);
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_TRUE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	EXPECT_FALSE(edges[0].To.IsValid());
	EXPECT_EQ(edges[0].TargetPath, "/Project/T/a.png");
	EXPECT_EQ(edges[0].FieldPath, "albedo.texture");

	// The target appears: the edge is rebound in place, the reverse side included.
	SRegistryDelta textures;
	AddTexture(textures, "/Project/T/a.png");
	store.Commit(std::move(textures));
	const CRegistryView& view = store.Live();
	const SRecordId material2 = FindRecord(view, "/Project/M/m.frtmat.yml");
	const SRecordId texture = FindRecord(view, "/Project/T/a.png");
	edges = GetDependencies(view, material2);
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_FALSE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	EXPECT_EQ(edges[0].To, texture);
	EXPECT_EQ(edges[0].From, material2);
	EXPECT_EQ(GetReferencers(view, texture), std::vector<SRecordId> { material2 });

	// And it is unbound again when the target goes.
	SRegistryDelta remove;
	remove.FileRemoves.push_back("/Project/T/a.png");
	store.Commit(std::move(remove));
	edges = GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/M/m.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_TRUE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
}

TEST(RegistryStoreTest, AReferenceSpelledInAnotherCaseResolvesWithRef006)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/Rock.png");
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Project/t/rock.png" });
	store.Commit(std::move(delta));
	const CRegistryView& view = store.Live();
	const SRecordId material = FindRecord(view, "/Project/M/m.frtmat.yml");
	const std::vector<SDependencyView> edges = GetDependencies(view, material);
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_EQ(edges[0].To, FindRecord(view, "/Project/T/Rock.png"));
	const std::vector<SDiagnostic> ref006 = GetDiagnostics(view, material, Ref006);
	ASSERT_EQ(ref006.size(), 1u);
	EXPECT_EQ(ref006[0].Severity, ESeverity::Info);
	EXPECT_EQ(ref006[0].Subject, "/Project/M/m.frtmat.yml");
	EXPECT_EQ(ref006[0].FieldPath, "albedo.texture");
	EXPECT_TRUE(ref006[0].Message.View().Contains("/Project/T/Rock.png")) << ref006[0].Message.CStr();
	SAssetData data;
	ASSERT_TRUE(view.GetRecord(material, data));
	EXPECT_TRUE(HasAnyFlags(data.Flags, ERecordFlags::HasProblems));

	// Fixing the spelling clears it.
	SRegistryDelta fix;
	AddMaterial(fix, "/Project/M/m.frtmat.yml", { "/Project/T/Rock.png" });
	store.Commit(std::move(fix));
	const SRecordId fixed = FindRecord(store.Live(), "/Project/M/m.frtmat.yml");
	EXPECT_TRUE(GetDiagnostics(store.Live(), fixed, Ref006).empty());
	ASSERT_TRUE(store.Live().GetRecord(fixed, data));
	EXPECT_FALSE(HasAnyFlags(data.Flags, ERecordFlags::HasProblems));
}

TEST(RegistryStoreTest, TwoFilesEqualUnderTheFoldMakeEdgesAndLookupsFailWithPath001)
{
	// A case-sensitive directory inside a case-insensitive mount holds both spellings: FILES keeps both rows
	// (equal under the mount's rule), the portable index holds both records.
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/Rock.png", 1, 1);
	AddTexture(delta, "/Project/T/rock.png", 2, 2);
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Project/T/ROCK.png" });
	store.Commit(std::move(delta));
	const CRegistryView& view = store.Live();
	EXPECT_EQ(view.GetFileCount(), 3u);
	EXPECT_TRUE(view.FindFileExact("/Project/T/Rock.png").has_value());
	EXPECT_TRUE(view.FindFileExact("/Project/T/rock.png").has_value());
	EXPECT_NE(*view.FindFileExact("/Project/T/Rock.png"), *view.FindFileExact("/Project/T/rock.png"));
	EXPECT_EQ(view.FindFile("/Project/T/Rock.png"), view.FindFileExact("/Project/T/Rock.png"));   // exact wins
	EXPECT_FALSE(view.FindFile("/Project/T/ROCK.png").has_value());                                // two match

	SDiagnostic error;
	EXPECT_FALSE(view.FindByPath("/Project/T/rock.png", &error).has_value());
	EXPECT_EQ(error.Code, Path001);

	const SRecordId material = FindRecord(view, "/Project/M/m.frtmat.yml");
	const std::vector<SDependencyView> edges = GetDependencies(view, material);
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_TRUE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	EXPECT_TRUE(edges[0].bAmbiguous);
	EXPECT_EQ(GetDiagnostics(view, material, Path001).size(), 1u);

	// Removing one spelling resolves the edge (with REF006: the reference is spelled ROCK).
	SRegistryDelta remove;
	remove.FileRemoves.push_back("/Project/T/rock.png");
	store.Commit(std::move(remove));
	const SRecordId material2 = FindRecord(store.Live(), "/Project/M/m.frtmat.yml");
	EXPECT_EQ(GetDependencies(store.Live(), material2)[0].To, FindRecord(store.Live(), "/Project/T/Rock.png"));
	EXPECT_TRUE(GetDiagnostics(store.Live(), material2, Path001).empty());
	EXPECT_EQ(GetDiagnostics(store.Live(), material2, Ref006).size(), 1u);
}

TEST(RegistryStoreTest, ACaseSensitiveMountFindsFilesAndFoldersByteExact)
{
	// /Project on a case-sensitive volume: its FILES and FOLDERS indices compare bytes, so the OS's two files
	// stay two rows and a third spelling finds neither. /Engine stays case-insensitive (NTFS's rule). The
	// portable lookups do not follow the volume: FindByPath still fails with PATH001.
	CRegistryStore store;
	SRegistryDelta delta;
	delta.Mounts.push_back(MakeMount("Engine", EngineMountId));
	delta.Mounts.push_back(MakeMount("Project", ProjectMountId, true));
	AddTexture(delta, "/Project/T/Rock.png", 1, 1);
	AddTexture(delta, "/Project/T/rock.png", 2, 2);
	AddTexture(delta, "/Engine/A/White.png");
	store.Commit(std::move(delta));
	const CRegistryView& view = store.Live();

	ASSERT_TRUE(view.FindMount("Project").has_value());
	EXPECT_TRUE(view.FindMount("Project")->bCaseSensitive);
	EXPECT_FALSE(view.FindMount("Engine")->bCaseSensitive);
	const std::optional<uint32> upper = view.FindFileExact("/Project/T/Rock.png");
	const std::optional<uint32> lower = view.FindFileExact("/Project/T/rock.png");
	ASSERT_TRUE(upper.has_value() && lower.has_value());
	EXPECT_NE(*upper, *lower);
	EXPECT_EQ(view.FindFile("/Project/T/Rock.png"), upper);
	EXPECT_EQ(view.FindFile("/Project/T/rock.png"), lower);
	EXPECT_FALSE(view.FindFile("/Project/T/ROCK.png").has_value());
	EXPECT_FALSE(view.FindFile("/Project/t/Rock.png").has_value());
	EXPECT_TRUE(view.FindFolder("/Project/T").has_value());
	EXPECT_FALSE(view.FindFolder("/Project/t").has_value());
	EXPECT_EQ(view.FindFile("/Engine/a/WHITE.PNG"), view.FindFileExact("/Engine/A/White.png"));
	EXPECT_EQ(view.FindFolder("/Engine/a"), view.FindFolder("/Engine/A"));

	SDiagnostic error;
	EXPECT_FALSE(view.FindByPath("/Project/T/Rock.png", &error).has_value());
	EXPECT_EQ(error.Code, Path001);

	// Removing one spelling leaves the other alone; the third spelling still names no file on this volume,
	// while the portable lookup now resolves.
	SRegistryDelta remove;
	remove.FileRemoves.push_back("/Project/T/rock.png");
	store.Commit(std::move(remove));
	EXPECT_EQ(store.Live().FindFile("/Project/T/Rock.png"), upper);
	EXPECT_FALSE(store.Live().FindFile("/Project/T/rock.png").has_value());
	EXPECT_FALSE(store.Live().FindFile("/Project/T/ROCK.png").has_value());
	EXPECT_EQ(FindRecord(store.Live(), "/Project/T/ROCK.png"), FindRecord(store.Live(), "/Project/T/Rock.png"));
	EXPECT_TRUE(FindRecord(store.Live(), "/Project/T/Rock.png").IsValid());
}

TEST(RegistryStoreTest, AMountWhoseCaseRuleChangedIsReindexed)
{
	// A snapshot registers the mount with the rule it had; CMountTable's upsert brings the volume's rule now.
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/Rock.png");
	store.Commit(std::move(delta));
	EXPECT_TRUE(store.Live().FindFile("/Project/t/ROCK.png").has_value());
	EXPECT_TRUE(store.Live().FindFolder("/Project/t").has_value());

	SRegistryDelta sensitive;
	sensitive.Mounts.push_back(MakeMount("Project", ProjectMountId, true));
	store.Commit(std::move(sensitive));
	const CRegistryView& view = store.Live();
	EXPECT_TRUE(view.FindMount("Project")->bCaseSensitive);
	EXPECT_FALSE(view.FindFile("/Project/t/ROCK.png").has_value());
	EXPECT_FALSE(view.FindFolder("/Project/t").has_value());
	EXPECT_TRUE(view.FindFile("/Project/T/Rock.png").has_value());
	EXPECT_TRUE(view.FindFolder("/Project/T").has_value());
	EXPECT_TRUE(view.FindFolder("/Project").has_value());
	EXPECT_EQ(CountWithCode(view, "/Project", 0u), 0u);   // a changed volume, not a contract violation
	EXPECT_TRUE(FindRecord(view, "/Project/t/rock.png").IsValid());   // records stay under the portable fold

	SRegistryDelta insensitive;
	insensitive.Mounts.push_back(MakeMount("Project", ProjectMountId, false));
	store.Commit(std::move(insensitive));
	EXPECT_FALSE(store.Live().FindMount("Project")->bCaseSensitive);
	EXPECT_TRUE(store.Live().FindFile("/Project/t/ROCK.png").has_value());
	EXPECT_TRUE(store.Live().FindFolder("/Project/t").has_value());
}

TEST(RegistryStoreTest, EveryFileKeepsAFoldedShadowOfItsPath)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/Textures/T_Rock.png");
	AddTexture(delta, "/Project/textures/t_lower.png");
	store.Commit(std::move(delta));
	const SRegistryTables& tables = GetTables(store.Live());
	const uint32 rock = *store.Live().FindFileExact("/Project/Textures/T_Rock.png");
	const uint32 lower = *store.Live().FindFileExact("/Project/textures/t_lower.png");
	EXPECT_EQ(GetFoldedFilePath(tables, rock), "/project/textures/t_rock.png");
	EXPECT_TRUE(GetFoldedFilePath(tables, rock).Contains("textures/t_rock"));   // a substring across a '/'
	EXPECT_EQ(GetFoldedFilePath(tables, lower), "/project/textures/t_lower.png");
}

TEST(RegistryStoreTest, AVeryDeepFolderChainCommitsRoundTripsAndIsRemoved)
{
	// 2000 nested folders: creating, exporting, adopting (with children listed before their parents, as a
	// hand-made or hostile image may) and removing them must not recurse once per level.
	FString folder("/Project");
	for (uint32 i = 0u; i < 2000u; ++i)
	{
		folder += "/d";
	}
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, folder + FStringView("/a.png"));
	store.Commit(std::move(delta));
	EXPECT_EQ(store.Live().GetFolderCount(), 2002u);   // both mount roots and the chain
	const FString original = Dump(store.Live());

	SRegistryImage image = store.Live().ExportImage({});
	const uint32 count = static_cast<uint32>(image.Folders.size());
	std::reverse(image.Folders.begin(), image.Folders.end());
	for (SImageFolder& entry : image.Folders)
	{
		entry.Parent = entry.Parent == ~0u ? ~0u : count - 1u - entry.Parent;
	}
	for (SImageFile& file : image.Files)
	{
		file.Folder = count - 1u - file.Folder;
	}
	CRegistryStore adopted;
	adopted.AdoptImage(std::move(image), 1u);
	EXPECT_EQ(Dump(adopted.Live()), original);

	SRegistryDelta remove;
	remove.FolderRemoves.push_back("/Project/d");
	store.Commit(std::move(remove));
	EXPECT_EQ(store.Live().GetFolderCount(), 2u);
	EXPECT_EQ(store.Live().GetFileCount(), 0u);
	EXPECT_EQ(store.Live().GetRecordCount(), 0u);
}

TEST(RegistryStoreTest, ARecordKeepsOneEdgePerTargetSubAndFlags)
{
	// Two slots of a legacy material naming one texture: one edge (the first field path), one referencer.
	// Other flags are another edge (section 5.2: "hard + editor-only" never collapses into "hard + game").
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/a.png");
	delta.FileUpserts.push_back(MakeFile("/Project/M/m.frtmat.yml"));
	SRecordUpsert material = MakeRecord("/Project/M/m.frtmat.yml", asset_types::Material, "Material.Legacy");
	material.Edges.push_back(MakePathEdge("/Project/T/a.png", EDepFlags::Build | EDepFlags::Path, "albedo.texture"));
	material.Edges.push_back(MakePathEdge("/Project/T/a.png", EDepFlags::Build | EDepFlags::Path, "emissive.texture"));
	material.Edges.push_back(MakePathEdge("/Project/T/a.png", EDepFlags::Hard, "preview"));
	delta.RecordUpserts.push_back(std::move(material));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const std::vector<SDependencyView> edges = GetDependencies(view, FindRecord(view, "/Project/M/m.frtmat.yml"));
	ASSERT_EQ(edges.size(), 2u);
	EXPECT_EQ(edges[0].FieldPath, "albedo.texture");
	EXPECT_EQ(edges[0].Flags, EDepFlags::Build | EDepFlags::Path);
	EXPECT_EQ(edges[1].FieldPath, "preview");
	EXPECT_EQ(GetReferencers(view, FindRecord(view, "/Project/T/a.png")).size(), 2u);
}

TEST(RegistryStoreTest, LabelledTargetsResolveToSubEntriesAndHashesInFolderNamesStayPaths)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, {} });
	delta.RecordUpserts.push_back(std::move(model));
	AddTexture(delta, "/Project/C#/x.png");
	delta.FileUpserts.push_back(MakeFile("/Project/User.frtmat.yml"));
	SRecordUpsert user = MakeRecord("/Project/User.frtmat.yml", asset_types::Material, "Material.Legacy");
	user.Edges.push_back(MakePathEdge("/Project/Hero.gltf#mesh:Body"));
	user.Edges.push_back(MakePathEdge("/Project/Hero.gltf#mesh:Missing"));
	user.Edges.push_back(MakePathEdge("/Project/C#/x.png"));
	delta.RecordUpserts.push_back(std::move(user));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const std::vector<SDependencyView> edges = GetDependencies(view, FindRecord(view, "/Project/User.frtmat.yml"));
	ASSERT_EQ(edges.size(), 3u);
	EXPECT_EQ(edges[0].To, FindRecord(view, "/Project/Hero.gltf#mesh:Body"));
	EXPECT_EQ(edges[0].TargetSub, MakeSubId("mesh:Body"));
	EXPECT_EQ(edges[1].To, FindRecord(view, "/Project/Hero.gltf"));   // the file resolves; the label names nothing
	EXPECT_EQ(edges[1].TargetSub, MakeSubId("mesh:Missing"));
	EXPECT_EQ(edges[2].To, FindRecord(view, "/Project/C#/x.png"));
	EXPECT_EQ(edges[2].TargetSub, 0u);
	EXPECT_EQ(GetReferencers(view, FindRecord(view, "/Project/Hero.gltf#mesh:Body")).size(), 1u);
	EXPECT_EQ(GetReferencers(view, FindRecord(view, "/Project/Hero.gltf")).size(), 2u);
}

TEST(RegistryStoreTest, TheCsrKeepsOverflowListsAndMasksSupersededEdges)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/a.png");
	AddTexture(delta, "/Project/T/b.png");
	AddTexture(delta, "/Project/T/c.png");
	AddMaterial(delta, "/Project/M/m1.frtmat.yml", { "/Project/T/a.png", "/Project/T/b.png" });
	AddMaterial(delta, "/Project/M/m2.frtmat.yml", { "/Project/T/a.png" });
	store.Commit(std::move(delta));
	store.CompactGraph();
	{
		const SRegistryTables& tables = GetTables(store.Live());
		EXPECT_EQ(tables.Graph.BaseEdges.Size(), 3u);
		EXPECT_EQ(tables.Graph.OverflowEdges.Size(), 0u);
		EXPECT_EQ(tables.Graph.GarbageEdges, 0u);
	}

	// m1 now points at c only: its base slice is masked and the new edge lives in an overflow list.
	SRegistryDelta edit;
	AddMaterial(edit, "/Project/M/m1.frtmat.yml", { "/Project/T/c.png" });
	store.Commit(std::move(edit));
	const auto check = [] (const CRegistryView& View)
	{
		const SRecordId m1 = FindRecord(View, "/Project/M/m1.frtmat.yml");
		const SRecordId m2 = FindRecord(View, "/Project/M/m2.frtmat.yml");
		const std::vector<SDependencyView> edges = GetDependencies(View, m1);
		ASSERT_EQ(edges.size(), 1u);
		EXPECT_EQ(edges[0].To, FindRecord(View, "/Project/T/c.png"));
		EXPECT_EQ(GetReferencers(View, FindRecord(View, "/Project/T/a.png")), std::vector<SRecordId> { m2 });
		EXPECT_TRUE(GetReferencers(View, FindRecord(View, "/Project/T/b.png")).empty());
		EXPECT_EQ(GetReferencers(View, FindRecord(View, "/Project/T/c.png")), std::vector<SRecordId> { m1 });
	};
	{
		const SRegistryTables& tables = GetTables(store.Live());
		const uint32 m1Row = FindRecord(store.Live(), "/Project/M/m1.frtmat.yml").Value;
		EXPECT_TRUE(tables.Assets.BaseEdgesMasked.Test(m1Row));
		EXPECT_EQ(tables.Assets.OverflowEdges[m1Row].Count, 1u);
		EXPECT_EQ(tables.Graph.GarbageEdges, 2u);
		EXPECT_EQ(tables.Graph.LiveEdges, 2u);
	}
	check(store.Live());

	// A compaction rebuilds both CSRs with the same answers.
	const FString before = Dump(store.Live());
	store.CompactGraph();
	check(store.Live());
	EXPECT_EQ(Dump(store.Live()), before);
	EXPECT_EQ(GetTables(store.Live()).Graph.BaseEdges.Size(), 2u);
	EXPECT_EQ(GetTables(store.Live()).Graph.GarbageEdges, 0u);
}

TEST(RegistryStoreTest, ClosuresAndWhyReferencedFollowResolvedEdges)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/a.png");
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Project/T/a.png" });
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.SubEntries.push_back(SSubEntryDecl { "material:Skin", 0u, asset_types::Material, ERecordFlags::None, {} });
	SEdgeDecl edge = MakePathEdge("/Project/M/m.frtmat.yml");
	edge.FromSub = MakeSubId("material:Skin");
	model.Edges.push_back(edge);
	delta.RecordUpserts.push_back(std::move(model));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const SRecordId hero = FindRecord(view, "/Project/Hero.gltf");
	const SRecordId skin = FindRecord(view, "/Project/Hero.gltf#material:Skin");
	const SRecordId material = FindRecord(view, "/Project/M/m.frtmat.yml");
	const SRecordId texture = FindRecord(view, "/Project/T/a.png");

	// The edge comes from the sub-entry: the asset sees it, the sub-entry sees it, the referencer is the sub-entry.
	EXPECT_EQ(GetDependencies(view, hero).size(), 1u);
	EXPECT_EQ(GetDependencies(view, skin).size(), 1u);
	EXPECT_EQ(GetReferencers(view, material), std::vector<SRecordId> { skin });

	// The traversals walk files: the model's file reaches the texture through its sub-entry's edge, and the
	// texture's referencers reach the whole model file.
	std::vector<SRecordId> closure;
	const SRecordId roots[] = { hero };
	view.GetClosure(roots, EDepFlags {}, closure);
	EXPECT_EQ(closure, (std::vector<SRecordId> { hero, skin, material, texture }));
	view.GetClosure(roots, EDepFlags::Hard, closure);   // no Hard edges
	EXPECT_EQ(closure, (std::vector<SRecordId> { hero, skin }));

	const SRecordId referencerRoots[] = { texture };
	view.GetReferencerClosure(referencerRoots, EDepFlags {}, closure);
	EXPECT_EQ(closure, (std::vector<SRecordId> { texture, material, hero, skin }));

	std::vector<SDependencyView> chain;
	ASSERT_TRUE(view.WhyReferenced(hero, texture, chain));
	ASSERT_EQ(chain.size(), 2u);
	EXPECT_EQ(chain[0].From, skin);
	EXPECT_EQ(chain[0].To, material);
	EXPECT_EQ(chain[1].From, material);
	EXPECT_EQ(chain[1].To, texture);
	EXPECT_TRUE(view.WhyReferenced(skin, texture, chain));
	EXPECT_EQ(chain.size(), 2u);
	EXPECT_TRUE(view.WhyReferenced(hero, skin, chain));   // one file
	EXPECT_TRUE(chain.empty());
	EXPECT_FALSE(view.WhyReferenced(texture, hero, chain));
}

TEST(RegistryStoreTest, AFileAndItsSubEntriesAreOneNodeForEveryTraversal)
{
	// Hero.gltf: a file-level edge to its buffer, a sub-entry with an edge to a texture. Whole references
	// the file, Part references a sub-entry that declares no edges, Loose references the texture directly.
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/skin.png");
	AddTexture(delta, "/Project/Hero.bin");   // stands in for a buffer with a record of its own
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, {} });
	model.SubEntries.push_back(SSubEntryDecl { "material:Skin", 0u, asset_types::Material, ERecordFlags::None, {} });
	model.Edges.push_back(MakePathEdge("/Project/Hero.bin", EDepFlags::Build | EDepFlags::Path, "buffers[0].uri"));
	SEdgeDecl skinEdge = MakePathEdge("/Project/T/skin.png", EDepFlags::Hard);
	skinEdge.FromSub = MakeSubId("material:Skin");
	model.Edges.push_back(skinEdge);
	delta.RecordUpserts.push_back(std::move(model));
	AddMaterial(delta, "/Project/Whole.frtmat.yml", { "/Project/Hero.gltf" });
	AddMaterial(delta, "/Project/Part.frtmat.yml", { "/Project/Hero.gltf#mesh:Body" });
	AddMaterial(delta, "/Project/Loose.frtmat.yml", { "/Project/T/skin.png" });
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	const SRecordId hero = FindRecord(view, "/Project/Hero.gltf");
	const SRecordId body = FindRecord(view, "/Project/Hero.gltf#mesh:Body");
	const SRecordId skin = FindRecord(view, "/Project/Hero.gltf#material:Skin");
	const SRecordId bin = FindRecord(view, "/Project/Hero.bin");
	const SRecordId texture = FindRecord(view, "/Project/T/skin.png");
	const SRecordId whole = FindRecord(view, "/Project/Whole.frtmat.yml");
	const SRecordId part = FindRecord(view, "/Project/Part.frtmat.yml");
	const SRecordId loose = FindRecord(view, "/Project/Loose.frtmat.yml");
	const std::vector<SRecordId> all { hero, body, skin, bin, texture, whole, part, loose };
	for (const SRecordId record : all)
	{
		ASSERT_TRUE(record.IsValid());
	}
	const auto contains = [] (const std::vector<SRecordId>& List, SRecordId Record)
	{
		return std::find(List.begin(), List.end(), Record) != List.end();
	};

	// Whoever references the file or one of its parts transitively references what any part references.
	std::vector<SRecordId> closure;
	const SRecordId textureRoot[] = { texture };
	view.GetReferencerClosure(textureRoot, EDepFlags {}, closure);
	for (const SRecordId expected : { hero, body, skin, whole, part, loose })
	{
		EXPECT_TRUE(contains(closure, expected));
	}
	const SRecordId partRoot[] = { part };
	view.GetClosure(partRoot, EDepFlags {}, closure);
	EXPECT_EQ(closure, (std::vector<SRecordId> { part, hero, body, skin, bin, texture }));
	std::vector<SDependencyView> chain;
	ASSERT_TRUE(view.WhyReferenced(part, bin, chain));
	ASSERT_EQ(chain.size(), 2u);
	EXPECT_EQ(chain[0].To, body);
	EXPECT_EQ(chain[1].From, hero);
	EXPECT_EQ(chain[1].To, bin);
	EXPECT_TRUE(view.WhyReferenced(part, hero, chain));
	EXPECT_TRUE(view.WhyReferenced(whole, texture, chain));
	EXPECT_FALSE(view.WhyReferenced(loose, hero, chain));

	// The three traversals agree on every pair, and Follow filters both directions alike.
	for (const EDepFlags follow : { EDepFlags {}, EDepFlags::Hard })
	{
		for (const SRecordId from : all)
		{
			const SRecordId fromRoot[] = { from };
			view.GetClosure(fromRoot, follow, closure);
			for (const SRecordId to : all)
			{
				const SRecordId toRoot[] = { to };
				std::vector<SRecordId> referencers;
				view.GetReferencerClosure(toRoot, follow, referencers);
				EXPECT_EQ(contains(closure, to), contains(referencers, from)) << from.Value << " -> " << to.Value;
				if (follow == EDepFlags {})
				{
					EXPECT_EQ(contains(closure, to), view.WhyReferenced(from, to, chain)) << from.Value << " -> " << to.Value;
				}
			}
		}
	}
}


// ---------------- Garbage and compaction ----------------

namespace
{
SRecordUpsert MakeHero (uint32 Variant, int64 Vertices)
{
	SRecordUpsert record = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	record.RuleLabels = { "Hero", "Characters" };
	record.Companions = { "/Project/Hero.bin" };
	record.Edges.push_back(MakePathEdge("/Project/Hero.bin", EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
	record.Tags = { MakeIntTag("MeshCount", 2) };
	record.SubEntries.push_back(SSubEntryDecl { FString::Format("mesh:Body{}", Variant), 0u, asset_types::Mesh, ERecordFlags::None,
		{ MakeIntTag("VertexCount", Vertices) } });
	record.SubEntries.push_back(SSubEntryDecl { "material:Skin", 0u, asset_types::Material, ERecordFlags::None, {} });
	return record;
}

SRecordUpsert MakeInputMap (std::initializer_list<FStringView> Actions)
{
	SRecordUpsert record = MakeRecord("/Project/Input/Map.frtinputmap", asset_types::InputActionMap, "Input.Legacy");
	for (const FStringView action : Actions)
	{
		record.Tags.push_back(STagDecl { "Actions", STagValue::FromName(action) });
	}
	return record;
}

struct SPoolSizes
{
	uint32 SubEntries = 0u;
	uint32 MeshRows = 0u;
	uint32 Labels = 0u;
	uint32 Companions = 0u;
	uint32 Paths = 0u;
	uint32 Actions = 0u;

	bool operator== (const SPoolSizes&) const = default;
};

SPoolSizes GetPoolSizes (const CRegistryView& View)
{
	const SRegistryTables& tables = GetTables(View);
	return SPoolSizes { tables.SubEntries.Owner.Size(), tables.Types[asset_types::Mesh].Records.Size(), tables.Labels.Size(),
		tables.Companions.Size(), tables.Paths.Size(), tables.Types[asset_types::InputActionMap].Columns[0].Multi.Size() };
}
}

TEST(RegistryStoreTest, ReUpsertsOfTheSameStructureReuseEveryRowAndPool)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 5u, EFileClass::Unclaimed));
	delta.FileUpserts.push_back(MakeFile("/Project/Input/Map.frtinputmap"));
	delta.RecordUpserts.push_back(MakeHero(0u, 100));
	delta.RecordUpserts.push_back(MakeInputMap({ "Jump", "Fire" }));
	store.Commit(std::move(delta));
	const SPoolSizes initial = GetPoolSizes(store.Live());

	// A rules edit or a re-analyzed file re-upserts the same structure: values change, nothing appends.
	for (uint32 i = 0u; i < 20u; ++i)
	{
		SRegistryDelta again;
		again.RecordUpserts.push_back(MakeHero(0u, 100 + i));
		again.RecordUpserts.push_back(MakeInputMap({ "Jump", "Fire" }));
		store.Commit(std::move(again));
	}
	const CRegistryView& view = store.Live();
	EXPECT_EQ(GetPoolSizes(view), initial);
	EXPECT_EQ(GetTables(view).GarbagePoolItems, 0u);
	EXPECT_EQ(GetTables(view).Graph.GarbageEdges, 0u);         // the unchanged edge stays where it is
	EXPECT_EQ(GetTables(view).Graph.OverflowEdges.Size(), 1u);
	STagValue vertices;
	ASSERT_TRUE(view.GetTag(FindRecord(view, "/Project/Hero.gltf#mesh:Body0"), "VertexCount", vertices));
	EXPECT_EQ(vertices.AsInt64(), 119);
	std::vector<FString> labels;
	view.ForEachLabel(FindRecord(view, "/Project/Hero.gltf"), [&] (FStringView Label, bool) { labels.push_back(FString(Label)); });
	EXPECT_EQ(labels, (std::vector<FString> { "Hero", "Characters" }));
	SFileData bin;
	ASSERT_TRUE(view.GetFile(*view.FindFileExact("/Project/Hero.bin"), bin));
	EXPECT_EQ(bin.Class, EFileClass::Companion);
}

TEST(RegistryStoreTest, ACompactionReclaimsDeadRowsAndPoolsAndRenumbersSubEntries)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 5u, EFileClass::Unclaimed));
	delta.FileUpserts.push_back(MakeFile("/Project/Input/Map.frtinputmap"));
	delta.RecordUpserts.push_back(MakeHero(0u, 100));
	delta.RecordUpserts.push_back(MakeInputMap({ "Jump", "Fire" }));
	AddTexture(delta, "/Project/T/gone.png");
	store.Commit(std::move(delta));
	const SPoolSizes initial = GetPoolSizes(store.Live());

	// Other sub-entries and other actions each time: dead rows and replaced lists pile up.
	for (uint32 i = 1u; i <= 10u; ++i)
	{
		SRegistryDelta change;
		change.RecordUpserts.push_back(MakeHero(i, i));
		change.RecordUpserts.push_back(MakeInputMap({ "Jump", FString::Format("Action{}", i) }));
		store.Commit(std::move(change));
	}
	SRegistryDelta remove;
	remove.FileRemoves.push_back("/Project/T/gone.png");
	store.Commit(std::move(remove));
	EXPECT_GT(GetPoolSizes(store.Live()).SubEntries, initial.SubEntries);
	EXPECT_GT(GetTables(store.Live()).GarbagePoolItems, 0u);
	EXPECT_FALSE(store.NeedsCompaction());   // far below the thresholds

	const std::shared_ptr<const CRegistryView> held = store.Acquire();
	const FString before = Dump(*held);
	const SRecordId oldBody = FindRecord(*held, "/Project/Hero.gltf#mesh:Body10");
	ASSERT_TRUE(oldBody.IsValid());
	store.CompactTables();
	EXPECT_TRUE(store.GetLastCommitStats().bCompactedTables);

	const CRegistryView& view = store.Live();
	EXPECT_EQ(Dump(view), before);
	EXPECT_EQ(Dump(*held), before);   // the held view keeps its columns
	EXPECT_EQ(view.GetRevision(), held->GetRevision() + 1u);
	EXPECT_FALSE(view.IsValid(oldBody));
	EXPECT_EQ(GetPoolSizes(view), (SPoolSizes { 2u, 1u, 2u, 1u, initial.Paths, 2u }));
	EXPECT_EQ(GetTables(view).Types[asset_types::Texture].Records.Size(), 0u);
	EXPECT_EQ(GetTables(view).GarbagePoolItems, 0u);

	const SRecordId body = FindRecord(view, "/Project/Hero.gltf#mesh:Body10");
	ASSERT_TRUE(body.IsValid());
	STagValue vertices;
	ASSERT_TRUE(view.GetTag(body, "VertexCount", vertices));
	EXPECT_EQ(vertices.AsInt64(), 10);
	SAssetData data;
	ASSERT_TRUE(view.GetRecord(body, data));
	EXPECT_EQ(view.Find(data.Id64), body);
	EXPECT_EQ(view.Find(data.Id), body);
	std::vector<FString> actions;
	view.ForEachTagValue(FindRecord(view, "/Project/Input/Map.frtinputmap"), "Actions",
		[&] (const STagValue& Value) { actions.push_back(Value.Text); });
	EXPECT_EQ(actions, (std::vector<FString> { "Jump", "Action10" }));

	// Commits after a compaction append behind the compacted rows.
	SRegistryDelta later;
	later.RecordUpserts.push_back(MakeHero(11u, 11));
	store.Commit(std::move(later));
	EXPECT_TRUE(FindRecord(store.Live(), "/Project/Hero.gltf#mesh:Body11").IsValid());
	EXPECT_FALSE(FindRecord(store.Live(), "/Project/Hero.gltf#mesh:Body10").IsValid());
}

TEST(RegistryStoreTest, EnoughDeadRowsMakeACompactionDueAndCommitsNeverRunIt)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.FileUpserts.push_back(MakeFile("/Project/Big.gltf"));
	store.Commit(std::move(delta));
	// 64 sub-entries under new labels 70 times: some 4500 dead SUBENTRY rows and as many dead mesh rows.
	for (uint32 round = 0u; round < 70u && !store.NeedsCompaction(); ++round)
	{
		SRecordUpsert model = MakeRecord("/Project/Big.gltf", asset_types::Model, "Model.Gltf");
		for (uint32 i = 0u; i < 64u; ++i)
		{
			model.SubEntries.push_back(SSubEntryDecl { FString::Format("mesh:R{}_{}", round, i), 0u, asset_types::Mesh, ERecordFlags::None, {} });
		}
		SRegistryDelta change;
		change.RecordUpserts.push_back(std::move(model));
		store.Commit(std::move(change));
		EXPECT_FALSE(store.GetLastCommitStats().bCompactedTables);
		EXPECT_FALSE(store.GetLastCommitStats().bCompactedGraph);
	}
	ASSERT_TRUE(store.NeedsCompaction());
	const FString before = Dump(store.Live());
	store.Compact();
	EXPECT_TRUE(store.GetLastCommitStats().bCompactedTables);
	EXPECT_FALSE(store.NeedsCompaction());
	EXPECT_EQ(Dump(store.Live()), before);
	EXPECT_EQ(GetTables(store.Live()).SubEntries.Owner.Size(), 64u);
	const uint64 revision = store.GetRevision();
	store.Compact();   // nothing due: no publication, no revision
	EXPECT_EQ(store.GetRevision(), revision);
}


// ---------------- Companions ----------------

TEST(RegistryStoreTest, CompanionsAreClassifiedInEitherCommitOrder)
{
	const auto modelWith = [] (std::initializer_list<FStringView> Companions)
	{
		SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
		for (const FStringView companion : Companions)
		{
			model.Companions.push_back(FString(companion));
		}
		return model;
	};
	const auto classOf = [] (const CRegistryView& View, FStringView Path)
	{
		SFileData data;
		const std::optional<uint32> row = View.FindFileExact(Path);
		return row && View.GetFile(*row, data) ? data.Class : EFileClass::Excluded;
	};

	// File first, then the record that names it.
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 10u, EFileClass::Unclaimed));
	store.Commit(std::move(delta));
	EXPECT_EQ(classOf(store.Live(), "/Project/Hero.bin"), EFileClass::Unclaimed);
	SRegistryDelta record;
	record.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	record.RecordUpserts.push_back(modelWith({ "/Project/hero.BIN" }));   // named under the portable fold
	store.Commit(std::move(record));
	EXPECT_EQ(classOf(store.Live(), "/Project/Hero.bin"), EFileClass::Companion);

	// A rescan of the file keeps it a companion.
	SRegistryDelta rescan;
	rescan.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 11u, EFileClass::Unclaimed));
	store.Commit(std::move(rescan));
	EXPECT_EQ(classOf(store.Live(), "/Project/Hero.bin"), EFileClass::Companion);

	// The record stops naming it: back to what the scan said.
	SRegistryDelta drop;
	drop.RecordUpserts.push_back(modelWith({}));
	store.Commit(std::move(drop));
	EXPECT_EQ(classOf(store.Live(), "/Project/Hero.bin"), EFileClass::Unclaimed);

	// Record first, then the file.
	CRegistryStore reversed;
	SRegistryDelta first = MakeMountsDelta();
	first.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	first.RecordUpserts.push_back(modelWith({ "/Project/Hero.bin" }));
	reversed.Commit(std::move(first));
	SRegistryDelta second;
	second.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 10u, EFileClass::Unclaimed));
	reversed.Commit(std::move(second));
	EXPECT_EQ(classOf(reversed.Live(), "/Project/Hero.bin"), EFileClass::Companion);

	// Removing the record releases it.
	SRegistryDelta remove;
	remove.FileRemoves.push_back("/Project/Hero.gltf");
	reversed.Commit(std::move(remove));
	EXPECT_EQ(classOf(reversed.Live(), "/Project/Hero.bin"), EFileClass::Unclaimed);
}


// ---------------- Folders ----------------

TEST(RegistryStoreTest, FolderIntervalsAreRecomputedAfterAFolderIsAddedMidMount)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/b/x.png");
	AddTexture(delta, "/Project/D/y.png");
	AddTexture(delta, "/Project/b/c/z.png");
	store.Commit(std::move(delta));

	SRegistryDelta add;
	add.FolderUpserts.push_back(SFolderUpsert { "/Project/C" });          // between b and D under the fold
	AddTexture(add, "/Project/b/A/w.png");                                 // before b/c
	store.Commit(std::move(add));

	const CRegistryView& view = store.Live();
	const auto folder = [&] (FStringView Path)
	{
		SFolderData data;
		EXPECT_TRUE(view.GetFolder(view.FindFolder(Path).value_or(InvalidRow), data)) << FString(Path).CStr();
		return data;
	};
	// Pre-order, siblings by (fold, bytes): /Project, b, b/A, b/c, C, D.
	EXPECT_EQ(folder("/Project").PreorderBegin, 0u);
	EXPECT_EQ(folder("/Project").PreorderEnd, 6u);
	EXPECT_EQ(folder("/Project/b").PreorderBegin, 1u);
	EXPECT_EQ(folder("/Project/b").PreorderEnd, 4u);
	EXPECT_EQ(folder("/Project/b/A").PreorderBegin, 2u);
	EXPECT_EQ(folder("/Project/b/c").PreorderBegin, 3u);
	EXPECT_EQ(folder("/Project/C").PreorderBegin, 4u);
	EXPECT_EQ(folder("/Project/C").PreorderEnd, 5u);
	EXPECT_EQ(folder("/Project/D").PreorderBegin, 5u);

	std::vector<FString> under;
	view.ForEachFileUnder(*view.FindFolder("/Project/b"), [&] (uint32 Row)
	{
		SFileData data;
		view.GetFile(Row, data);
		under.push_back(FString(data.MountPath));
	});
	std::sort(under.begin(), under.end());
	EXPECT_EQ(under, (std::vector<FString> { "/Project/b/A/w.png", "/Project/b/c/z.png", "/Project/b/x.png" }));

	// A removal renumbers too.
	SRegistryDelta remove;
	remove.FolderRemoves.push_back("/Project/b");
	store.Commit(std::move(remove));
	SFolderData data;
	ASSERT_TRUE(store.Live().GetFolder(*store.Live().FindFolder("/Project/D"), data));
	EXPECT_EQ(data.PreorderBegin, 2u);
	ASSERT_TRUE(store.Live().GetFolder(*store.Live().FindFolder("/Project"), data));
	EXPECT_EQ(data.PreorderEnd, 3u);
}


// ---------------- Problems ----------------

TEST(RegistryStoreTest, ProblemListsAreReplacedPerSubjectAndSource)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/a.png");
	delta.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Scan, { MakeScanWarning("/Project/T/a.png", "one") } });
	SDiagnostic analyze = MakeScanWarning("/Project/T/a.png", "bad header");
	analyze.Code = 0u;
	analyze.Severity = ESeverity::Error;
	delta.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Analyze, { analyze } });
	store.Commit(std::move(delta));
	const auto countAll = [&] ()
	{
		uint32 count = 0u;
		store.Live().ForEachDiagnostic(FindRecord(store.Live(), "/Project/T/a.png"), [&] (const SDiagnostic&) { ++count; });
		return count;
	};
	EXPECT_EQ(countAll(), 2u);
	SAssetData data;
	ASSERT_TRUE(store.Live().GetRecord(FindRecord(store.Live(), "/Project/T/a.png"), data));
	EXPECT_TRUE(HasAnyFlags(data.Flags, ERecordFlags::HasProblems));
	EXPECT_EQ(GetTables(store.Live()).Assets.Problems[FindRecord(store.Live(), "/Project/T/a.png").Value], 2u);

	// A new Scan list replaces the Scan list only.
	SRegistryDelta rescan;
	rescan.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Scan,
		{ MakeScanWarning("/Project/T/a.png", "two"), MakeScanWarning("/Project/T/a.png", "three") } });
	store.Commit(std::move(rescan));
	EXPECT_EQ(countAll(), 3u);

	// A ProblemClears of the folder clears that source below it; an empty list clears one pair.
	SRegistryDelta clear;
	clear.ProblemClears.push_back(SProblemsClear { "/Project/T", EProblemSource::Scan });
	store.Commit(std::move(clear));
	EXPECT_EQ(countAll(), 1u);
	SRegistryDelta empty;
	empty.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Analyze, {} });
	store.Commit(std::move(empty));
	EXPECT_EQ(countAll(), 0u);
	ASSERT_TRUE(store.Live().GetRecord(FindRecord(store.Live(), "/Project/T/a.png"), data));
	EXPECT_FALSE(HasAnyFlags(data.Flags, ERecordFlags::HasProblems));

	// A sub-entry's problems ("file#label") count for its record.
	SRegistryDelta sub;
	sub.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, {} });
	sub.RecordUpserts.push_back(std::move(model));
	sub.Problems.push_back(SProblemsSet { "/Project/Hero.gltf#mesh:Body", EProblemSource::Analyze, { analyze } });
	store.Commit(std::move(sub));
	const SRecordId hero = FindRecord(store.Live(), "/Project/Hero.gltf");
	ASSERT_TRUE(store.Live().GetRecord(hero, data));
	EXPECT_TRUE(HasAnyFlags(data.Flags, ERecordFlags::HasProblems));
	uint32 heroCount = 0u;
	store.Live().ForEachDiagnostic(hero, [&] (const SDiagnostic&) { ++heroCount; });
	EXPECT_EQ(heroCount, 1u);
}


TEST(RegistryStoreTest, AProblemOnlyDeltaChangesTheRecordWhoseCountItChanges)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/T/a.png");
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, {} });
	delta.RecordUpserts.push_back(std::move(model));
	store.Commit(std::move(delta));
	const auto changesSince = [&] (uint64 Revision)
	{
		std::vector<std::pair<FString, ERecordChange>> changes;
		EXPECT_TRUE(store.ForEachChangeSince(Revision, [&] (const SRecordChange& Change) { changes.emplace_back(Change.MountPath, Change.Kind); }));
		std::sort(changes.begin(), changes.end());
		return changes;
	};
	const auto record = [&] (FStringView Path)
	{
		SAssetData data;
		EXPECT_TRUE(store.Live().GetRecord(FindRecord(store.Live(), Path), data));
		return data;
	};

	// A Scan list on the file and an Analyze list on a sub-entry: both records changed.
	uint64 before = store.GetRevision();
	SRegistryDelta problems;
	problems.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Scan, { MakeScanWarning("/Project/T/a.png", "long") } });
	problems.Problems.push_back(SProblemsSet { "/Project/Hero.gltf#mesh:Body", EProblemSource::Analyze,
		{ MakeScanWarning("/Project/Hero.gltf#mesh:Body", "degenerate") } });
	store.Commit(std::move(problems));
	EXPECT_EQ(changesSince(before), (std::vector<std::pair<FString, ERecordChange>> {
		{ "/Project/Hero.gltf", ERecordChange::Modified }, { "/Project/T/a.png", ERecordChange::Modified } }));
	EXPECT_TRUE(HasAnyFlags(record("/Project/T/a.png").Flags, ERecordFlags::HasProblems));
	EXPECT_EQ(record("/Project/T/a.png").Revision, store.GetRevision());
	EXPECT_EQ(record("/Project/Hero.gltf").Revision, store.GetRevision());

	// The same list again changes nothing.
	before = store.GetRevision();
	SRegistryDelta same;
	same.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Scan, { MakeScanWarning("/Project/T/a.png", "long") } });
	store.Commit(std::move(same));
	EXPECT_TRUE(changesSince(before).empty());

	// Clearing it changes the record back.
	before = store.GetRevision();
	SRegistryDelta clear;
	clear.Problems.push_back(SProblemsSet { "/Project/T/a.png", EProblemSource::Scan, {} });
	store.Commit(std::move(clear));
	EXPECT_EQ(changesSince(before), (std::vector<std::pair<FString, ERecordChange>> { { "/Project/T/a.png", ERecordChange::Modified } }));
	EXPECT_FALSE(HasAnyFlags(record("/Project/T/a.png").Flags, ERecordFlags::HasProblems));
}


// ---------------- Contract violations ----------------

TEST(RegistryStoreTest, ContractViolationsAreSkippedAndReported)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.Mounts.push_back(MakeMount("Project", 5u));                  // a known name with another id
	delta.Mounts.push_back(MakeMount("Plugin.Trees", ProjectMountId)); // a known id with another name
	delta.FileUpserts.push_back(MakeFile("/Nowhere/a.png"));           // no such mount
	delta.FileUpserts.push_back(MakeFile("/Project/../a.png"));        // malformed
	delta.FileUpserts.push_back(MakeFile("/Project/a.png"));
	SRecordUpsert record = MakeRecord("/Project/a.png");
	record.Tags.push_back(MakeIntTag("Width", 4));
	record.Tags.push_back(MakeIntTag("Depth", 4));                     // not a key of frt.Texture
	record.Tags.push_back(STagDecl { "Height", STagValue::FromBool(true) });   // wrong type
	record.SubEntries.push_back(SSubEntryDecl { "image:a", 0u, asset_types::Texture, ERecordFlags::None, {} });
	record.SubEntries.push_back(SSubEntryDecl { "image:a", 0u, asset_types::Texture, ERecordFlags::None, {} });   // same SubId
	SEdgeDecl fromNowhere = MakePathEdge("/Project/b.png");
	fromNowhere.FromSub = 12345u;
	record.Edges.push_back(fromNowhere);
	delta.RecordUpserts.push_back(std::move(record));
	store.Commit(std::move(delta));

	const CRegistryView& view = store.Live();
	EXPECT_EQ(view.GetMounts().size(), 2u);
	EXPECT_EQ(view.FindMount("Project")->Id, ProjectMountId);
	EXPECT_EQ(view.GetFileCount(), 1u);
	EXPECT_EQ(CountWithCode(view, "/Project", 0u), 2u);
	EXPECT_EQ(CountWithCode(view, "/Nowhere/a.png", 0u), 1u);
	const SRecordId a = FindRecord(view, "/Project/a.png");
	STagValue value;
	EXPECT_TRUE(view.GetTag(a, "Width", value));
	EXPECT_FALSE(view.GetTag(a, "Height", value));
	EXPECT_EQ(view.GetSubEntryCount(), 1u);
	EXPECT_TRUE(GetDependencies(view, a).empty());
	EXPECT_EQ(GetDiagnostics(view, a, 0u).size(), 4u);

	// A clean upsert of the record drops its old contract problems.
	SRegistryDelta clean;
	AddTexture(clean, "/Project/a.png");
	store.Commit(std::move(clean));
	EXPECT_TRUE(GetDiagnostics(store.Live(), FindRecord(store.Live(), "/Project/a.png"), 0u).empty());
}


// ---------------- Change ring ----------------

TEST(RegistryStoreTest, TheChangeRingReportsOneEntryPerRecordTouched)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/a.png");
	AddTexture(delta, "/Project/b.png");
	store.Commit(std::move(delta));
	const uint64 afterAdd = store.GetRevision();

	SRegistryDelta edit;
	AddTexture(edit, "/Project/a.png", 1, 1);
	edit.FileRemoves.push_back("/Project/b.png");
	AddTexture(edit, "/Project/c.png");
	store.Commit(std::move(edit));

	std::vector<std::pair<FString, ERecordChange>> changes;
	EXPECT_TRUE(store.ForEachChangeSince(afterAdd, [&] (const SRecordChange& Change)
	{
		EXPECT_EQ(Change.Revision, afterAdd + 1u);
		changes.emplace_back(Change.MountPath, Change.Kind);
	}));
	std::sort(changes.begin(), changes.end());
	EXPECT_EQ(changes, (std::vector<std::pair<FString, ERecordChange>> {
		{ "/Project/a.png", ERecordChange::Modified }, { "/Project/b.png", ERecordChange::Removed }, { "/Project/c.png", ERecordChange::Added } }));

	uint32 all = 0u;
	EXPECT_TRUE(store.ForEachChangeSince(0u, [&] (const SRecordChange&) { ++all; }));
	EXPECT_EQ(all, 5u);
}

TEST(RegistryStoreTest, AnOverflowingChangeRingAsksForAFullRefresh)
{
	CRegistryStore store;
	store.Commit(MakeMountsDelta());
	const uint64 start = store.GetRevision();
	SRegistryDelta big;
	for (uint32 i = 0u; i < CRegistryStore::ChangeRingCapacity + 10u; ++i)
	{
		big.RecordUpserts.push_back(MakeRecord(FString::Format("/Project/F{}/r{}.png", i % 64u, i)));
	}
	store.Commit(std::move(big));
	EXPECT_FALSE(store.ForEachChangeSince(start, [] (const SRecordChange&) {}));
	SRegistryDelta single;
	AddTexture(single, "/Project/one.png");
	store.Commit(std::move(single));
	uint32 count = 0u;
	EXPECT_TRUE(store.ForEachChangeSince(start + 1u, [&] (const SRecordChange&) { ++count; }));
	EXPECT_EQ(count, 1u);
}


// ---------------- Images ----------------

TEST(RegistryStoreTest, AnExportedImageAdoptedByAFreshStoreDumpsIdentically)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	delta.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId));
	AddTexture(delta, "/Engine/Textures/White.png");
	AddTexture(delta, "/Project/T/Rock.png");
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Engine/Textures/White.png", "/Project/t/rock.png", "/Project/T/gone.png" });
	AddMaterial(delta, "/Plugin.Trees/Oak.frtmat.yml", { "/Project/T/Rock.png" });
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 5u, EFileClass::Unclaimed));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.Companions.push_back("/Project/Hero.bin");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 9) } });
	model.RuleLabels = { "Hero" };
	model.AnalyzeStamp = 0x1234u;
	delta.RecordUpserts.push_back(std::move(model));
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "", 11u });
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/T", 12u });
	delta.FolderUpserts.push_back(SFolderUpsert { "/Project/Empty" });
	delta.Problems.push_back(SProblemsSet { "/Project/T/Rock.png", EProblemSource::Scan, { MakeScanWarning("/Project/T/Rock.png", "long") } });
	store.Commit(std::move(delta));
	const FString original = Dump(store.Live());

	CRegistryStore adopted;
	adopted.AdoptImage(store.Live().ExportImage({}), 42u);
	EXPECT_EQ(adopted.GetRevision(), 42u);
	EXPECT_EQ(Dump(adopted.Live()), original);
	EXPECT_FALSE(adopted.ForEachChangeSince(10u, [] (const SRecordChange&) {}));

	// The engine and the project's mounts as two images, loaded one after the other.
	const MountId engine[] = { EngineMountId };
	const MountId project[] = { ProjectMountId, FirstPluginMountId };
	SRegistryImage engineImage = store.Live().ExportImage(engine);
	SRegistryImage projectImage = store.Live().ExportImage(project);
	EXPECT_EQ(engineImage.Mounts.size(), 1u);
	EXPECT_EQ(projectImage.Mounts.size(), 2u);
	CRegistryStore merged;
	merged.AdoptImage(std::move(projectImage), 7u);   // the project first: its edge into /Engine waits
	merged.AdoptImage(std::move(engineImage), 8u);
	EXPECT_EQ(Dump(merged.Live()), original);
}

namespace
{
// A store with everything an image carries: three mounts (one case-sensitive), nested folders, rule files,
// problems, companions, rule labels, sub-entries with tags, MultiValue tags, an unknown importer name,
// edges from sub-entries, by id, unresolved, spelled in another case (REF006) and ambiguous (PATH001).
SRegistryDelta MakeRichContent ()
{
	SRegistryDelta delta = MakeMountsDelta();
	delta.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, true));
	AddTexture(delta, "/Engine/Textures/White.png", 4, 4);
	AddTexture(delta, "/Project/T/Rock.png", 1, 1);
	AddTexture(delta, "/Project/T/rock.png", 2, 2);
	AddTexture(delta, "/Project/T/Deep/Er/Grass.png", 8, 8);
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Engine/textures/white.png", "/Project/T/ROCK.png", "/Project/T/gone.png" });
	AddMaterial(delta, "/Plugin.Trees/Oak.frtmat.yml", { "/Project/T/Deep/Er/Grass.png", "/Plugin.Trees/Bark.png" });
	AddTexture(delta, "/Plugin.Trees/Bark.png");
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.gltf"));
	delta.FileUpserts.push_back(MakeFile("/Project/Hero.bin", 5u, EFileClass::Unclaimed));
	delta.FileUpserts.push_back(MakeFile("/Project/notes.txt", 7u, EFileClass::Unclaimed));
	SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.Companions.push_back("/Project/Hero.bin");
	model.RuleLabels = { "Hero", "Characters" };
	model.AnalyzeStamp = 0x1234u;
	model.Tags = { MakeIntTag("MeshCount", 1) };
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 9) } });
	model.SubEntries.push_back(SSubEntryDecl { "material:Skin", 0u, asset_types::Material, ERecordFlags::None, {} });
	model.Edges.push_back(MakePathEdge("/Project/Hero.bin", EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared, "buffers[0].uri"));
	SEdgeDecl skin = MakePathEdge("/Project/M/m.frtmat.yml", EDepFlags::Hard, "materials[0]");
	skin.FromSub = MakeSubId("material:Skin");
	model.Edges.push_back(skin);
	SEdgeDecl byId;
	byId.Target = SAssetId { MakeProvisionalGuid("/Plugin.Trees/Oak.frtmat.yml"), 0u };
	byId.Flags = EDepFlags::Hard | EDepFlags::Declared;
	model.Edges.push_back(byId);
	SEdgeDecl missingId;
	missingId.Target = SAssetId { MakeProvisionalGuid("/Project/Nowhere.png"), 0u };
	missingId.TargetPath = "/Project/Nowhere.png";
	model.Edges.push_back(missingId);
	delta.RecordUpserts.push_back(std::move(model));
	delta.FileUpserts.push_back(MakeFile("/Project/Input/Map.frtinputmap"));
	SRecordUpsert map = MakeRecord("/Project/Input/Map.frtinputmap", asset_types::InputActionMap, "Input.Legacy");
	map.Tags = { STagDecl { "Actions", STagValue::FromName("Jump") }, STagDecl { "Actions", STagValue::FromName("Fire") } };
	map.Flags = ERecordFlags::Primary;
	map.PrimaryName = "Map";
	delta.RecordUpserts.push_back(std::move(map));
	delta.FileUpserts.push_back(MakeFile("/Project/Future.frt"));
	SRecordUpsert future = MakeRecord("/Project/Future.frt", asset_types::Raw, "Document");
	future.ImportStatus = EImportStatus::ImporterUnavailable;
	delta.RecordUpserts.push_back(std::move(future));
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "", 11u });
	delta.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/T", 12u });
	delta.FolderUpserts.push_back(SFolderUpsert { "/Project/Empty/Inner" });
	delta.Problems.push_back(SProblemsSet { "/Project/T/Rock.png", EProblemSource::Scan, { MakeScanWarning("/Project/T/Rock.png", "long") } });
	delta.Problems.push_back(SProblemsSet { "/Project/Hero.gltf#mesh:Body", EProblemSource::Analyze,
		{ MakeScanWarning("/Project/Hero.gltf#mesh:Body", "degenerate") } });
	return delta;
}

// What a dump does not show: lookups under each mount's rule, referencers, folded paths, file classes.
void ExpectSameAnswers (const CRegistryView& A, const CRegistryView& B)
{
	EXPECT_EQ(Dump(A), Dump(B));
	EXPECT_EQ(A.GetRecordCount(), B.GetRecordCount());
	EXPECT_EQ(A.GetSubEntryCount(), B.GetSubEntryCount());
	EXPECT_EQ(A.GetFileCount(), B.GetFileCount());
	EXPECT_EQ(A.GetFolderCount(), B.GetFolderCount());
	A.ForEachFile([&] (uint32 Row)
	{
		SFileData fileA;
		SFileData fileB;
		ASSERT_TRUE(A.GetFile(Row, fileA));
		const std::optional<uint32> rowB = B.FindFileExact(fileA.MountPath);
		ASSERT_TRUE(rowB.has_value()) << FString(fileA.MountPath).CStr();
		ASSERT_TRUE(B.GetFile(*rowB, fileB));
		EXPECT_EQ(fileA.Class, fileB.Class);
		EXPECT_EQ(GetFoldedFilePath(GetTables(A), Row), GetFoldedFilePath(GetTables(B), *rowB));
		FString upper(fileA.MountPath);
		upper = upper.ToUpper();
		EXPECT_EQ(A.FindFile(upper).has_value(), B.FindFile(upper).has_value()) << upper.CStr();
	});
	A.ForEachRecord([&] (SRecordId Record)
	{
		SAssetData data;
		ASSERT_TRUE(A.GetRecord(Record, data));
		const SRecordId other = B.FindByPath(data.MountPath).value_or(SRecordId {});
		if (!other.IsValid())
		{
			EXPECT_FALSE(A.FindByPath(data.MountPath).has_value());   // PATH001: neither finds it
			return;
		}
		EXPECT_EQ(GetReferencers(A, Record).size(), GetReferencers(B, other).size()) << data.MountPath.CStr();
		EXPECT_EQ(B.Find(data.Id), other);
		EXPECT_EQ(B.Find(data.Id64), other);
		std::vector<SRecordId> closureA;
		std::vector<SRecordId> closureB;
		const SRecordId rootA[] = { Record };
		const SRecordId rootB[] = { other };
		A.GetReferencerClosure(rootA, EDepFlags {}, closureA);
		B.GetReferencerClosure(rootB, EDepFlags {}, closureB);
		EXPECT_EQ(closureA.size(), closureB.size()) << data.MountPath.CStr();
	});
}
}

TEST(RegistryStoreTest, AFreshStoreAdoptsAnImageInBulkWithTheReplaysResult)
{
	CRegistryStore original;
	original.Commit(MakeRichContent());

	CRegistryStore bulk;
	bulk.AdoptImage(original.Live().ExportImage({}), 9u);
	EXPECT_FALSE(bulk.GetLastCommitStats().bReplayedImage);
	EXPECT_EQ(bulk.GetRevision(), 9u);

	// A store that holds the mounts already replays the image as a delta.
	CRegistryStore replayed;
	SRegistryDelta mounts = MakeMountsDelta();
	mounts.Mounts.push_back(MakeMount("Plugin.Trees", FirstPluginMountId, true));
	replayed.Commit(std::move(mounts));
	replayed.AdoptImage(original.Live().ExportImage({}), 9u);
	EXPECT_TRUE(replayed.GetLastCommitStats().bReplayedImage);

	ExpectSameAnswers(bulk.Live(), original.Live());
	ExpectSameAnswers(bulk.Live(), replayed.Live());
	EXPECT_FALSE(bulk.NeedsCompaction());
	EXPECT_EQ(GetTables(bulk.Live()).Graph.GarbageEdges, 0u);
	EXPECT_EQ(GetTables(bulk.Live()).Graph.LiveEdges, GetTables(replayed.Live()).Graph.LiveEdges);

	// Both go on the same way: a missing target arrives, a record changes its sub-entries, a folder goes.
	const auto edit = [] (CRegistryStore& Store)
	{
		SRegistryDelta delta;
		AddTexture(delta, "/Project/T/gone.png");
		AddTexture(delta, "/Project/Nowhere.png");
		SRecordUpsert model = MakeRecord("/Project/Hero.gltf", asset_types::Model, "Model.Gltf");
		model.SubEntries.push_back(SSubEntryDecl { "mesh:Head", 0u, asset_types::Mesh, ERecordFlags::None, { MakeIntTag("VertexCount", 3) } });
		delta.RecordUpserts.push_back(std::move(model));
		delta.FolderRemoves.push_back("/Project/T/Deep");
		Store.Commit(std::move(delta));
	};
	edit(bulk);
	edit(replayed);
	edit(original);
	ExpectSameAnswers(bulk.Live(), replayed.Live());
	ExpectSameAnswers(bulk.Live(), original.Live());
	bulk.CompactTables();
	ExpectSameAnswers(bulk.Live(), original.Live());
}

TEST(RegistryStoreTest, AnImageTheBulkPathDoesNotTakeIsReplayedAndReported)
{
	CRegistryStore original;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/a.png", 3, 4);
	original.Commit(std::move(delta));
	SRegistryImage image = original.Live().ExportImage({});
	ASSERT_EQ(image.Tags.size(), 2u);
	image.Tags[1].Type = ETagType::Bool;   // Height as a Bool: the replay drops it with a contract problem

	CRegistryStore adopted;
	adopted.AdoptImage(std::move(image), 1u);
	EXPECT_TRUE(adopted.GetLastCommitStats().bReplayedImage);
	const SRecordId a = FindRecord(adopted.Live(), "/Project/a.png");
	STagValue value;
	EXPECT_TRUE(adopted.Live().GetTag(a, "Width", value));
	EXPECT_FALSE(adopted.Live().GetTag(a, "Height", value));
	EXPECT_EQ(GetDiagnostics(adopted.Live(), a, 0u).size(), 1u);
}

TEST(RegistryStoreTest, EdgesByIdResolveInEitherOrderAndLeaveAnImageByPath)
{
	// An edge that names its target by SAssetId (a .meta reference from M4 on) waits unresolved until a
	// record with that GUID arrives, then points at the sub-entry its SubId names.
	const SGuid heroGuid = MakeProvisionalGuid("/Engine/Hero.gltf");
	const SAssetId body { heroGuid, MakeSubId("mesh:Body") };
	CRegistryStore store;
	SRegistryDelta first = MakeMountsDelta();
	first.FileUpserts.push_back(MakeFile("/Project/User.frtmat.yml"));
	SRecordUpsert user = MakeRecord("/Project/User.frtmat.yml", asset_types::Material, "Material.Legacy");
	SEdgeDecl byId;
	byId.Target = body;
	byId.Flags = EDepFlags::Hard | EDepFlags::Declared;
	byId.FieldPath = "mesh";
	user.Edges.push_back(byId);
	first.RecordUpserts.push_back(std::move(user));
	store.Commit(std::move(first));
	std::vector<SDependencyView> edges = GetDependencies(store.Live(), FindRecord(store.Live(), "/Project/User.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_TRUE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	EXPECT_EQ(edges[0].TargetId, body);

	SRegistryDelta second;
	second.FileUpserts.push_back(MakeFile("/Engine/Hero.gltf"));
	SRecordUpsert model = MakeRecord("/Engine/Hero.gltf", asset_types::Model, "Model.Gltf");
	model.SubEntries.push_back(SSubEntryDecl { "mesh:Body", 0u, asset_types::Mesh, ERecordFlags::None, {} });
	second.RecordUpserts.push_back(std::move(model));
	store.Commit(std::move(second));
	const CRegistryView& view = store.Live();
	edges = GetDependencies(view, FindRecord(view, "/Project/User.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_FALSE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	EXPECT_EQ(edges[0].To, FindRecord(view, "/Engine/Hero.gltf#mesh:Body"));
	EXPECT_EQ(GetReferencers(view, FindRecord(view, "/Engine/Hero.gltf#mesh:Body")).size(), 1u);

	// The project's image names the engine record by its mount path, and resolves again once both are loaded.
	const MountId project[] = { ProjectMountId };
	const MountId engine[] = { EngineMountId };
	SRegistryImage projectImage = view.ExportImage(project);
	ASSERT_EQ(projectImage.Edges.size(), 1u);
	EXPECT_FALSE(projectImage.Edges[0].Target.IsValid());
	ASSERT_LT(projectImage.Edges[0].TargetPath, projectImage.Strings.size());
	EXPECT_EQ(projectImage.Strings[projectImage.Edges[0].TargetPath], "/Engine/Hero.gltf#mesh:Body");
	CRegistryStore merged;
	merged.AdoptImage(std::move(projectImage), 1u);
	edges = GetDependencies(merged.Live(), FindRecord(merged.Live(), "/Project/User.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_TRUE(HasAnyFlags(edges[0].Flags, EDepFlags::Unresolved));
	merged.AdoptImage(view.ExportImage(engine), 2u);
	edges = GetDependencies(merged.Live(), FindRecord(merged.Live(), "/Project/User.frtmat.yml"));
	ASSERT_EQ(edges.size(), 1u);
	EXPECT_EQ(edges[0].To, FindRecord(merged.Live(), "/Engine/Hero.gltf#mesh:Body"));
	EXPECT_EQ(edges[0].TargetId, body);
}

TEST(RegistryStoreTest, AnImageRecordOfAnUnknownTypeIsKeptSchemalessWithStaleTags)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/a.png", 3, 4);
	store.Commit(std::move(delta));
	SRegistryImage image = store.Live().ExportImage({});
	ASSERT_EQ(image.TypeNames.size(), 1u);
	image.TypeNames[0] = "Game.Vanished";

	CRegistryStore adopted;
	adopted.AdoptImage(std::move(image), 1u);
	SAssetData data;
	ASSERT_TRUE(adopted.Live().GetRecord(FindRecord(adopted.Live(), "/Project/a.png"), data));
	EXPECT_EQ(data.Type, asset_types::Unknown);
	EXPECT_TRUE(HasAllFlags(data.Flags, ERecordFlags::Schemaless | ERecordFlags::StaleTags));
	STagValue value;
	EXPECT_FALSE(adopted.Live().GetTag(data.Record, "Width", value));
}
