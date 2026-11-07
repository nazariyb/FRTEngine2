// AssetTool's meta verbs as Core runs them (Assets/Meta/Validation.h, MetaVerbs.h, Migrations.h; design §13.2, §13.4,
// §14.8, M4's G4), in-process through a real registry with a lifecycle of its own and the recording provider, as AssetTool
// opens one: RunValidation lists the registry's problems and the rules only `validate` runs (META005, META007, a meta's
// unknown keys and settings, CRLF in rule files and text sources, VCS003 per pair, MIG001 per root), limited to staged
// paths, with no text read under --quick and with the rule files' Validation: severities; `fix` applies the safe repairs
// and never a relink; `new-meta` creates the derived meta and refuses a file that has one or takes none, writing nothing
// then; `migrate metas` is reproducible (two trees, the same bytes), skips existing metas, records itself in
// Schemas/Migrations.yml only once no file is left without one, and its dry run writes nothing; the Migrations.yml format
// is canonical, sorted and keeps steps it does not know; `resave --metas` rewrites only the metas that are not canonical.
// Every crash point of `migrate metas`, `fix` and `resave --metas` is rolled forward in-process, and `migrate metas` and
// `fix` each have one real killed child (edit [37]: the last two of M4's six). Trees hold at most eight files.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "../Rules/RulesTestUtils.h"
#include "Assets/Vcs/RecordingVcsProvider.h"
#include "AssetWriterTestUtils.h"
#include "TestCommon.h"
#include "TestProcess.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/MetaVerbs.h"
#include "Assets/Meta/Migrations.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/Meta/Validation.h"
#include "Assets/MountPaths.h"
#include "Assets/Registry/RegistryRecords.h"
#include "Hash/StableHash.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

using namespace frt;
using namespace frt::assets;
using frt::test::CRecordingVcsProvider;
using frt::test::rules::CRulesTree;


namespace
{
constexpr FStringLiteral Salt = "Перевірка";
constexpr FStringLiteral GuidA = "5d1e7a3b9c0f42d68e1a2b3c4d5e6f70";
constexpr FStringLiteral GuidB = "3f2a9c4e7b1d4e0f9a8b6c5d4e3f2a1b";

SGuid MakeGuid (FStringView Text)
{
	return SGuid::Parse(Text).value_or(SGuid {});
}

// A PNG's header as stb_image's info reads it; Size makes the bytes differ.
std::vector<uint8> MakePng (uint32 Size)
{
	std::vector<uint8> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	const auto u32 = [&] (uint32 Value) { png.insert(png.end(), { uint8(Value >> 24u), uint8(Value >> 16u), uint8(Value >> 8u), uint8(Value) }); };
	u32(13u);
	png.insert(png.end(), { 'I', 'H', 'D', 'R' });
	u32(Size);
	u32(Size);
	png.insert(png.end(), { 8u, 6u, 0u, 0u, 0u });
	u32(0u);
	for (const char* type : { "IDAT", "IEND" })
	{
		u32(0u);
		png.insert(png.end(), type, type + 4);
		u32(0u);
	}
	return png;
}

FString MetaText (FStringView Guid, FStringView Rest = {})
{
	return FString::Format("FormatVersion: 1\nGuid: {}\n{}", Guid, Rest);
}

FString P (FStringView Relative)
{
	return FString::Format("/Project/{}", Relative);
}

class CVerbTree : public CRulesTree
{
public:
	explicit CVerbTree (FStringView Name)
		: CRulesTree(Name)
	{}

	[[nodiscard]] std::filesystem::path Abs (FStringView Relative) const { return Tree.ProjectContent() / Relative.ToPath(); }
	void WritePng (FStringView Relative, uint32 Size = 4u) const
	{
		frt::test::rules::MakeDirectories(Abs(Relative).parent_path());
		ASSERT_TRUE(platform::WriteFileBytes(Abs(Relative), MakePng(Size)));
	}
	void WriteMeta (FStringView Relative, FStringView Text) const { WriteProjectContent(GetMetaPath(Relative), Text); }
	[[nodiscard]] bool Exists (FStringView Relative) const
	{
		std::error_code ec;
		return std::filesystem::is_regular_file(Abs(Relative), ec);
	}
	[[nodiscard]] FString Read (FStringView Relative) const { return ReadAt(Abs(Relative)); }
	[[nodiscard]] static FString ReadAt (const std::filesystem::path& Path)
	{
		const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytesUncached(Path);
		return bytes.has_value() ? FString(FStringView(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size()))) : FString();
	}
	[[nodiscard]] SGuid MetaGuid (FStringView Relative) const
	{
		SMetaParseResult parsed;
		ParseMeta(Read(GetMetaPath(Relative)), parsed);
		return parsed.State == EMetaParseState::Ok ? parsed.Meta.Guid : SGuid {};
	}
	[[nodiscard]] std::shared_ptr<const CMountTable> Mounts () const { return std::make_shared<const CMountTable>(Resolve()); }
	[[nodiscard]] FString Vcs (FStringView Relative) const { return FString::FromPath(Abs(Relative).make_preferred()); }
	[[nodiscard]] FString Migrations () const { return ReadAt(GetMigrationsPath(Tree.ProjectDir())); }
};

jobs::SWorkerPoolDesc MakePoolDesc ()
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = 2u;
	desc.Name = "Meta Verbs Test";
	return desc;
}

/**
 * The registry a meta verb opens in AssetTool (RegistryCommandlets.cpp, OpenRegistry): a lifecycle of its own with the
 * recording provider on /Project and the project salt, no live ticks, a writer that repairs unless bReport.
 */
class CVerbRig
{
public:
	CVerbRig (const CVerbTree& InTree, bool bReport = false)
		: Tree(InTree)
		, Pool(MakePoolDesc())
	{
		Lifecycle.Providers = { { ProjectMountId, &Vcs } };
		Lifecycle.ProjectSalt = FString(Salt);
		Lifecycle.bRepairs = !bReport;
		Desc.Mounts = Tree.Mounts();
		Desc.Pool = &Pool;
		Desc.bWriter = !bReport;
		Desc.bLoadSnapshots = false;
		Desc.bSnapshotAtShutDown = false;
		Desc.Lifecycle = &Lifecycle;
		Registry = std::make_unique<CAssetRegistry>(Desc);
		Registry->Start();
		bOpened = Registry->WaitForInitialScan(60.0);
	}

	~CVerbRig ()
	{
		Registry->ShutDown();
	}

	[[nodiscard]] CAssetRepairs& Repairs () { return *Registry->GetRepairs(); }
	[[nodiscard]] const CRegistryView& View () const { return Registry->Live(); }

	// validate: the lifecycle over the project first (a report-only run, or the safe repairs), then RunValidation.
	[[nodiscard]] std::vector<SDiagnostic> Validate (std::optional<std::vector<FString>> Paths = std::nullopt, bool bQuick = false, bool bFix = false)
	{
		SRepairOptions repair;
		repair.Paths = Paths.value_or(std::vector<FString> { FString("/Project") });
		if (bFix)
		{
			repair.Kinds.assign(std::begin(SafeFixKinds), std::end(SafeFixKinds));
		}
		repair.bNoHashing = bQuick;
		(void)Repairs().RunNow(repair);
		SValidateOptions options;
		options.Mounts = Desc.Mounts.get();
		options.Rules = &Registry->GetRules();
		options.Scope = { ProjectMountId };
		options.Paths = std::move(Paths);
		options.bQuick = bQuick;
		options.Providers = Lifecycle.Providers;
		std::vector<SDiagnostic> problems;
		RunValidation(View(), options, problems);
		return problems;
	}

	[[nodiscard]] SMigrateMetasResult Migrate (bool bDryRun = false)
	{
		const MountId mounts[] = { ProjectMountId };
		return MigrateMetas(*Registry, mounts, Tree.Tree.ProjectDir(), &Vcs, bDryRun);
	}

	const CVerbTree& Tree;
	jobs::CWorkerPool Pool;
	CRecordingVcsProvider Vcs;
	SLifecycleDesc Lifecycle;
	SAssetRegistryDesc Desc;
	std::unique_ptr<CAssetRegistry> Registry;
	bool bOpened = false;
};

const SDiagnostic* Find (std::span<const SDiagnostic> Problems, uint32 Code, FStringView Subject, FStringView FieldPath = {})
{
	const auto found = std::find_if(Problems.begin(), Problems.end(), [&] (const SDiagnostic& Problem)
	{
		return Problem.Code == Code && Problem.Subject == Subject && (FieldPath.IsEmpty() || Problem.FieldPath == FieldPath);
	});
	return found != Problems.end() ? &*found : nullptr;
}

FString Describe (std::span<const SDiagnostic> Problems)
{
	return test::rules::DescribeDiagnostics(Problems);
}

SGuid DerivedGuid (FStringView MountPath, uint32 PngSize = 4u)
{
	return MakeCreateMetaGuid(Salt, MountPath, hash::HashContent(MakePng(PngSize)));
}

constexpr FStringLiteral MigrationsWithMetas = "FormatVersion: 1\nApplied:\n  - metas\n";
}


// ---------------- validate ----------------

TEST(MetaVerbs, ValidateListsTheRegistrysProblemsAndTheRulesOnlyItRuns)
{
	CVerbTree tree("Перевірити");
	tree.WriteProjectRules("FormatVersion: 1\nScan: { Exclude: [ \"*.tmp\" ] }\n");
	tree.WritePng("Rock.png");
	tree.WriteMeta("Rock.png", FString::Format("Guid: {}\nFormatVersion: 1\nColour: red\nSettings:\n  ColorSpace: Purple\n  Bogus: 3\n", GuidA));
	tree.WritePng("New.png", 5u);                                       // no meta: META001 in a report-only run
	tree.WriteProjectContent("Notes.txt", "notes");                     // unclaimed
	tree.WriteMeta("Notes.txt", MetaText(GuidB));
	tree.WriteProjectContent("Old.tmp", "temporary");                    // excluded
	tree.WriteMeta("Old.tmp", MetaText("6a1e7a3b9c0f42d68e1a2b3c4d5e6f71"));
	tree.WriteProjectContent("Stone.frtmat.yml", "version: 2\r\nname: Stone\r\n");
	tree.WriteProjectContent("Stone.frtmat.yml.meta", MetaText("7a1e7a3b9c0f42d68e1a2b3c4d5e6f72"));
	tree.WriteProjectContent("Sub/Assets.yml", "FormatVersion: 1\r\n");
	tree.WritePng("Sub/Moss.png", 6u);
	tree.WriteMeta("Sub/Moss.png", MetaText("8a1e7a3b9c0f42d68e1a2b3c4d5e6f73"));
	CVerbRig rig(tree, true);
	ASSERT_TRUE(rig.bOpened);
	const std::vector<SDiagnostic> problems = rig.Validate();

	const SDiagnostic* canonical = Find(problems, Meta005, P("Rock.png.meta"));
	ASSERT_NE(canonical, nullptr) << Describe(problems).CStr();
	EXPECT_EQ(canonical->Severity, ESeverity::Warning);
	EXPECT_EQ(canonical->File, "Content/Rock.png.meta");
	EXPECT_NE(Find(problems, 0u, P("Rock.png.meta"), "Colour"), nullptr) << "an unknown key: " << Describe(problems).CStr();
	EXPECT_NE(Find(problems, 0u, P("Rock.png.meta"), "Settings.Bogus"), nullptr) << Describe(problems).CStr();
	const SDiagnostic* value = Find(problems, 0u, P("Rock.png.meta"), "Settings.ColorSpace");
	ASSERT_NE(value, nullptr) << Describe(problems).CStr();
	EXPECT_EQ(value->Severity, ESeverity::Warning);
	EXPECT_TRUE(value->Message.Contains("Purple")) << value->Message.CStr();
	EXPECT_NE(Find(problems, Meta007, P("Notes.txt.meta")), nullptr) << "beside an unclaimed file: " << Describe(problems).CStr();
	EXPECT_NE(Find(problems, Meta007, P("Old.tmp.meta")), nullptr) << "beside an excluded file: " << Describe(problems).CStr();
	EXPECT_EQ(Find(problems, Meta002, P("Old.tmp.meta")), nullptr) << "a meta beside an excluded file is no orphan";
	EXPECT_NE(Find(problems, Vcs001, P("Stone.frtmat.yml")), nullptr) << "a CRLF text source: " << Describe(problems).CStr();
	EXPECT_NE(Find(problems, Vcs001, P("Sub/Assets.yml")), nullptr) << "a CRLF rule file: " << Describe(problems).CStr();
	const SDiagnostic* missing = Find(problems, Meta001, P("New.png"));
	ASSERT_NE(missing, nullptr) << "the lifecycle's, as the report-only run committed it: " << Describe(problems).CStr();
	EXPECT_EQ(missing->Severity, ESeverity::Error);
	const SDiagnostic* behind = Find(problems, Mig001, "/Project");
	ASSERT_NE(behind, nullptr) << Describe(problems).CStr();
	EXPECT_EQ(behind->File, "Schemas/Migrations.yml");
	EXPECT_TRUE(std::any_of(problems.begin(), problems.end(), [] (const SDiagnostic& Problem) { return Problem.Code == Rule002; }))
		<< "and what the registry gathered: " << Describe(problems).CStr();
	EXPECT_EQ(std::count_if(problems.begin(), problems.end(), [] (const SDiagnostic& Problem) { return Problem.Code == Meta005; }), 1)
		<< "deduplicated, and only the meta that is not canonical";
	for (size_t i = 1u; i < problems.size(); ++i)
	{
		EXPECT_FALSE(LessPortableThenBytes(problems[i].Subject, problems[i - 1u].Subject)) << "sorted by subject: " << Describe(problems).CStr();
	}
	EXPECT_FALSE(IsValidationClean(CountValidation(problems), false));
	EXPECT_TRUE(rig.Vcs.GetWriteCalls().empty()) << "a report-only run writes nothing";
}

TEST(MetaVerbs, ValidateLimitedToStagedPathsQuickAndWithValidationSeveritiesKeepsOnlyWhatItMust)
{
	CVerbTree tree("Обмежено");
	tree.WriteProjectRules("FormatVersion: 1\nValidation: { META005: Info }\n");
	tree.WritePng("A/Rock.png");
	tree.WriteMeta("A/Rock.png", FString::Format("Guid: {}\nFormatVersion: 1\n", GuidA));
	tree.WritePng("B/Moss.png", 5u);
	tree.WriteMeta("B/Moss.png", FString::Format("Guid: {}\nFormatVersion: 1\n", GuidB));
	tree.WriteProjectContent("B/Stone.frtmat.yml", "version: 2\r\nname: Stone\r\n");
	tree.WriteProjectContent("B/Stone.frtmat.yml.meta", MetaText("7a1e7a3b9c0f42d68e1a2b3c4d5e6f72"));
	tree.WritePng("C/Bad.png", 6u);
	tree.WriteMeta("C/Bad.png", MetaText("8a1e7a3b9c0f42d68e1a2b3c4d5e6f73", "Labels: [ unclosed\n"));
	ASSERT_TRUE(platform::WriteFileAtomic(GetMigrationsPath(tree.Tree.ProjectDir()), FStringView(MigrationsWithMetas)));
	CVerbRig rig(tree, true);
	ASSERT_TRUE(rig.bOpened);

	const std::vector<SDiagnostic> all = rig.Validate();
	const SDiagnostic* info = Find(all, Meta005, P("A/Rock.png.meta"));
	ASSERT_NE(info, nullptr) << Describe(all).CStr();
	EXPECT_EQ(info->Severity, ESeverity::Info) << "the rule file's Validation: applies to a V-only rule";
	EXPECT_EQ(Find(all, Mig001, "/Project"), nullptr) << "Migrations.yml lists metas: " << Describe(all).CStr();
	EXPECT_FALSE(IsValidationClean(CountValidation(all), false)) << "the CRLF source is an error: " << Describe(all).CStr();

	// Staged: only the meta staged and its source; the root's MIG001 would come whatever was staged.
	const std::vector<SDiagnostic> staged = rig.Validate(std::vector<FString> { P("A/Rock.png.meta") });
	EXPECT_NE(Find(staged, Meta005, P("A/Rock.png.meta")), nullptr) << Describe(staged).CStr();
	EXPECT_EQ(Find(staged, Meta005, P("B/Moss.png.meta")), nullptr) << Describe(staged).CStr();
	EXPECT_EQ(Find(staged, Vcs001, P("B/Stone.frtmat.yml")), nullptr) << Describe(staged).CStr();
	// A staged meta that does not parse is the lifecycle's META004, which the run over that meta alone still finds.
	const std::vector<SDiagnostic> broken = rig.Validate(std::vector<FString> { P("C/Bad.png.meta") }, true);
	EXPECT_TRUE(std::any_of(broken.begin(), broken.end(), [] (const SDiagnostic& Problem) { return Problem.Code == Meta004; })) << Describe(broken).CStr();
	EXPECT_EQ(Find(broken, Meta005, P("A/Rock.png.meta")), nullptr) << Describe(broken).CStr();

	// Quick reads no text source unless it was asked for by path.
	EXPECT_EQ(Find(rig.Validate(std::nullopt, true), Vcs001, P("B/Stone.frtmat.yml")), nullptr);
	EXPECT_NE(Find(rig.Validate(std::vector<FString> { P("B") }, true), Vcs001, P("B/Stone.frtmat.yml")), nullptr);
}

TEST(MetaVerbs, Vcs003IsAPairTheVcsTracksDifferentlyAskedInOneStatusCall)
{
	CVerbTree tree("Відстежено");
	ASSERT_TRUE(platform::WriteFileAtomic(GetMigrationsPath(tree.Tree.ProjectDir()), FStringView(MigrationsWithMetas)));
	tree.WritePng("Tracked.png");
	tree.WriteMeta("Tracked.png", MetaText(GuidA));
	tree.WritePng("Local.png", 5u);
	tree.WriteMeta("Local.png", MetaText(GuidB));
	tree.WritePng("Both.png", 6u);
	tree.WriteMeta("Both.png", MetaText("7a1e7a3b9c0f42d68e1a2b3c4d5e6f72"));
	CVerbRig rig(tree, true);
	ASSERT_TRUE(rig.bOpened);
	rig.Vcs.SetStatus(tree.Vcs("Tracked.png"), SVcsStatus { EVcsState::Clean, {} });
	rig.Vcs.SetStatus(tree.Vcs("Tracked.png.meta"), SVcsStatus { EVcsState::Clean, {} });
	rig.Vcs.SetStatus(tree.Vcs("Local.png"), SVcsStatus { EVcsState::Clean, {} });
	rig.Vcs.SetStatus(tree.Vcs("Both.png.meta"), SVcsStatus { EVcsState::Added, {} });

	const std::vector<SDiagnostic> problems = rig.Validate();
	const SDiagnostic* local = Find(problems, Vcs003, P("Local.png"));
	ASSERT_NE(local, nullptr) << Describe(problems).CStr();
	EXPECT_EQ(local->Severity, ESeverity::Warning);
	EXPECT_TRUE(local->Message.Contains("commit it")) << local->Message.CStr();
	EXPECT_NE(Find(problems, Vcs003, P("Both.png")), nullptr) << "a meta added without its source: " << Describe(problems).CStr();
	EXPECT_EQ(Find(problems, Vcs003, P("Tracked.png")), nullptr) << Describe(problems).CStr();
	EXPECT_FALSE(IsValidationClean(CountValidation(problems), true)) << "a warning fails --ci";
	EXPECT_TRUE(IsValidationClean(CountValidation(problems), false));
	const std::vector<CRecordingVcsProvider::SCall> calls = rig.Vcs.GetCalls();
	EXPECT_EQ(std::count_if(calls.begin(), calls.end(), [] (const CRecordingVcsProvider::SCall& Call) { return Call.Name == "StatusOf"; }), 1)
		<< "every pair in one call";
}

// ---------------- Schemas/Migrations.yml ----------------

TEST(MetaVerbs, MigrationsYmlIsCanonicalSortedMergeableAndKeepsStepsItDoesNotKnow)
{
	FString text;
	const FString applied[] = { "metas", "documents.material", "metas" };
	ASSERT_TRUE(EmitMigrations(applied, text));
	EXPECT_EQ(text, "FormatVersion: 1\nApplied:\n  - documents.material\n  - metas\n");

	CVerbTree tree("Міграції");
	const std::filesystem::path root = tree.Tree.ProjectDir();
	SMigrationsFile file;
	ASSERT_TRUE(ReadMigrations(root, file));
	EXPECT_FALSE(file.bExists);
	const std::vector<FStringLiteral> missing = GetMissingMigrations(file);
	ASSERT_EQ(missing.size(), 1u);
	EXPECT_EQ(FStringView(missing[0]), "metas");

	ASSERT_TRUE(platform::WriteFileAtomic(GetMigrationsPath(root), FStringView("FormatVersion: 1\nApplied: [ texture-colorspaces ]\n")));
	FString error;
	ASSERT_TRUE(RecordMigration(root, "metas", nullptr, &error)) << error.CStr();
	EXPECT_EQ(tree.Migrations(), "FormatVersion: 1\nApplied:\n  - metas\n  - texture-colorspaces\n") << "a newer engine's step is kept";
	ASSERT_TRUE(ReadMigrations(root, file));
	EXPECT_TRUE(GetMissingMigrations(file).empty());

	ASSERT_TRUE(platform::WriteFileAtomic(GetMigrationsPath(root), FStringView("FormatVersion: 9\n")));
	SDiagnostic broken;
	EXPECT_FALSE(ReadMigrations(root, file, &broken));
	EXPECT_EQ(broken.Code, Mig001);
	EXPECT_EQ(broken.File, "Schemas/Migrations.yml");
	EXPECT_EQ(FindMigrationStep("METAS"), &GetMigrationSteps()[0]);
	EXPECT_FALSE(FindMigrationStep("material-refs")->bAvailable);
}

// ---------------- migrate metas ----------------

TEST(MetaVerbs, MigrateMetasIsReproducibleSkipsExistingMetasAndRecordsItselfOnceComplete)
{
	const auto fill = [] (const CVerbTree& Tree)
	{
		Tree.WritePng("Textures/Камінь.png", 4u);
		Tree.WritePng("Textures/Мох.png", 5u);
		Tree.WritePng("Kept.png", 6u);
		Tree.WriteMeta("Kept.png", MetaText(GuidA));
		Tree.WriteProjectContent("Readme.txt", "an unclaimed file takes no meta");
	};
	CVerbTree first("Перша копія");
	CVerbTree second("Друга копія");
	fill(first);
	fill(second);
	const FString kept = first.Read("Kept.png.meta");
	{
		CVerbRig rig(first);
		ASSERT_TRUE(rig.bOpened);
		const SMigrateMetasResult dryRun = rig.Migrate(true);
		EXPECT_EQ(dryRun.Report.Planned.size(), 2u);
		EXPECT_FALSE(first.Exists("Textures/Камінь.png.meta")) << "a dry run writes nothing";
		EXPECT_FALSE(std::filesystem::exists(GetMigrationsPath(first.Tree.ProjectDir())));

		const SMigrateMetasResult result = rig.Migrate();
		EXPECT_TRUE(result.Remaining.empty());
		EXPECT_TRUE(result.bRecorded) << result.Error.CStr();
		EXPECT_FALSE(result.bWasRecorded);
		EXPECT_EQ(first.MetaGuid("Textures/Камінь.png"), DerivedGuid(P("Textures/Камінь.png"), 4u));
		EXPECT_EQ(first.MetaGuid("Textures/Мох.png"), DerivedGuid(P("Textures/Мох.png"), 5u));
		for (const SLifecycleAction& planned : dryRun.Report.Planned)
		{
			EXPECT_EQ(planned.Guid, first.MetaGuid(planned.Path.View().RightChop(9u))) << "the dry run said what it writes";
		}
		EXPECT_EQ(first.Read("Kept.png.meta"), kept) << "an existing meta is never reminted";
		EXPECT_FALSE(first.Exists("Readme.txt.meta"));
		EXPECT_EQ(first.Migrations(), FString(MigrationsWithMetas));

		const SMigrateMetasResult again = rig.Migrate();
		EXPECT_TRUE(again.bWasRecorded);
		EXPECT_TRUE(again.Report.Planned.empty()) << "nothing left to create";
	}
	{
		CVerbRig rig(second);
		ASSERT_TRUE(rig.bOpened);
		ASSERT_TRUE(rig.Migrate().bRecorded);
	}
	for (const FStringView file : { FStringView("Textures/Камінь.png.meta"), FStringView("Textures/Мох.png.meta") })
	{
		EXPECT_EQ(first.Read(file), second.Read(file)) << "two machines that migrate one commit write the same bytes";
	}
}

TEST(MetaVerbs, MigrateMetasLeavesItselfUnrecordedWhileAFileStillHasNoMeta)
{
	CVerbTree tree("Незавершено");
	tree.WritePng("Smudged.png", 4u);
	// An un-smudged Git LFS checkout: never hashed into a GUID (VCS004), so it stays without a meta until `git lfs pull`.
	tree.WriteProjectContent("Pointer.png", "version https://git-lfs.github.com/spec/v1\noid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\nsize 12345\n");
	CVerbRig rig(tree);
	ASSERT_TRUE(rig.bOpened);
	const SMigrateMetasResult result = rig.Migrate();
	EXPECT_EQ(result.Remaining, std::vector<FString> { P("Pointer.png") });
	EXPECT_FALSE(result.bRecorded);
	EXPECT_TRUE(tree.Exists("Smudged.png.meta"));
	EXPECT_FALSE(tree.Exists("Pointer.png.meta"));
	EXPECT_FALSE(std::filesystem::exists(GetMigrationsPath(tree.Tree.ProjectDir())));
	EXPECT_NE(Find(result.Report.Problems, Vcs004, P("Pointer.png")), nullptr) << Describe(result.Report.Problems).CStr();
}

// ---------------- new-meta, fix ----------------

TEST(MetaVerbs, NewMetaCreatesTheDerivedMetaAndRefusesAFileWithOneOrNoneWritingNothingThen)
{
	CVerbTree tree("Нова мета");
	tree.WriteProjectRules("FormatVersion: 1\nSafety:\n  AutoRepairMetas: false\n");
	tree.WritePng("Rock.png", 4u);
	tree.WritePng("Moss.png", 5u);
	tree.WriteMeta("Moss.png", MetaText(GuidA));
	tree.WriteProjectContent("Readme.txt", "unclaimed");
	CVerbRig rig(tree);
	ASSERT_TRUE(rig.bOpened);

	const FString refused[] = { P("Rock.png"), P("Moss.png"), P("Readme.txt"), P("Gone.png") };
	const SNewMetasResult none = NewMetas(*rig.Registry, refused);
	EXPECT_TRUE(none.bRefused);
	ASSERT_EQ(none.Problems.size(), 3u) << Describe(none.Problems).CStr();
	EXPECT_TRUE(none.Problems[0].Message.Contains("has a meta already")) << none.Problems[0].Message.CStr();
	EXPECT_EQ(none.Problems[0].File, "Content/Moss.png");
	EXPECT_FALSE(tree.Exists("Rock.png.meta")) << "one refusal writes nothing";

	const FString named[] = { P("Rock.png") };
	const SNewMetasResult created = NewMetas(*rig.Registry, named);
	EXPECT_FALSE(created.bRefused);
	EXPECT_TRUE(created.Problems.empty()) << Describe(created.Problems).CStr();
	EXPECT_EQ(created.Created, std::vector<FString> { P("Rock.png") });
	EXPECT_EQ(tree.MetaGuid("Rock.png"), DerivedGuid(P("Rock.png"), 4u)) << "Safety.AutoRepairMetas gates no command";
}

TEST(MetaVerbs, FixAppliesTheSafeRepairsAndNeverARelink)
{
	CVerbTree tree("Виправити");
	tree.WritePng("New.png", 4u);
	tree.WritePng("Moved/Stone.png", 5u);
	tree.WriteMeta("Moved/Stone.png", MetaText(GuidA));
	CVerbRig rig(tree);
	ASSERT_TRUE(rig.bOpened);
	(void)rig.Validate(std::nullopt, false, true);
	ASSERT_TRUE(tree.Exists("Moved/Stone.png.meta"));
	EXPECT_EQ(tree.MetaGuid("New.png"), DerivedGuid(P("New.png"), 4u));

	// An Explorer move: the orphan meta matches the moved file by FileId, a relink that fix leaves to a person.
	std::error_code ec;
	std::filesystem::create_directories(tree.Abs("Elsewhere"), ec);
	std::filesystem::rename(tree.Abs("Moved/Stone.png"), tree.Abs("Elsewhere/Stone.png"), ec);
	ASSERT_FALSE(ec);
	const FString scan[] = { P("Moved"), P("Elsewhere") };
	ASSERT_TRUE(rig.Registry->ScanPathsSynchronous(scan, 30.0));
	const std::vector<SDiagnostic> remaining = rig.Validate(std::nullopt, false, true);
	EXPECT_FALSE(tree.Exists("Elsewhere/Stone.png.meta")) << "no relink and no new identity: " << Describe(remaining).CStr();
	EXPECT_TRUE(tree.Exists("Moved/Stone.png.meta"));
	EXPECT_TRUE(std::any_of(remaining.begin(), remaining.end(), [] (const SDiagnostic& Problem) { return Problem.Message.Contains("MoveMeta"); }))
		<< "reported as a repair this run does not make: " << Describe(remaining).CStr();
}

// ---------------- resave --metas ----------------

TEST(MetaVerbs, ResaveMetasRewritesOnlyMetasThatAreNotCanonicalAndEveryCrashPointRollsForward)
{
	CVerbTree tree("Перезапис");
	const FString canonical = MetaText(GuidA);
	tree.WritePng("Canonical.png", 4u);
	tree.WriteMeta("Canonical.png", canonical);
	tree.WritePng("Messy.png", 5u);
	tree.WritePng("Crlf.png", 6u);
	tree.WritePng("Broken.png", 7u);
	tree.WriteMeta("Broken.png", "FormatVersion: 1\nGuid: 6a1e7a3b9c0f42d68e1a2b3c4d5e6f71\nLabels: [ unclosed\n");
	const FString messy = FString::Format("Labels: [ b, a ]\nGuid: {}\nFormatVersion: 1\n", GuidB);
	const FString crlf = "FormatVersion: 1\r\nGuid: 7a1e7a3b9c0f42d68e1a2b3c4d5e6f72\r\n";
	tree.WriteMeta("Messy.png", messy);
	tree.WriteMeta("Crlf.png", crlf);
	const FString broken = tree.Read("Broken.png.meta");
	CVerbRig rig(tree);
	ASSERT_TRUE(rig.bOpened);
	const MountId mounts[] = { ProjectMountId };
	CAssetWriter& writer = *rig.Repairs().GetWriterOf(ProjectMountId);

	for (uint32 stop = 0u; stop <= 2u; ++stop)
	{
		tree.WriteMeta("Messy.png", messy);
		tree.WriteMeta("Crlf.png", crlf);
		SResaveMetasResult planned;
		const std::vector<SAssetOp> ops = PlanResaveMetas(writer, rig.View(), mounts, planned);
		ASSERT_EQ(ops.size(), 1u);
		ASSERT_EQ(ops[0].Steps.size(), 2u);
		EXPECT_EQ(planned.Rewritten, (std::vector<FString> { P("Crlf.png.meta"), P("Messy.png.meta") }));
		EXPECT_EQ(planned.Broken, std::vector<FString> { P("Broken.png.meta") });
		EXPECT_EQ(planned.Canonical, 1u);
		writer.SetStopAfterStepForTest(stop);
		(void)writer.Run(ops[0]);
		writer.SetStopAfterStepForTest(~0u);
		ASSERT_TRUE(writer.GetUnfinished().has_value()) << stop;
		FString error;
		ASSERT_TRUE(writer.RollForward(&error)) << error.CStr();
		EXPECT_EQ(tree.Read("Messy.png.meta"), FString::Format("FormatVersion: 1\nGuid: {}\nLabels:\n  - a\n  - b\n", GuidB)) << stop;
		EXPECT_EQ(tree.Read("Crlf.png.meta"), "FormatVersion: 1\nGuid: 7a1e7a3b9c0f42d68e1a2b3c4d5e6f72\n") << stop;
	}

	tree.WriteMeta("Messy.png", messy);
	const SResaveMetasResult result = ResaveMetas(*rig.Registry, mounts, false);
	EXPECT_TRUE(result.Problems.empty()) << Describe(result.Problems).CStr();
	EXPECT_EQ(result.Rewritten, std::vector<FString> { P("Messy.png.meta") });
	EXPECT_EQ(tree.Read("Canonical.png.meta"), canonical);
	EXPECT_EQ(tree.Read("Broken.png.meta"), broken) << "a broken meta is never rewritten";
	for (const CRecordingVcsProvider::SCall& call : rig.Vcs.GetWriteCalls())
	{
		EXPECT_EQ(std::count(call.Files.begin(), call.Files.end(), tree.Vcs("Canonical.png.meta")), 0) << "a canonical meta is not checked out";
	}
}


// ---------------- The crash points of migrate metas and fix ----------------

namespace
{
// "<engine dir> <engine content> <engine config> <project dir> <migrate|fix> <stop after>": opens the registry a meta verb
// opens, arms its writer to stop after that many steps of its first op, runs the verb, and blocks there to be killed.
int RunMetaVerbKill (std::span<const FString> Arguments)
{
	if (Arguments.size() < 6u)
	{
		return 2;
	}
	jobs::CWorkerPool pool(MakePoolDesc());
	CRecordingVcsProvider vcs;
	SLifecycleDesc lifecycle;
	lifecycle.Providers = { { ProjectMountId, &vcs } };
	lifecycle.ProjectSalt = FString(Salt);
	lifecycle.bRepairs = true;
	SAssetRegistryDesc desc;
	desc.Mounts = std::make_shared<const CMountTable>(test::meta::ResolveMounts(Arguments[0].ToPath(), Arguments[1].ToPath(),
		Arguments[2].ToPath(), Arguments[3].ToPath()));
	desc.Pool = &pool;
	desc.bWriter = true;
	desc.bLoadSnapshots = false;
	desc.bSnapshotAtShutDown = false;
	desc.Lifecycle = &lifecycle;
	CAssetRegistry registry(desc);
	registry.Start();
	if (!registry.WaitForInitialScan(60.0))
	{
		return 3;
	}
	registry.GetRepairs()->GetWriter()->SetStopAfterStepForTest(Arguments[5].ToUInt32().value_or(0u), []
	{
		std::fputs("stopped\n", stdout);
		std::fflush(stdout);
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	});
	if (Arguments[4] == "migrate")
	{
		const MountId mounts[] = { ProjectMountId };
		(void)MigrateMetas(registry, mounts, Arguments[3].ToPath(), &vcs, false);
	}
	else
	{
		SRepairOptions options;
		options.Kinds.assign(std::begin(SafeFixKinds), std::end(SafeFixKinds));
		(void)registry.GetRepairs()->RunNow(options);
	}
	std::fputs("returned\n", stdout);
	return 4;
}

const bool bMetaVerbKillRegistered = test::RegisterChildMode("meta-verb-kill", &RunMetaVerbKill);

// Runs the verb in a child that stops after StopAfter steps of its first op, kills it there, and returns the op it left.
std::optional<SOpJournalEntry> KillMidVerb (const CVerbTree& Tree, FStringView Verb, uint32 StopAfter)
{
	const std::vector<FString> arguments = { FString(test::ChildModeSwitch) + "meta-verb-kill", FString::FromPath(Tree.Tree.EngineDir()),
		FString::FromPath(Tree.Tree.EngineContent()), FString::FromPath(Tree.Tree.EngineConfig()), FString::FromPath(Tree.Tree.ProjectDir()),
		FString(Verb), FString::Format("{}", StopAfter) };
	test::CTestProcess child;
	FString error;
	EXPECT_TRUE(child.Start(platform::GetExecutablePath(), arguments, false, {}, &error)) << error.CStr();
	EXPECT_TRUE(test::WaitUntil([&child] { return child.ReadOutput().Contains("stopped") || !child.IsRunning(); })) << child.ReadErrors().CStr();
	EXPECT_TRUE(child.ReadOutput().Contains("stopped")) << child.ReadOutput().CStr() << child.ReadErrors().CStr();
	child.Kill();
	child.Wait();
	SOpJournalContents contents;
	if (!ReadOpJournal(GetOpJournalPath(Tree.Tree.ProjectDir()), contents) || !contents.Unfinished.has_value())
	{
		return std::nullopt;
	}
	return contents.Unfinished->Op.Verb.IsEmpty() ? std::nullopt : std::optional<SOpJournalEntry>(std::move(*contents.Unfinished));
}

void WritePair (const CVerbTree& Tree, FStringView Folder)
{
	Tree.WritePng(FString::Format("{}/Rock.png", Folder), 4u);
	Tree.WriteMeta(FString::Format("{}/Rock.png", Folder), MetaText(GuidA));
	Tree.WritePng(FString::Format("{}/Skin.png", Folder), 5u);
	Tree.WriteMeta(FString::Format("{}/Skin.png", Folder), MetaText(GuidB));
}

void ExpectCopyDeduped (const CVerbTree& Tree, FStringView Context)
{
	EXPECT_EQ(Tree.MetaGuid("Pack/Rock.png"), MakeGuid(GuidA)) << FString(Context).CStr();
	EXPECT_EQ(Tree.MetaGuid("Pack2/Rock.png"), MakeDedupeGuid(Salt, MakeGuid(GuidA), P("Pack2/Rock.png"))) << FString(Context).CStr();
	EXPECT_EQ(Tree.MetaGuid("Pack2/Skin.png"), MakeDedupeGuid(Salt, MakeGuid(GuidB), P("Pack2/Skin.png"))) << FString(Context).CStr();
}
}

TEST(MetaVerbs, EveryCrashPointOfMigrateMetasRollsForwardToTheSameBytesAndOneRealKillDoesToo)
{
	CVerbTree reference("Еталон");
	reference.WritePng("A.png", 4u);
	reference.WritePng("B.png", 5u);
	{
		CVerbRig rig(reference);
		ASSERT_TRUE(rig.bOpened);
		ASSERT_TRUE(rig.Migrate().bRecorded);
	}

	// In-process: the first op stops after k of its steps (its meta not yet written, or written without Add and End); the
	// run goes on, the next op's Run finishing it first, and a second run records the step.
	for (uint32 stop = 0u; stop <= 1u; ++stop)
	{
		CVerbTree tree(FString::Format("Збій міграції {}", stop));
		tree.WritePng("A.png", 4u);
		tree.WritePng("B.png", 5u);
		{
			CVerbRig rig(tree);
			ASSERT_TRUE(rig.bOpened);
			std::vector<SGuid> planned;
			CAssetWriter& writer = *rig.Repairs().GetWriter();
			writer.SetStopAfterStepForTest(stop, [&writer, &planned]
			{
				planned = writer.GetUnfinished().value_or(SOpJournalEntry {}).Op.PlannedGuids;
			});
			(void)rig.Migrate();
			ASSERT_EQ(planned.size(), 1u) << "the stopped op holds the GUID it promised";
			EXPECT_TRUE(planned[0] == DerivedGuid(P("A.png"), 4u) || planned[0] == DerivedGuid(P("B.png"), 5u));
			(void)rig.Migrate();
		}
		for (const FStringView file : { FStringView("A.png.meta"), FStringView("B.png.meta") })
		{
			EXPECT_EQ(tree.Read(file), reference.Read(file)) << stop;
		}
		EXPECT_EQ(tree.Migrations(), FString(MigrationsWithMetas)) << stop;
	}

	// One real kill, after the first op's write: start-up rolls it forward, and the run again completes and records.
	CVerbTree killed("Вбита міграція");
	killed.WritePng("A.png", 4u);
	killed.WritePng("B.png", 5u);
	const std::optional<SOpJournalEntry> left = KillMidVerb(killed, "migrate", 1u);
	ASSERT_TRUE(left.has_value()) << "the killed process left its op in the journal";
	EXPECT_EQ(left->Op.Verb, "create-meta");
	ASSERT_EQ(left->Op.PlannedGuids.size(), 1u);
	{
		CVerbRig rig(killed);
		ASSERT_TRUE(rig.bOpened);
		EXPECT_FALSE(rig.Repairs().GetWriter()->GetUnfinished().has_value()) << "rolled forward before the first scan";
		ASSERT_TRUE(rig.Migrate().bRecorded);
	}
	for (const FStringView file : { FStringView("A.png.meta"), FStringView("B.png.meta") })
	{
		EXPECT_EQ(killed.Read(file), reference.Read(file));
	}
	EXPECT_TRUE(left->Op.PlannedGuids[0] == killed.MetaGuid("A.png") || left->Op.PlannedGuids[0] == killed.MetaGuid("B.png"));
}

TEST(MetaVerbs, EveryCrashPointOfFixRollsForwardToTheDedupedCopyAndOneRealKillDoesToo)
{
	// In-process: fix's dedupe op of a copied folder (one op, both losers) stopped after k steps, then rolled forward.
	for (uint32 stop = 0u; stop <= 2u; ++stop)
	{
		CVerbTree tree(FString::Format("Збій виправлення {}", stop));
		WritePair(tree, "Pack");
		WritePair(tree, "Pack2");
		CVerbRig rig(tree);
		ASSERT_TRUE(rig.bOpened);
		std::vector<SGuid> planned;
		CAssetWriter& writer = *rig.Repairs().GetWriter();
		writer.SetStopAfterStepForTest(stop, [&writer, &planned]
		{
			planned = writer.GetUnfinished().value_or(SOpJournalEntry {}).Op.PlannedGuids;
		});
		(void)rig.Validate(std::nullopt, false, true);
		EXPECT_EQ(planned.size(), 2u) << "both losers' new GUIDs were promised before the first write";
		FString error;
		EXPECT_TRUE(writer.RollForward(&error)) << error.CStr();
		ExpectCopyDeduped(tree, FString::Format("stopped after {}", stop));
	}

	// One real kill, after the first loser's write; start-up rolls the op forward.
	CVerbTree killed("Вбите виправлення");
	WritePair(killed, "Pack");
	WritePair(killed, "Pack2");
	const std::optional<SOpJournalEntry> left = KillMidVerb(killed, "fix", 1u);
	ASSERT_TRUE(left.has_value()) << "the killed process left its op in the journal";
	EXPECT_EQ(left->Op.Verb, "dedupe");
	EXPECT_EQ(left->Op.PlannedGuids.size(), 2u);
	CVerbRig rig(killed);
	ASSERT_TRUE(rig.bOpened);
	EXPECT_FALSE(rig.Repairs().GetWriter()->GetUnfinished().has_value());
	ExpectCopyDeduped(killed, "killed after one step");
}
