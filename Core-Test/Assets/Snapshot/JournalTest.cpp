// The registry journal (Assets/Registry/Journal.h, design §5.3). Every member of a delta survives its encoding;
// a snapshot plus its journal loads the latest committed state; a journal keeps only its snapshot's mounts; a
// torn tail is dropped at load and cut off before the next append, and so is a commit whose delta does not decode;
// a journal that continues another snapshot is ignored; after a full write, Restart keeps exactly the commits the
// snapshot's view did not hold; a failed append breaks the journal until a full write, and loses no commit; a delta
// larger than one frame is split and joined again; types are stored by name, and a name this build does not know
// waits Schemaless; a snapshot that lost its rule outputs replays no rule hashes.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "Assets/AssetTypes.h"
#include "Assets/RecordFrames.h"
#include "SnapshotTestUtils.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::snapshot_test;


namespace frt::assets
{
// CRegistryJournal's test seam (a friend): the next Append writes only Bytes of its frames and fails, as a full
// disk cuts a write short.
struct SRegistryJournalTestAccess
{
	static void FailNextAppendAfter (CRegistryJournal& Journal, size_t Bytes) { Journal.FailAppendAfterBytes = Bytes; }
};
}


namespace
{
// A delta that sets every member, removes and clears included.
SRegistryDelta MakeEveryMember ()
{
	SRegistryDelta delta = MakeContent();
	delta.Sequence = 77u;
	delta.FolderRemoves = { "/Project/Old" };
	delta.FileRemoves = { "/Project/T/old.png" };
	delta.RecordRemoves = { "/Project/T/older.png" };
	delta.RuleFileRemoves = { SRuleFileUpsert { "Project", "/Project/Old", 5u } };
	delta.ProblemClears = { SProblemsClear { "/Project/T", EProblemSource::Rules } };
	delta.FileUpserts[0].ContentHash.reset();
	delta.FileUpserts[1].MTime = -42;
	SDiagnostic full = MakeWarning("/Project/T/Rock.png", "every field", DiagnosticCode("REF007"));
	full.File = "Content/T/Assets.yml";
	full.Line = 3u;
	full.Column = 9u;
	full.FieldPath = "Rules[0].Match";
	delta.Problems.push_back(SProblemsSet { "/Project/T/Rock.png", EProblemSource::Analyze, { full } });
	delta.RecordUpserts[0].Tags.push_back(STagDecl { "Channels", STagValue::FromInt64(-3) });
	return delta;
}

std::vector<uint8> Encode (const SRegistryDelta& Delta)
{
	std::vector<uint8> bytes;
	EncodeRegistryDelta(Delta, bytes);
	return bytes;
}

// A material edit as F3 would commit it after a re-analyze.
SRegistryDelta MakeEdit (uint32 I)
{
	SRegistryDelta delta;
	delta.Sequence = I;
	AddMaterial(delta, "/Project/M/m.frtmat.yml", { "/Project/T/Rock.png" });
	AddTexture(delta, FString::Format("/Project/T/New{}.png", I), 8 + I, 8);
	return delta;
}

// Commits a delta the way F3 does: journal first, at the revision the commit will get.
void CommitJournaled (CRegistryStore& Store, CRegistryJournal& Journal, SRegistryDelta&& Delta)
{
	FString error;
	ASSERT_TRUE(Journal.Append(Delta, Store.GetRevision() + 1u, &error)) << error.CStr();
	Store.Commit(std::move(Delta));
}

class JournalTest : public SnapshotTest
{
protected:
	void SetUp () override
	{
		SnapshotTest::SetUp();
		Snapshot = Dir / "AssetRegistry.snapshot";
		Journal = GetJournalPath(Snapshot);
		Store.Commit(MakeContent());
		ASSERT_TRUE(WriteSnapshot(Store.Live(), {}, Snapshot, ProjectId));
	}

	SSnapshotLoadResult Load (CRegistryStore& Into)
	{
		const SSnapshotSource sources[] = { SSnapshotSource { Snapshot, ProjectId } };
		return LoadSnapshots(Into, sources);
	}

	std::filesystem::path Snapshot;
	std::filesystem::path Journal;
	SGuid ProjectId = SGuid::NewRandom();
	CRegistryStore Store;
};
}


// ---------------- Encoding ----------------

TEST(JournalEncodingTest, EveryMemberOfADeltaSurvivesItsEncoding)
{
	const SRegistryDelta delta = MakeEveryMember();
	const std::vector<uint8> bytes = Encode(delta);
	const std::optional<SRegistryDelta> decoded = DecodeRegistryDelta(bytes);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(Encode(*decoded), bytes);
	EXPECT_EQ(decoded->Sequence, 77u);
	EXPECT_EQ(decoded->Mounts.size(), delta.Mounts.size());
	EXPECT_TRUE(decoded->Mounts[2].bCaseSensitive);
	EXPECT_FALSE(decoded->FileUpserts[0].ContentHash.has_value());
	EXPECT_EQ(decoded->FileUpserts[1].MTime, -42);
	EXPECT_EQ(decoded->FileUpserts[1].Identity, delta.FileUpserts[1].Identity);
	EXPECT_EQ(decoded->FileUpserts[1].ContentHash, delta.FileUpserts[1].ContentHash);
	EXPECT_EQ(decoded->Problems.back().Diagnostics, delta.Problems.back().Diagnostics);
	EXPECT_EQ(decoded->RecordUpserts[0].Tags.back().Value, STagValue::FromInt64(-3));

	// Both stores answer alike.
	CRegistryStore a;
	CRegistryStore b;
	a.Commit(MakeEveryMember());
	b.Commit(SRegistryDelta(*decoded));
	EXPECT_EQ(Dump(a.Live()), Dump(b.Live()));

	// Cut short or followed by anything, it is refused.
	EXPECT_FALSE(DecodeRegistryDelta(std::span<const uint8>(bytes).first(bytes.size() - 1u)).has_value());
	std::vector<uint8> longer = bytes;
	longer.push_back(0u);
	EXPECT_FALSE(DecodeRegistryDelta(longer).has_value());
	for (size_t cut = 0u; cut < bytes.size(); cut += 97u)
	{
		EXPECT_FALSE(DecodeRegistryDelta(std::span<const uint8>(bytes).first(cut)).has_value()) << cut;
	}
}

namespace
{
// Replaces a length-prefixed text in encoded bytes by another of the same length.
void ReplaceText (std::vector<uint8>& Bytes, FStringView From, FStringView To)
{
	ASSERT_EQ(From.Len(), To.Len());
	std::vector<uint8> needle { static_cast<uint8>(From.Len()) };
	needle.insert(needle.end(), From.AsBytes().begin(), From.AsBytes().end());
	const auto found = std::search(Bytes.begin(), Bytes.end(), needle.begin(), needle.end());
	ASSERT_NE(found, Bytes.end());
	std::copy(To.AsBytes().begin(), To.AsBytes().end(), found + 1);
	EXPECT_EQ(std::search(found + 1, Bytes.end(), needle.begin(), needle.end()), Bytes.end()) << "stored once per delta";
}

const SRecordUpsert& FindUpsert (const SRegistryDelta& Delta, FStringView Path)
{
	const auto found = std::find_if(Delta.RecordUpserts.begin(), Delta.RecordUpserts.end(), [&] (const SRecordUpsert& Record)
	{
		return Record.FileMountPath.View() == Path;
	});
	EXPECT_NE(found, Delta.RecordUpserts.end());
	return found != Delta.RecordUpserts.end() ? *found : Delta.RecordUpserts.front();
}
}

TEST(JournalEncodingTest, TypesAreStoredByNameAndAnUnknownNameWaitsSchemaless)
{
	// The bytes name the types (never a TypeId, which another build numbers differently). Another build's names:
	// the action's type and the model's mesh sub-entry type are unknown here.
	std::vector<uint8> bytes = Encode(MakeContent());
	ReplaceText(bytes, "frt.InputAction", "Game.WeaponType");
	ReplaceText(bytes, "frt.Mesh", "Game.Gun");
	const std::optional<SRegistryDelta> decoded = DecodeRegistryDelta(bytes);
	ASSERT_TRUE(decoded.has_value());

	const SRecordUpsert& action = FindUpsert(*decoded, "/Project/Input/Jump.frtinputaction");
	EXPECT_EQ(action.Type, asset_types::Unknown);
	EXPECT_TRUE(HasAllFlags(action.Flags, ERecordFlags::Schemaless | ERecordFlags::StaleTags));
	EXPECT_TRUE(action.Tags.empty());
	const SRecordUpsert& map = FindUpsert(*decoded, "/Project/Input/Map.frtinputmap");
	EXPECT_EQ(map.Type, asset_types::InputActionMap);
	EXPECT_FALSE(HasAnyFlags(map.Flags, ERecordFlags::Schemaless | ERecordFlags::StaleTags));
	EXPECT_EQ(map.Tags.size(), 2u);

	// A sub-entry of an unknown type waits too, and its owner is analyzed again; its other sub-entries keep theirs.
	const SRecordUpsert& model = FindUpsert(*decoded, "/Project/Hero.gltf");
	EXPECT_EQ(model.Type, asset_types::Model);
	EXPECT_TRUE(HasAnyFlags(model.Flags, ERecordFlags::StaleTags));
	EXPECT_FALSE(HasAnyFlags(model.Flags, ERecordFlags::Schemaless));
	EXPECT_EQ(model.Tags.size(), 2u);
	ASSERT_EQ(model.SubEntries.size(), 3u);
	EXPECT_EQ(model.SubEntries[0].Type, asset_types::Unknown);
	EXPECT_TRUE(HasAllFlags(model.SubEntries[0].Flags, ERecordFlags::Schemaless | ERecordFlags::StaleTags));
	EXPECT_TRUE(model.SubEntries[0].Tags.empty());
	EXPECT_EQ(model.SubEntries[2].Type, asset_types::Texture);
	EXPECT_EQ(model.SubEntries[2].Tags.size(), 1u);

	// The store takes the decoded delta: the action waits for its type.
	CRegistryStore store;
	store.Commit(SRegistryDelta(*decoded));
	const SAssetData data = GetData(store.Live(), "/Project/Input/Jump.frtinputaction");
	EXPECT_EQ(data.Type, asset_types::Unknown);
	EXPECT_TRUE(HasAllFlags(data.Flags, ERecordFlags::Schemaless | ERecordFlags::StaleTags));
}

TEST(JournalEncodingTest, ADeltaSplitByMountKeepsEachPartWhole)
{
	const SRegistryDelta delta = MakeEveryMember();
	const SRegistryDelta project = FilterDeltaByMount(delta, [] (FStringView Mount) { return Mount == "Project"; });
	const SRegistryDelta others = FilterDeltaByMount(delta, [] (FStringView Mount) { return Mount != "Project"; });
	EXPECT_EQ(project.Sequence, delta.Sequence);
	EXPECT_EQ(project.Mounts.size(), 1u);
	EXPECT_EQ(project.FileUpserts.size() + others.FileUpserts.size(), delta.FileUpserts.size());
	EXPECT_EQ(project.RecordUpserts.size() + others.RecordUpserts.size(), delta.RecordUpserts.size());
	EXPECT_EQ(project.Problems.size() + others.Problems.size(), delta.Problems.size());
	EXPECT_EQ(project.RuleFileUpserts.size() + others.RuleFileUpserts.size(), delta.RuleFileUpserts.size());
	EXPECT_EQ(project.ProblemClears.size(), 1u);
	for (const SRecordUpsert& record : others.RecordUpserts)
	{
		EXPECT_FALSE(record.FileMountPath.StartsWith("/Project/")) << record.FileMountPath.CStr();
	}
	// A part keeps its records whole: the model's edge into another mount stays with it.
	const auto model = std::find_if(project.RecordUpserts.begin(), project.RecordUpserts.end(),
		[] (const SRecordUpsert& Record) { return Record.FileMountPath == "/Project/Hero.gltf"; });
	ASSERT_NE(model, project.RecordUpserts.end());
	EXPECT_EQ(model->Edges.size(), 4u);
}


// ---------------- Load = snapshot + journal ----------------

TEST_F(JournalTest, ASnapshotPlusItsJournalLoadsTheLatestState)
{
	CRegistryJournal journal;
	FString error;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}, &error)) << error.CStr();
	for (uint32 i = 0u; i < 3u; ++i)
	{
		CommitJournaled(Store, journal, MakeEdit(i));
	}
	SRegistryDelta removal;
	removal.FileRemoves.push_back("/Project/notes.txt");
	removal.FolderRemoves.push_back("/Project/Empty");
	CommitJournaled(Store, journal, std::move(removal));
	CommitJournaled(Store, journal, SRegistryDelta {});   // nothing to write
	EXPECT_EQ(journal.GetStats().Commits, 4u);
	journal.Close();

	CRegistryStore loaded;
	const SSnapshotLoadResult result = Load(loaded);
	const SSnapshotFileReport& report = result.Files[0];
	EXPECT_TRUE(report.bJournalFound);
	EXPECT_TRUE(report.bJournalUsed) << report.JournalMessage.CStr();
	EXPECT_FALSE(report.bJournalTornTail);
	EXPECT_EQ(report.JournalCommits, 4u);
	ExpectSameContent(Store.Live(), loaded.Live());
}

TEST_F(JournalTest, AJournalKeepsOnlyItsSnapshotsMounts)
{
	// The project's snapshot and journal: /Engine's part of a commit is not written there.
	const MountId projectMounts[] = { ProjectMountId, FirstPluginMountId };
	ASSERT_TRUE(WriteSnapshot(Store.Live(), projectMounts, Snapshot, ProjectId));
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), { "Project", "Plugin.Trees" }));
	SRegistryDelta both = MakeEdit(1u);
	AddTexture(both, "/Engine/Textures/Black.png");
	CommitJournaled(Store, journal, std::move(both));
	SRegistryDelta engineOnly;
	AddTexture(engineOnly, "/Engine/Textures/Grey.png");
	CommitJournaled(Store, journal, std::move(engineOnly));
	EXPECT_EQ(journal.GetStats().Commits, 1u);
	journal.Close();

	const SJournalContents contents = ReadJournal(Journal);
	ASSERT_EQ(contents.Deltas.size(), 1u);
	for (const SRecordUpsert& record : contents.Deltas[0].RecordUpserts)
	{
		EXPECT_FALSE(record.FileMountPath.StartsWith("/Engine/")) << record.FileMountPath.CStr();
	}

	CRegistryStore loaded;
	ASSERT_TRUE(Load(loaded).AnyLoaded());
	EXPECT_TRUE(FindRecord(loaded.Live(), "/Project/T/New1.png").IsValid());
	EXPECT_FALSE(loaded.Live().FindMount("Engine").has_value());
}

TEST_F(JournalTest, ATornTailIsDroppedAndCutOffBeforeTheNextAppend)
{
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}));
	CommitJournaled(Store, journal, MakeEdit(1u));
	CommitJournaled(Store, journal, MakeEdit(2u));
	const FString afterTwo = Dump(Store.Live());
	CommitJournaled(Store, journal, MakeEdit(3u));
	journal.Close();

	// The process died in the middle of the third append.
	std::vector<uint8> bytes = ReadBytes(Journal);
	bytes.resize(bytes.size() - 5u);
	WriteBytes(Journal, bytes);
	{
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded).Files[0];
		EXPECT_TRUE(report.bJournalUsed);
		EXPECT_TRUE(report.bJournalTornTail);
		EXPECT_EQ(report.JournalCommits, 2u);
		EXPECT_EQ(Dump(loaded.Live()), afterTwo);
	}

	// Garbage after the whole commits is a torn tail too.
	const SJournalContents whole = ReadJournal(Journal);
	std::vector<uint8> garbage = ReadBytes(Journal);
	garbage.resize(static_cast<size_t>(whole.ValidBytes));
	garbage.insert(garbage.end(), { 0xFEu, 0x42u, 0x53u, 0xA7u, 0x01u });
	WriteBytes(Journal, garbage);
	const SJournalContents torn = ReadJournal(Journal);
	EXPECT_TRUE(torn.bTornTail);
	EXPECT_EQ(torn.Deltas.size(), 2u);
	EXPECT_EQ(torn.ValidBytes, whole.ValidBytes);

	// The next process continues the journal: the tail is cut off first, so its own commit is read back.
	CRegistryStore next;
	const SSnapshotLoadResult result = Load(next);
	CRegistryJournal continued;
	ASSERT_TRUE(continued.Open(Journal, ProjectId, result.Files[0].JournalBase, {}));
	EXPECT_FALSE(ReadJournal(Journal).bTornTail);
	CommitJournaled(next, continued, MakeEdit(4u));
	continued.Close();
	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded).Files[0];
	EXPECT_FALSE(report.bJournalTornTail);
	EXPECT_EQ(report.JournalCommits, 3u);
	ExpectSameContent(next.Live(), loaded.Live());
}

TEST_F(JournalTest, AJournalOfAnotherSnapshotIsIgnored)
{
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}));
	CommitJournaled(Store, journal, MakeEdit(1u));
	journal.Close();
	// A full write that was never followed by Restart (the process died in between).
	ASSERT_TRUE(WriteSnapshot(Store.Live(), {}, Snapshot, ProjectId));
	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded).Files[0];
	EXPECT_TRUE(report.bJournalFound);
	EXPECT_FALSE(report.bJournalUsed);
	EXPECT_FALSE(report.JournalMessage.IsEmpty());
	ExpectSameContent(Store.Live(), loaded.Live());

	// Another project's journal, or a mismatched base, starts anew when opened.
	CRegistryJournal other;
	ASSERT_TRUE(other.Open(Journal, SGuid::NewRandom(), Store.GetRevision(), {}));
	other.Close();
	EXPECT_TRUE(ReadJournal(Journal).Deltas.empty());
	CRegistryJournal fresh;
	ASSERT_TRUE(fresh.Open(Journal, ProjectId, std::nullopt, {}));
	EXPECT_EQ(fresh.GetJournalBase(), 0u);
}

TEST_F(JournalTest, RestartKeepsExactlyTheCommitsTheSnapshotDoesNotHold)
{
	// An earlier process appended two commits after the snapshot; this one loads them, commits more, and writes.
	{
		CRegistryJournal earlier;
		ASSERT_TRUE(earlier.Open(Journal, ProjectId, Store.GetRevision(), {}));
		CommitJournaled(Store, earlier, MakeEdit(1u));
		CommitJournaled(Store, earlier, MakeEdit(2u));
	}
	CRegistryStore store;
	const SSnapshotLoadResult result = Load(store);
	ASSERT_EQ(result.Files[0].JournalCommits, 2u);
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, result.Files[0].JournalBase, {}));
	CommitJournaled(store, journal, MakeEdit(3u));

	// The worker writes a view while the owner commits once more.
	const std::shared_ptr<const CRegistryView> view = store.Acquire();
	CommitJournaled(store, journal, MakeEdit(4u));
	ASSERT_TRUE(WriteSnapshot(*view, {}, Snapshot, ProjectId));
	FString error;
	ASSERT_TRUE(journal.Restart(view->GetRevision(), &error)) << error.CStr();
	EXPECT_EQ(journal.GetJournalBase(), view->GetRevision());
	const SJournalContents contents = ReadJournal(Journal);
	ASSERT_EQ(contents.Deltas.size(), 1u);   // only the commit after the view
	EXPECT_EQ(contents.Revisions[0], view->GetRevision() + 1u);
	CommitJournaled(store, journal, MakeEdit(5u));
	journal.Close();

	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded).Files[0];
	EXPECT_TRUE(report.bJournalUsed) << report.JournalMessage.CStr();
	EXPECT_EQ(report.JournalCommits, 2u);
	ExpectSameContent(store.Live(), loaded.Live());
}

TEST_F(JournalTest, ACommitWhoseDeltaDoesNotDecodeIsCutOffBeforeTheNextAppend)
{
	{
		CRegistryJournal journal;
		ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}));
		CommitJournaled(Store, journal, MakeEdit(1u));
	}
	// A commit whose frame passes its check but whose delta does not decode: the load stops before it.
	std::vector<uint8> bytes = ReadBytes(Journal);
	std::vector<uint8> payload;
	binary::AppendLittleEndian(payload, Store.GetRevision() + 1u, 8u);
	binary::AppendVarint(payload, 0u);
	binary::AppendVarint(payload, 1u);
	payload.insert(payload.end(), { uint8(0x05u), uint8(0xFFu), uint8(0xFFu), uint8(0xFFu) });
	record_frames::AppendFrame(bytes, 1u, payload);
	WriteBytes(Journal, bytes);
	const SJournalContents contents = ReadJournal(Journal);
	EXPECT_EQ(contents.Deltas.size(), 1u);
	EXPECT_TRUE(contents.bTornTail);
	EXPECT_FALSE(contents.Message.IsEmpty());

	// The next process continues the journal: that commit is cut off first, so its own commit is read back.
	CRegistryStore next;
	const SSnapshotLoadResult result = Load(next);
	ASSERT_EQ(result.Files[0].JournalCommits, 1u);
	CRegistryJournal continued;
	ASSERT_TRUE(continued.Open(Journal, ProjectId, result.Files[0].JournalBase, {}));
	CommitJournaled(next, continued, MakeEdit(2u));
	continued.Close();
	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded).Files[0];
	EXPECT_FALSE(report.bJournalTornTail) << report.JournalMessage.CStr();
	EXPECT_EQ(report.JournalCommits, 2u);
	ExpectSameContent(next.Live(), loaded.Live());
}

TEST_F(JournalTest, AFailedAppendBreaksTheJournalUntilAFullWriteAndLosesNoCommit)
{
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}));
	CommitJournaled(Store, journal, MakeEdit(1u));
	const FString afterOne = Dump(Store.Live());

	// The disk fills in the middle of the second append: part of its frame reaches the file. The commit goes ahead.
	SRegistryJournalTestAccess::FailNextAppendAfter(journal, 20u);
	FString error;
	SRegistryDelta second = MakeEdit(2u);
	EXPECT_FALSE(journal.Append(second, Store.GetRevision() + 1u, &error));
	EXPECT_FALSE(error.IsEmpty());
	Store.Commit(std::move(second));
	EXPECT_TRUE(journal.IsBroken());
	EXPECT_FALSE(journal.IsOpen());
	EXPECT_EQ(journal.GetStats().FailedAppends, 1u);

	// Nothing is written after the torn bytes: a later append fails too and the file is unchanged.
	const uintmax_t tornSize = std::filesystem::file_size(Journal);
	SRegistryDelta third = MakeEdit(3u);
	FString refused;
	EXPECT_FALSE(journal.Append(third, Store.GetRevision() + 1u, &refused));
	EXPECT_FALSE(refused.IsEmpty());
	Store.Commit(std::move(third));
	EXPECT_EQ(std::filesystem::file_size(Journal), tornSize);
	{
		// A crash now: the load replays the commit before the tear (the scan finds the rest).
		CRegistryStore loaded;
		const SSnapshotFileReport report = Load(loaded).Files[0];
		EXPECT_TRUE(report.bJournalUsed);
		EXPECT_TRUE(report.bJournalTornTail);
		EXPECT_EQ(report.JournalCommits, 1u);
		EXPECT_EQ(Dump(loaded.Live()), afterOne);
	}

	// The full write F3 schedules: a view on the worker while the owner commits once more (still refused).
	const std::shared_ptr<const CRegistryView> view = Store.Acquire();
	SRegistryDelta fourth = MakeEdit(4u);
	EXPECT_FALSE(journal.Append(fourth, Store.GetRevision() + 1u));
	Store.Commit(std::move(fourth));
	ASSERT_TRUE(WriteSnapshot(*view, {}, Snapshot, ProjectId));
	ASSERT_TRUE(journal.Restart(view->GetRevision(), &error)) << error.CStr();
	EXPECT_FALSE(journal.IsBroken());
	EXPECT_TRUE(journal.IsOpen());
	// The commit after the view never reached the file; Restart wrote it from memory.
	const SJournalContents contents = ReadJournal(Journal);
	ASSERT_EQ(contents.Deltas.size(), 1u);
	EXPECT_EQ(contents.Revisions[0], view->GetRevision() + 1u);
	EXPECT_FALSE(contents.bTornTail);
	CommitJournaled(Store, journal, MakeEdit(5u));
	journal.Close();

	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded).Files[0];
	EXPECT_TRUE(report.bJournalUsed) << report.JournalMessage.CStr();
	EXPECT_FALSE(report.bJournalTornTail);
	EXPECT_EQ(report.JournalCommits, 2u);
	ExpectSameContent(Store.Live(), loaded.Live());
}

TEST_F(JournalTest, ASnapshotThatLostItsRuleOutputsReplaysNoRuleHashes)
{
	// The journal holds rule-file hashes written after the snapshot, and an edit.
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}));
	SRegistryDelta rules;
	rules.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/T", 99u });
	rules.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "", 98u });
	CommitJournaled(Store, journal, std::move(rules));
	CommitJournaled(Store, journal, MakeEdit(1u));
	journal.Close();

	// LABELS is lost: the rule labels of records the journal never touched are gone, so no folder may look current.
	DamageSection(Snapshot, ESnapshotSection::Labels);
	CRegistryStore loaded;
	const SSnapshotFileReport report = Load(loaded).Files[0];
	EXPECT_TRUE(report.bRuleFilesDropped);
	EXPECT_TRUE(report.bJournalUsed) << report.JournalMessage.CStr();
	EXPECT_EQ(report.JournalCommits, 2u);
	EXPECT_FALSE(loaded.Live().FindRuleFile("Project", "/Project/T").has_value());
	EXPECT_FALSE(loaded.Live().FindRuleFile("Project", "").has_value());
	EXPECT_TRUE(FindRecord(loaded.Live(), "/Project/T/New1.png").IsValid());   // the rest of the journal is replayed

	// An intact snapshot keeps the journal's hashes.
	ASSERT_TRUE(WriteSnapshot(Store.Live(), {}, Snapshot, ProjectId));
	CRegistryJournal fresh;
	ASSERT_TRUE(fresh.Open(Journal, ProjectId, Store.GetRevision(), {}));
	SRegistryDelta again;
	again.RuleFileUpserts.push_back(SRuleFileUpsert { "Project", "/Project/T", 97u });
	CommitJournaled(Store, fresh, std::move(again));
	fresh.Close();
	CRegistryStore intact;
	ASSERT_TRUE(Load(intact).AnyLoaded());
	const std::optional<uint32> row = intact.Live().FindRuleFile("Project", "/Project/T");
	ASSERT_TRUE(row.has_value());
	SRuleFileData ruleFile;
	ASSERT_TRUE(intact.Live().GetRuleFile(*row, ruleFile));
	EXPECT_EQ(ruleFile.RulesHash, 97u);
}

TEST_F(JournalTest, ADeltaLargerThanAFrameIsSplitAndJoined)
{
	CRegistryJournal journal;
	ASSERT_TRUE(journal.Open(Journal, ProjectId, Store.GetRevision(), {}));
	SRegistryDelta big;
	FString message;
	message.Reserve(17u * 1024u * 1024u + 10u);
	for (uint32 i = 0u; i < 17u * 1024u; ++i)
	{
		message += FStringView("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef");
	}
	ASSERT_GT(message.Len(), 16u * 1024u * 1024u);
	big.Problems.push_back(SProblemsSet { "/Project/T/Rock.png", EProblemSource::Analyze, { MakeWarning("/Project/T/Rock.png", message) } });
	CommitJournaled(Store, journal, std::move(big));
	CommitJournaled(Store, journal, MakeEdit(1u));
	journal.Close();

	const SJournalContents contents = ReadJournal(Journal);
	ASSERT_EQ(contents.Deltas.size(), 2u);
	ASSERT_EQ(contents.Deltas[0].Problems.size(), 1u);
	EXPECT_EQ(contents.Deltas[0].Problems[0].Diagnostics[0].Message, message);

	// A tear inside the second part of the big commit drops it and everything after it.
	std::vector<uint8> bytes = ReadBytes(Journal);
	bytes.resize(JournalHeaderBytes + 16u * 1024u * 1024u + 100u);
	WriteBytes(Journal, bytes);
	const SJournalContents torn = ReadJournal(Journal);
	EXPECT_TRUE(torn.bTornTail);
	EXPECT_TRUE(torn.Deltas.empty());
	EXPECT_EQ(torn.ValidBytes, JournalHeaderBytes);
}
