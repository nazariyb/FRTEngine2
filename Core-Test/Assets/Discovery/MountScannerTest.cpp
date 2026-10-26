// The stat scan (Assets/Discovery/MountScanner.h, design §5.4 step 4, §3.1, §13.4): a generated tree with
// non-ASCII names comes back in the batch order ScanTypes.h promises, whatever the thread count; the
// Unchanged test is equality of size and mtime, identity-only differences are their own state, and nothing
// is opened; the membership protocol holds - OpenScope for the root, ShouldDescend before a folder is
// listed (an excluded folder's rule file is never read), EnterFolder with the folder's Assets.yml before
// any of its files is classified, its scope and RulesHash kept per folder; rule files and built-in
// excludes never appear as files; junctions are never followed; folders that cannot be listed are named;
// subtree scans check their root; and the portable-name rules (PATH001 on a case-sensitive mount and for
// U+212A KELVIN SIGN on plain NTFS, PATH002, PATH003) are reported with on-disk spellings kept. Also the
// built-in exclude list, the lazy file hash the watcher shares, and a 20k-file warm scan against §5.11.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "DiscoveryTestUtils.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/BuiltinExcludes.h"
#include "Assets/Discovery/MountScanner.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/FileHash.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Thread.h"
#include "Platform/Timestamp.h"

using namespace frt;
using namespace frt::assets;
using frt::test::discovery::CDiscoveryTree;
using frt::test::discovery::WriteText;


namespace
{
void MakeDirectories (const std::filesystem::path& Path)
{
	std::error_code ec;
	std::filesystem::create_directories(Path, ec);
}

CMountTable ResolveTree (const CDiscoveryTree& Tree)
{
	MakeDirectories(Tree.EngineContent());
	MakeDirectories(Tree.EngineConfig());
	MakeDirectories(Tree.ProjectContent());
	SMountRoots roots;
	roots.EngineDir = Tree.EngineDir();
	roots.EngineContentDir = Tree.EngineContent();
	roots.EngineConfigDir = Tree.EngineConfig();
	roots.ProjectDir = Tree.ProjectDir();
	return CMountTable::Resolve(roots, nullptr);
}

std::unique_ptr<jobs::CWorkerPool> MakePool (uint32 Threads)
{
	jobs::SWorkerPoolDesc desc;
	desc.ThreadCount = Threads;
	desc.Name = "Scan Test";
	return std::make_unique<jobs::CWorkerPool>(desc);
}

// The known-files seam over a map, byte-exact like the registry's.
class CKnownMap final : public IKnownFiles
{
public:
	std::optional<SKnownFile> Find (FStringView FileMountPath) const override
	{
		const auto found = Files.find(FileMountPath);
		return found != Files.end() ? std::optional(found->second) : std::nullopt;
	}

	std::unordered_map<FString, SKnownFile, SStringHash, SStringEqual> Files;
};

CKnownMap KnowEverything (const SScanBatch& Batch)
{
	CKnownMap known;
	for (const SScanFile& file : Batch.Files)
	{
		known.Files[MakeChildMountPath(Batch.Folders[file.Folder].MountPath, file.Name)] = SKnownFile { file.Size, file.MTime, file.Identity };
	}
	return known;
}

FString FilePath (const SScanBatch& Batch, const SScanFile& File)
{
	return MakeChildMountPath(Batch.Folders[File.Folder].MountPath, File.Name);
}

const SScanFile* FindFile (const SScanBatch& Batch, FStringView MountPath)
{
	for (const SScanFile& file : Batch.Files)
	{
		if (FilePath(Batch, file) == MountPath)
		{
			return &file;
		}
	}
	return nullptr;
}

std::vector<const SDiagnostic*> FindDiagnostics (const SScanBatch& Batch, uint32 Code)
{
	std::vector<const SDiagnostic*> found;
	for (const SDiagnostic& diagnostic : Batch.Diagnostics)
	{
		if (diagnostic.Code == Code)
		{
			found.push_back(&diagnostic);
		}
	}
	return found;
}

// Everything a batch says except timings and scopes, one line per item, for exact comparisons.
FString DumpBatch (const SScanBatch& Batch)
{
	FString text = FString::Format("root {}\n", Batch.RootMountPath);
	for (const SScanFolder& folder : Batch.Folders)
	{
		text.AppendFormat("folder {} parent {} rules {} {:x}\n", folder.MountPath, folder.Parent == ~0u ? -1 : static_cast<int64>(folder.Parent),
			folder.bHasRuleFile, folder.RulesHash);
	}
	for (const SScanFile& file : Batch.Files)
	{
		text.AppendFormat("file {} {} size {} class {} quiet {} state {}\n", file.Folder, file.Name, file.Size,
			enum_::ToString(file.Verdict.Class), file.Verdict.bQuiet, enum_::ToString(file.State));
	}
	for (const FString& failed : Batch.FailedFolders)
	{
		text.AppendFormat("failed {}\n", failed);
	}
	for (const FString& link : Batch.SkippedLinks)
	{
		text.AppendFormat("link {}\n", link);
	}
	for (const SDiagnostic& diagnostic : Batch.Diagnostics)
	{
		text.AppendFormat("diagnostic {} {} {}: {}\n", FormatDiagnosticCode(diagnostic.Code), GetSeverityName(diagnostic.Severity),
			diagnostic.Subject, diagnostic.Message);
	}
	for (const SDiagnostic& diagnostic : Batch.RuleDiagnostics)
	{
		text.AppendFormat("rule diagnostic {} {} {}: {}\n", FormatDiagnosticCode(diagnostic.Code), GetSeverityName(diagnostic.Severity),
			diagnostic.Subject, diagnostic.Message);
	}
	return text;
}

std::vector<FString> FolderPaths (const SScanBatch& Batch)
{
	std::vector<FString> paths;
	for (const SScanFolder& folder : Batch.Folders)
	{
		paths.push_back(folder.MountPath);
	}
	return paths;
}

std::vector<FString> FilePaths (const SScanBatch& Batch)
{
	std::vector<FString> paths;
	for (const SScanFile& file : Batch.Files)
	{
		paths.push_back(FilePath(Batch, file));
	}
	return paths;
}


/**
 * An oracle that records every call in order (one log for all workers) and applies a few rules: folders
 * named in Excluded are not descended, files ending in ".raw" are Raw, ".unk" Unclaimed, ".quiet" quiet,
 * ".skip" excluded. EnterFolder gives every folder a scope of its own naming it, and a RulesHash derived
 * from the rule file's path; a rule file holding "bad" adds RULE001 and keeps the parent's scope.
 */
struct STestScope final : SScanScope
{
	FString Folder;
};

class CRecordingMembership final : public IScanMembership
{
public:
	std::shared_ptr<const SScanScope> OpenScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const override
	{
		Record(FString::Format("open {}", FolderMountPath));
		if (bOpenScopeDiagnostic)
		{
			SDiagnostic diagnostic;
			diagnostic.Code = Rule001;
			diagnostic.Subject = FString(FolderMountPath);
			diagnostic.Message = "from OpenScope";
			OutDiagnostics.push_back(diagnostic);
		}
		auto scope = std::make_shared<STestScope>();
		scope->Folder = "(above)";
		return scope;
	}

	std::shared_ptr<const SScanScope> EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		const std::filesystem::path* RuleFile, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const override
	{
		Record(FString::Format("enter {} {}", FolderMountPath, RuleFile != nullptr ? FString::FromPath(RuleFile->filename()) : FString("-")));
		if (OnEnter)
		{
			OnEnter(FolderMountPath);
		}
		OutRulesHash = 0u;
		if (RuleFile == nullptr)
		{
			return Parent;
		}
		const std::optional<FString> text = platform::ReadFileText(*RuleFile);
		if (text.has_value() && text->View().Contains(FStringView("bad")))
		{
			SDiagnostic diagnostic;
			diagnostic.Code = Rule001;
			diagnostic.Subject = FString(FolderMountPath);
			diagnostic.Message = "bad rules";
			OutDiagnostics.push_back(diagnostic);
			return Parent;
		}
		OutRulesHash = hash::Xxh3_64(FolderMountPath.AsBytes());
		auto scope = std::make_shared<STestScope>();
		scope->Folder = FString(FolderMountPath);
		return scope;
	}

	bool ShouldDescend (const SScanScope& Scope, FStringView FolderMountPath) const override
	{
		Record(FString::Format("descend {} in {}", FolderMountPath, static_cast<const STestScope&>(Scope).Folder));
		const FStringView leaf = GetMountPathLeaf(FolderMountPath);
		return std::find(Excluded.begin(), Excluded.end(), leaf) == Excluded.end();
	}

	std::optional<SFileVerdict> Classify (const SScanScope& Scope, FStringView FileMountPath) const override
	{
		Record(FString::Format("classify {} in {}", FileMountPath, static_cast<const STestScope&>(Scope).Folder));
		if (FileMountPath.EndsWith(".skip"))
		{
			return std::nullopt;
		}
		SFileVerdict verdict;
		if (FileMountPath.EndsWith(".raw"))
		{
			verdict.Class = EFileClass::Raw;
		}
		else if (FileMountPath.EndsWith(".unk"))
		{
			verdict.Class = EFileClass::Unclaimed;
		}
		verdict.bQuiet = FileMountPath.EndsWith(".quiet");
		return verdict;
	}

	[[nodiscard]] std::vector<FString> GetEvents () const
	{
		const std::lock_guard lock(Mutex);
		return Events;
	}

	std::vector<FString> Excluded;
	bool bOpenScopeDiagnostic = false;
	std::function<void (FStringView)> OnEnter;

private:
	void Record (FString Event) const
	{
		const std::lock_guard lock(Mutex);
		Events.push_back(std::move(Event));
	}

	mutable std::mutex Mutex;
	mutable std::vector<FString> Events;
};

ptrdiff_t IndexOf (const std::vector<FString>& Events, FStringView Prefix)
{
	for (size_t i = 0u; i < Events.size(); ++i)
	{
		if (Events[i].View().StartsWith(Prefix))
		{
			return static_cast<ptrdiff_t>(i);
		}
	}
	return -1;
}
}


// ---------------- The batch ----------------

TEST(MountScannerTest, AGeneratedTreeComesBackInBatchOrderWithoutExcludedNamesOrRuleFiles)
{
	CDiscoveryTree tree("Order");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	for (const FStringView file : { FStringView("b.png"), FStringView("A.png"), FStringView("Ґрунт.png"), FStringView("漢字.txt"),
		FStringView("Thumbs.db"), FStringView("x.tmp"), FStringView("a.png.meta"), FStringView("Assets.yml"), FStringView("Textures/Rock.png"),
		FStringView("Textures/assets.YML"), FStringView(".git/config"), FStringView("Models/Hero/Hero.gltf"), FStringView("Models/Hero/Hero.bin"),
		FStringView("Емодзі 😀/файл.png"), FStringView("Models/Hero/Hero.blend1"), FStringView("Models/Old.tmp/kept.png") })
	{
		ASSERT_TRUE(WriteText(content / file.ToPath(), file));
	}
	MakeDirectories(content / "textures2");

	const auto pool = MakePool(4u);
	const CDefaultScanMembership membership;
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);

	EXPECT_EQ(batch.RootMountPath, "/Project");
	const std::vector<FString> expectedFolders = { "/Project", "/Project/Models", "/Project/Models/Hero", "/Project/Models/Old.tmp",
		"/Project/Textures", "/Project/textures2", "/Project/Емодзі 😀" };
	EXPECT_EQ(FolderPaths(batch), expectedFolders);
	const std::vector<uint32> expectedParents = { ~0u, 0u, 1u, 1u, 0u, 0u, 0u };
	for (uint32 i = 0u; i < batch.Folders.size() && i < expectedParents.size(); ++i)
	{
		EXPECT_EQ(batch.Folders[i].Parent, expectedParents[i]) << batch.Folders[i].MountPath.CStr();
		EXPECT_EQ(batch.Folders[i].RulesHash, 0u);
		EXPECT_NE(batch.Folders[i].Scope, nullptr);
	}
	ASSERT_EQ(batch.Folders.size(), expectedFolders.size());
	EXPECT_TRUE(batch.Folders[0].bHasRuleFile);
	EXPECT_TRUE(batch.Folders[4].bHasRuleFile);   // "assets.YML": the reserved name under the fold
	EXPECT_FALSE(batch.Folders[1].bHasRuleFile);

	const std::vector<FString> expectedFiles = { "/Project/A.png", "/Project/b.png", "/Project/Ґрунт.png", "/Project/漢字.txt",
		"/Project/Models/Hero/Hero.bin", "/Project/Models/Hero/Hero.gltf", "/Project/Models/Old.tmp/kept.png", "/Project/Textures/Rock.png",
		"/Project/Емодзі 😀/файл.png" };
	EXPECT_EQ(FilePaths(batch), expectedFiles);
	for (const SScanFile& file : batch.Files)
	{
		EXPECT_EQ(file.State, EScanFileState::New);
		EXPECT_EQ(file.Verdict.Class, EFileClass::Asset);
		EXPECT_EQ(file.Size, FilePath(batch, file).Len() - 9u);   // each file holds its own relative path
		EXPECT_TRUE(file.Identity.IsValid());
		EXPECT_NE(file.MTime, 0);
	}
	EXPECT_TRUE(batch.FailedFolders.empty());
	EXPECT_TRUE(batch.SkippedLinks.empty());
	EXPECT_TRUE(batch.Diagnostics.empty()) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.FoldersListed, 7u);
	EXPECT_GE(batch.Milliseconds, 0.0);
}

// ScanAsync hands the batch over as a pool completion: on the owner thread, inside PumpCompletions.
TEST(MountScannerTest, ScanAsyncCompletesOnTheOwnerThread)
{
	CDiscoveryTree tree("Async");
	const CMountTable table = ResolveTree(tree);
	ASSERT_TRUE(WriteText(tree.ProjectContent() / "a.png"));
	ASSERT_TRUE(WriteText(tree.ProjectContent() / "Sub" / "b.png"));

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	std::optional<SScanBatch> result;
	uint64 completionThread = 0u;
	CMountScanner::ScanAsync(*pool, *table.Find("Project"), "/Project", membership, nullptr, [&] (SScanBatch&& Batch)
	{
		completionThread = platform::GetCurrentThreadId();
		result = std::move(Batch);
	});
	EXPECT_FALSE(result.has_value());
	ASSERT_TRUE(pool->PumpUntil([&] { return result.has_value(); }, 30.0));
	EXPECT_EQ(completionThread, platform::GetCurrentThreadId());
	EXPECT_EQ(result->Files.size(), 2u);
	EXPECT_TRUE(pool->IsIdle());
}

TEST(MountScannerTest, TheBatchIsTheSameWhateverTheThreadCount)
{
	CDiscoveryTree tree("Determinism");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	const FStringView names[] = { "Ґ", "a", "B", "漢", "z😀", "Ä", "ä2", "k" };
	for (uint32 a = 0u; a < 6u; ++a)
	{
		for (uint32 b = 0u; b < 5u; ++b)
		{
			const std::filesystem::path folder = content / FString::Format("{} {}", names[a], a).ToPath() / FString::Format("{}{}", names[b + 1u], b).ToPath();
			for (uint32 f = 0u; f < 8u; ++f)
			{
				ASSERT_TRUE(WriteText(folder / FString::Format("{} файл {}.png", names[f], f).ToPath(), FString::Format("{}", a * b * f)));
			}
		}
	}
	// "K" + KELVIN SIGN and "kk": two names to NTFS, one under the portable fold (PATH001 twice).
	ASSERT_TRUE(WriteText(content / FStringView("K\xE2\x84\xAA.png").ToPath()));
	ASSERT_TRUE(WriteText(content / "kk.png"));

	const CDefaultScanMembership membership;
	FString reference;
	for (const uint32 threads : { 1u, 3u, 16u })
	{
		const auto pool = MakePool(threads);
		const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
		EXPECT_EQ(batch.Files.size(), 6u * 5u * 8u + 2u);
		const FString dump = DumpBatch(batch);
		if (reference.IsEmpty())
		{
			reference = dump;
		}
		else
		{
			EXPECT_EQ(dump, reference) << threads << " threads";
		}
	}
	EXPECT_EQ(reference.View().CountOccurrences("PATH001"), 2u) << reference.CStr();
}


// ---------------- Known files ----------------

// Unchanged: size and mtime both equal (any other difference is Changed, older times included);
// IdentityChanged: only the identity differs; New: not known, matched byte-exact.
TEST(MountScannerTest, KnownFilesDecideUnchangedChangedAndIdentityOnly)
{
	CDiscoveryTree tree("Known");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	for (const FStringView name : { FStringView("Same.png"), FStringView("Older.png"), FStringView("Bigger.png"), FStringView("Moved.png"),
		FStringView("New.png"), FStringView("Case.png") })
	{
		ASSERT_TRUE(WriteText(content / "Тест" / name.ToPath(), "1234"));
	}

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SMount& project = *table.Find("Project");
	const SScanBatch first = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, nullptr);
	ASSERT_EQ(first.Files.size(), 6u);

	CKnownMap known = KnowEverything(first);
	known.Files.erase(FString("/Project/Тест/New.png"));
	known.Files["/Project/Тест/Older.png"].MTime += 1;   // the record is newer than the disk: still Changed
	known.Files["/Project/Тест/Bigger.png"].Size += 1;
	known.Files["/Project/Тест/Moved.png"].Identity.Bytes[0] ^= 0xFFu;
	const SKnownFile caseRecord = known.Files["/Project/Тест/Case.png"];
	known.Files.erase(FString("/Project/Тест/Case.png"));
	known.Files["/Project/Тест/case.png"] = caseRecord;   // an offline case-only rename: the listed spelling is New

	const SScanBatch second = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, &known);
	const auto stateOf = [&second] (FStringView Path)
	{
		const SScanFile* file = FindFile(second, Path);
		return file != nullptr ? file->State : EScanFileState::New;
	};
	EXPECT_EQ(stateOf("/Project/Тест/Same.png"), EScanFileState::Unchanged);
	EXPECT_EQ(stateOf("/Project/Тест/Older.png"), EScanFileState::Changed);
	EXPECT_EQ(stateOf("/Project/Тест/Bigger.png"), EScanFileState::Changed);
	EXPECT_EQ(stateOf("/Project/Тест/Moved.png"), EScanFileState::IdentityChanged);
	EXPECT_EQ(stateOf("/Project/Тест/New.png"), EScanFileState::New);
	EXPECT_EQ(stateOf("/Project/Тест/Case.png"), EScanFileState::New);
	EXPECT_NE(FindFile(second, "/Project/Тест/New.png"), nullptr);

	// A rewrite with other bytes of the same length changes the mtime: Changed.
	ASSERT_TRUE(WriteText(content / "Тест" / "Same.png", "5678"));
	known.Files["/Project/Тест/Same.png"] = SKnownFile { FindFile(first, "/Project/Тест/Same.png")->Size,
		FindFile(first, "/Project/Тест/Same.png")->MTime - 10'000'000, FindFile(first, "/Project/Тест/Same.png")->Identity };
	const SScanBatch third = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, &known);
	EXPECT_EQ(FindFile(third, "/Project/Тест/Same.png")->State, EScanFileState::Changed);
}


// ---------------- The membership protocol ----------------

TEST(MountScannerTest, ExcludedFoldersAreNeverListedAndTheirRuleFilesNeverRead)
{
	CDiscoveryTree tree("Excluded");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "Assets.yml", "root rules"));
	ASSERT_TRUE(WriteText(content / "Kept" / "Assets.yml", "kept rules"));
	ASSERT_TRUE(WriteText(content / "Kept" / "a.png"));
	ASSERT_TRUE(WriteText(content / "Kept" / "Inner" / "b.raw"));
	ASSERT_TRUE(WriteText(content / "Kept" / "c.unk"));
	ASSERT_TRUE(WriteText(content / "Kept" / "d.quiet"));
	ASSERT_TRUE(WriteText(content / "Kept" / "e.skip"));
	ASSERT_TRUE(WriteText(content / "Відкинуто" / "Assets.yml", "never read"));
	ASSERT_TRUE(WriteText(content / "Відкинуто" / "x.png"));
	ASSERT_TRUE(WriteText(content / "Відкинуто" / "Deep" / "y.png"));

	CRecordingMembership membership;
	membership.Excluded = { "Відкинуто" };
	const auto pool = MakePool(4u);
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);

	const std::vector<FString> expectedFolders = { "/Project", "/Project/Kept", "/Project/Kept/Inner" };
	EXPECT_EQ(FolderPaths(batch), expectedFolders);
	const std::vector<FString> expectedFiles = { "/Project/Kept/a.png", "/Project/Kept/c.unk", "/Project/Kept/d.quiet", "/Project/Kept/Inner/b.raw" };
	EXPECT_EQ(FilePaths(batch), expectedFiles);
	EXPECT_EQ(FindFile(batch, "/Project/Kept/Inner/b.raw")->Verdict.Class, EFileClass::Raw);
	EXPECT_EQ(FindFile(batch, "/Project/Kept/c.unk")->Verdict.Class, EFileClass::Unclaimed);
	EXPECT_TRUE(FindFile(batch, "/Project/Kept/d.quiet")->Verdict.bQuiet);
	EXPECT_EQ(batch.FoldersListed, 3u);

	// Rule files: each folder's RulesHash and scope are what its EnterFolder returned.
	EXPECT_TRUE(batch.Folders[0].bHasRuleFile);
	EXPECT_EQ(batch.Folders[0].RulesHash, hash::Xxh3_64(FStringView("/Project").AsBytes()));
	EXPECT_EQ(static_cast<const STestScope&>(*batch.Folders[0].Scope).Folder, "/Project");
	EXPECT_EQ(batch.Folders[1].RulesHash, hash::Xxh3_64(FStringView("/Project/Kept").AsBytes()));
	EXPECT_EQ(static_cast<const STestScope&>(*batch.Folders[1].Scope).Folder, "/Project/Kept");
	EXPECT_FALSE(batch.Folders[2].bHasRuleFile);
	EXPECT_EQ(batch.Folders[2].RulesHash, 0u);
	EXPECT_EQ(static_cast<const STestScope&>(*batch.Folders[2].Scope).Folder, "/Project/Kept");   // the parent's, unchanged

	const std::vector<FString> events = membership.GetEvents();
	FString all;
	for (const FString& event : events)
	{
		all.AppendFormat("{}\n", event);
	}
	// The excluded folder: asked about, with the parent's scope, and nothing else.
	EXPECT_GE(IndexOf(events, "descend /Project/Відкинуто in /Project"), 0) << all.CStr();
	EXPECT_LT(IndexOf(events, "enter /Project/Відкинуто"), 0) << all.CStr();
	EXPECT_LT(IndexOf(events, "classify /Project/Відкинуто"), 0) << all.CStr();
	// The root: OpenScope, then ShouldDescend with the scope above it, then EnterFolder with its rule file.
	const ptrdiff_t open = IndexOf(events, "open /Project");
	const ptrdiff_t descendRoot = IndexOf(events, "descend /Project in (above)");
	const ptrdiff_t enterRoot = IndexOf(events, "enter /Project Assets.yml");
	EXPECT_EQ(open, 0) << all.CStr();
	EXPECT_GT(descendRoot, open) << all.CStr();
	EXPECT_GT(enterRoot, descendRoot) << all.CStr();
	// Every file is classified after its folder was entered, in that folder's scope.
	for (const FStringView file : { FStringView("/Project/Kept/a.png"), FStringView("/Project/Kept/e.skip"), FStringView("/Project/Kept/Inner/b.raw") })
	{
		const FString folder(GetMountPathParent(file));
		const ptrdiff_t enter = IndexOf(events, FString::Format("enter {} ", folder));
		const ptrdiff_t classify = IndexOf(events, FString::Format("classify {} in ", file));
		EXPECT_GE(enter, 0) << all.CStr();
		EXPECT_GT(classify, enter) << file.GetData() << "\n" << all.CStr();
	}
	EXPECT_GE(IndexOf(events, "classify /Project/Kept/Inner/b.raw in /Project/Kept"), 0) << all.CStr();
	EXPECT_GE(IndexOf(events, "descend /Project/Kept/Inner in /Project/Kept"), 0) << all.CStr();
	// The rule files themselves are never classified.
	EXPECT_LT(IndexOf(events, "classify /Project/Assets.yml"), 0) << all.CStr();
	EXPECT_LT(IndexOf(events, "classify /Project/Kept/Assets.yml"), 0) << all.CStr();
}

// The oracle's diagnostics land in the batch's RuleDiagnostics (never among the scan's own): OpenScope's first,
// then per folder in batch order; a malformed rule file keeps the parent's scope and reports RulesHash 0 (no last
// good rules at a cold start).
TEST(MountScannerTest, TheOraclesDiagnosticsAreInTheBatch)
{
	CDiscoveryTree tree("Oracle diagnostics");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "Broken" / "Assets.yml", "bad"));
	ASSERT_TRUE(WriteText(content / "Broken" / "a.png"));

	CRecordingMembership membership;
	membership.bOpenScopeDiagnostic = true;
	const auto pool = MakePool(2u);
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
	EXPECT_TRUE(FindDiagnostics(batch, Rule001).empty()) << DumpBatch(batch).CStr();
	const std::vector<SDiagnostic>& rules = batch.RuleDiagnostics;
	ASSERT_EQ(rules.size(), 2u) << DumpBatch(batch).CStr();
	EXPECT_EQ(rules[0].Message, "from OpenScope");
	EXPECT_EQ(rules[1].Subject, "/Project/Broken");
	ASSERT_EQ(batch.Folders.size(), 2u);
	EXPECT_TRUE(batch.Folders[1].bHasRuleFile);
	EXPECT_EQ(batch.Folders[1].RulesHash, 0u);
	EXPECT_EQ(batch.Folders[1].Scope, batch.Folders[0].Scope);
}

// A folder whose listing fails is named in FailedFolders (and is in Folders, empty), with a warning; the
// rest of the tree is scanned. Made by removing the folder between its parent's listing and its own.
TEST(MountScannerTest, AFolderThatCannotBeListedIsNamedAndTheRestIsScanned)
{
	CDiscoveryTree tree("Failed folder");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "Gone" / "a.png"));
	ASSERT_TRUE(WriteText(content / "Stays" / "b.png"));

	CRecordingMembership membership;
	membership.OnEnter = [&content] (FStringView Folder)
	{
		if (Folder == "/Project")
		{
			test::discovery::RemoveTree(content / "Gone");
		}
	};
	const auto pool = MakePool(2u);
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
	const std::vector<FString> expectedFolders = { "/Project", "/Project/Gone", "/Project/Stays" };
	EXPECT_EQ(FolderPaths(batch), expectedFolders);
	ASSERT_EQ(batch.FailedFolders.size(), 1u);
	EXPECT_EQ(batch.FailedFolders[0], "/Project/Gone");
	EXPECT_EQ(FilePaths(batch), std::vector<FString> { "/Project/Stays/b.png" });
	ASSERT_EQ(batch.Diagnostics.size(), 1u) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.Diagnostics[0].Subject, "/Project/Gone");
	EXPECT_EQ(batch.Diagnostics[0].Severity, ESeverity::Warning);
	EXPECT_EQ(batch.FoldersListed, 2u);
}


// ---------------- Roots ----------------

TEST(MountScannerTest, ASubtreeScanStartsAtItsRootWithTheScopeAboveIt)
{
	CDiscoveryTree tree("Subtree");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "Моделі" / "Assets.yml", "rules"));
	ASSERT_TRUE(WriteText(content / "Моделі" / "Hero" / "Hero.gltf"));
	ASSERT_TRUE(WriteText(content / "Моделі" / "a.png"));
	ASSERT_TRUE(WriteText(content / "Other" / "b.png"));

	CRecordingMembership membership;
	const auto pool = MakePool(2u);
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project/Моделі", membership, nullptr);
	EXPECT_EQ(batch.RootMountPath, "/Project/Моделі");
	const std::vector<FString> expectedFolders = { "/Project/Моделі", "/Project/Моделі/Hero" };
	EXPECT_EQ(FolderPaths(batch), expectedFolders);
	EXPECT_EQ(batch.Folders[0].Parent, ~0u);
	EXPECT_TRUE(batch.Folders[0].bHasRuleFile);
	EXPECT_NE(batch.Folders[0].RulesHash, 0u);
	EXPECT_EQ(FilePaths(batch), (std::vector<FString> { "/Project/Моделі/a.png", "/Project/Моделі/Hero/Hero.gltf" }));
	const std::vector<FString> events = membership.GetEvents();
	EXPECT_EQ(IndexOf(events, "open /Project/Моделі"), 0);
	EXPECT_GT(IndexOf(events, "descend /Project/Моделі in (above)"), 0);
	EXPECT_TRUE(batch.Diagnostics.empty()) << DumpBatch(batch).CStr();

	// An excluded root: nothing, and its rule file is not read.
	membership.Excluded = { "Моделі" };
	const SScanBatch excluded = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project/Моделі", membership, nullptr);
	EXPECT_TRUE(excluded.Folders.empty());
	EXPECT_TRUE(excluded.Files.empty());
	EXPECT_TRUE(excluded.FailedFolders.empty());
}

TEST(MountScannerTest, ARootThatIsNotThereHoldsNothingAndAnInvalidOneFails)
{
	CDiscoveryTree tree("Roots");
	const CMountTable table = ResolveTree(tree);
	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SMount& project = *table.Find("Project");

	const SScanBatch missing = CMountScanner::ScanBlocking(*pool, project, "/Project/Немає", membership, nullptr);
	EXPECT_TRUE(missing.Folders.empty());
	EXPECT_TRUE(missing.FailedFolders.empty());
	EXPECT_TRUE(missing.Diagnostics.empty());

	// A mount whose content directory does not exist.
	test::discovery::RemoveTree(tree.ProjectContent());
	const SScanBatch noContent = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, nullptr);
	EXPECT_TRUE(noContent.Folders.empty());
	EXPECT_TRUE(noContent.FailedFolders.empty());

	for (const FStringView bad : { FStringView("/Engine/A"), FStringView("Project/A"), FStringView("/Project/../Engine"), FStringView("/Project/a\\b"),
		FStringView("/project/A") })
	{
		const SScanBatch failed = CMountScanner::ScanBlocking(*pool, project, bad, membership, nullptr);
		EXPECT_EQ(failed.RootMountPath, bad);
		EXPECT_TRUE(failed.Folders.empty());
		ASSERT_EQ(failed.FailedFolders.size(), 1u) << bad.GetData();
		ASSERT_EQ(failed.Diagnostics.size(), 1u);
		EXPECT_EQ(failed.Diagnostics[0].Severity, ESeverity::Error);
	}
}


// A root is "not there" (an empty batch, nothing failed, which a consumer reads as "remove everything
// below") only when its parent's listing has no entry it could be opened as. On a case-insensitive mount a
// subtree root spelled otherwise than on disk exists: it fails, naming the on-disk spelling, and is never
// an empty clean batch. A mount root matches its content directory's name by NTFS's rule.
TEST(MountScannerTest, ARootSpelledOtherwiseOnDiskIsNeverAnEmptyCleanBatch)
{
	CDiscoveryTree tree("Root spelling");
	const CMountTable table = ResolveTree(tree);
	const SMount& project = *table.Find("Project");
	ASSERT_FALSE(project.Volume.bCaseSensitive);
	ASSERT_TRUE(WriteText(tree.ProjectContent() / "Textures" / "Rock.png"));
	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;

	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, project, "/Project/textures", membership, nullptr);
	EXPECT_TRUE(batch.Folders.empty());
	EXPECT_TRUE(batch.Files.empty());
	ASSERT_EQ(batch.FailedFolders, std::vector<FString> { "/Project/textures" }) << DumpBatch(batch).CStr();
	ASSERT_EQ(batch.Diagnostics.size(), 1u) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.Diagnostics[0].Severity, ESeverity::Warning);
	EXPECT_EQ(batch.Diagnostics[0].Subject, "/Project/textures");
	EXPECT_TRUE(batch.Diagnostics[0].Message.View().Contains(FStringView("'Textures'")));

	// Under its own spelling it scans.
	const SScanBatch exact = CMountScanner::ScanBlocking(*pool, project, "/Project/Textures", membership, nullptr);
	EXPECT_EQ(FilePaths(exact), std::vector<FString> { "/Project/Textures/Rock.png" });
	EXPECT_TRUE(exact.FailedFolders.empty());

	// A mount whose content directory is configured in another case still scans.
	SMount otherCase = project;
	otherCase.ContentDir = otherCase.ContentDir.parent_path() / "CONTENT";
	const SScanBatch mount = CMountScanner::ScanBlocking(*pool, otherCase, "/Project", membership, nullptr);
	EXPECT_EQ(FilePaths(mount), std::vector<FString> { "/Project/Textures/Rock.png" });
	EXPECT_TRUE(mount.FailedFolders.empty());
}

// A content directory that its parent still lists but that cannot be opened (here: delete-pending, so
// every open and stat fails) is a failed root, not an empty mount.
TEST(MountScannerTest, AContentDirectoryThatCannotBeStattedIsAFailedRootNotAnEmptyMount)
{
	CDiscoveryTree tree("Unstattable root");
	const CMountTable table = ResolveTree(tree);
	const SMount& project = *table.Find("Project");
	const HANDLE handle = CreateFileW(test::discovery::ToVerbatim(tree.ProjectContent()).c_str(), DELETE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	ASSERT_NE(handle, INVALID_HANDLE_VALUE);
	FILE_DISPOSITION_INFO disposition = {};
	disposition.DeleteFile = TRUE;
	ASSERT_TRUE(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)));
	bool bListed = false;
	platform::EnumerateDirectory(tree.ProjectDir(), [&bListed] (const platform::SDirectoryEntry& Entry) { bListed = bListed || Entry.Name == "Content"; });
	const bool bStatted = platform::Stat(tree.ProjectContent()).has_value();

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, nullptr);
	CloseHandle(handle);
	if (!bListed || bStatted)
	{
		GTEST_SKIP() << "a delete-pending directory is not listed-but-unopenable here";
	}
	EXPECT_EQ(FolderPaths(batch), std::vector<FString> { "/Project" }) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.FailedFolders, std::vector<FString> { "/Project" });
}

// An oracle that breaks OpenScope's contract fails the root instead of crashing the worker.
TEST(MountScannerTest, ANullScopeFromOpenScopeFailsTheRoot)
{
	class CNoScopeMembership final : public IScanMembership
	{
	public:
		std::shared_ptr<const SScanScope> OpenScope (FStringView, std::vector<SDiagnostic>&) const override { return nullptr; }
		std::shared_ptr<const SScanScope> EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView, const std::filesystem::path*,
			uint64& OutRulesHash, std::vector<SDiagnostic>&) const override
		{
			OutRulesHash = 0u;
			return Parent;
		}
		bool ShouldDescend (const SScanScope&, FStringView) const override { return true; }
		std::optional<SFileVerdict> Classify (const SScanScope&, FStringView) const override { return SFileVerdict {}; }
	};

	CDiscoveryTree tree("Null scope");
	const CMountTable table = ResolveTree(tree);
	ASSERT_TRUE(WriteText(tree.ProjectContent() / "Sub" / "a.png"));
	const auto pool = MakePool(2u);
	const CNoScopeMembership membership;
	for (const FStringView root : { FStringView("/Project"), FStringView("/Project/Sub") })
	{
		const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), root, membership, nullptr);
		EXPECT_TRUE(batch.Folders.empty());
		EXPECT_EQ(batch.FailedFolders, std::vector<FString> { FString(root) });
		ASSERT_EQ(batch.Diagnostics.size(), 1u) << DumpBatch(batch).CStr();
		EXPECT_EQ(batch.Diagnostics[0].Severity, ESeverity::Error);
	}
}


// ---------------- Links ----------------

// A junction is a name surrogate: listed as such, never followed - in a mount scan or as a subtree root.
TEST(MountScannerTest, JunctionsAreSkippedAndNothingBelowThemIsRecorded)
{
	CDiscoveryTree tree("Junction");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	const std::filesystem::path outside = tree / "Зовні";
	ASSERT_TRUE(WriteText(outside / "secret.png"));
	ASSERT_TRUE(WriteText(outside / "Deep" / "deeper.png"));
	ASSERT_TRUE(WriteText(content / "Real" / "a.png"));
	const std::filesystem::path link = content / FStringView("Посилання").ToPath();
	if (!test::discovery::MakeJunction(link, outside))
	{
		GTEST_SKIP() << "cannot make a junction here";
	}
	// One into its own ancestor too: a cycle, were it followed.
	ASSERT_TRUE(test::discovery::MakeJunction(content / "Real" / "Loop", content));

	bool bSeen = false;
	ASSERT_TRUE(platform::EnumerateDirectory(content, [&bSeen] (const platform::SDirectoryEntry& Entry)
	{
		if (Entry.Name == "Посилання")
		{
			bSeen = true;
			EXPECT_TRUE(Entry.bReparsePoint);
			EXPECT_TRUE(Entry.bNameSurrogate);
		}
		else
		{
			EXPECT_FALSE(Entry.bNameSurrogate);
		}
	}));
	EXPECT_TRUE(bSeen);

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
	EXPECT_EQ(FolderPaths(batch), (std::vector<FString> { "/Project", "/Project/Real" }));
	EXPECT_EQ(FilePaths(batch), std::vector<FString> { "/Project/Real/a.png" });
	EXPECT_EQ(batch.SkippedLinks, (std::vector<FString> { "/Project/Посилання", "/Project/Real/Loop" }));   // folder order
	ASSERT_EQ(batch.Diagnostics.size(), 2u) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.Diagnostics[0].Severity, ESeverity::Info);
	EXPECT_EQ(batch.Diagnostics[0].Subject, "/Project/Посилання");
	EXPECT_EQ(batch.Diagnostics[1].Subject, "/Project/Real/Loop");

	const SScanBatch rooted = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project/Посилання", membership, nullptr);
	EXPECT_TRUE(rooted.Folders.empty());
	EXPECT_TRUE(rooted.Files.empty());
	EXPECT_EQ(rooted.SkippedLinks, std::vector<FString> { "/Project/Посилання" });

	test::discovery::RemoveJunction(link);
	test::discovery::RemoveJunction(content / "Real" / "Loop");
	EXPECT_TRUE(platform::Stat(outside / "secret.png").has_value());   // nothing behind the links was touched
}


// ---------------- Portable names ----------------

// Rock.png and rock.png in a case-sensitive mount are two files, both scanned under their own spelling;
// PATH001 names each, the rule file's two spellings included; known files match byte-exact.
TEST(MountScannerTest, ACaseSensitiveMountScansBothSpellingsAndReportsPath001)
{
	CDiscoveryTree tree("Case-sensitive");
	MakeDirectories(tree.ProjectContent());
	if (!test::discovery::MakeCaseSensitive(tree.ProjectContent()))
	{
		GTEST_SKIP() << "cannot make a directory case-sensitive here";
	}
	const CMountTable table = ResolveTree(tree);
	const SMount& project = *table.Find("Project");
	ASSERT_TRUE(project.Volume.bCaseSensitive);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "Rock.png", "upper"));
	ASSERT_TRUE(WriteText(content / "rock.png", "lower!"));
	ASSERT_TRUE(WriteText(content / "Textures" / "a.png"));
	ASSERT_TRUE(WriteText(content / "textures" / "b.png"));
	ASSERT_TRUE(WriteText(content / "Assets.yml", "rules"));
	ASSERT_TRUE(WriteText(content / "assets.yml", "other rules"));

	CRecordingMembership membership;
	const auto pool = MakePool(2u);
	const SScanBatch first = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, nullptr);
	EXPECT_EQ(FolderPaths(first), (std::vector<FString> { "/Project", "/Project/Textures", "/Project/textures" }));
	EXPECT_EQ(FilePaths(first), (std::vector<FString> { "/Project/Rock.png", "/Project/rock.png", "/Project/Textures/a.png", "/Project/textures/b.png" }));
	EXPECT_GE(IndexOf(membership.GetEvents(), "enter /Project Assets.yml"), 0);   // the reserved spelling wins
	const std::vector<const SDiagnostic*> collisions = FindDiagnostics(first, Path001);
	ASSERT_EQ(collisions.size(), 6u) << DumpBatch(first).CStr();
	// The rule file's two spellings collide too, although neither is a file of the batch.
	EXPECT_EQ(collisions[0]->Subject, "/Project/Assets.yml");
	EXPECT_EQ(collisions[1]->Subject, "/Project/assets.yml");
	EXPECT_EQ(collisions[2]->Subject, "/Project/Rock.png");
	EXPECT_EQ(collisions[3]->Subject, "/Project/rock.png");
	EXPECT_EQ(collisions[4]->Subject, "/Project/Textures");
	EXPECT_EQ(collisions[5]->Subject, "/Project/textures");
	EXPECT_EQ(collisions[2]->Severity, ESeverity::Warning);
	EXPECT_TRUE(collisions[2]->Message.View().Contains(FStringView("'rock.png'")));

	CKnownMap known;
	const SScanFile& upper = *FindFile(first, "/Project/Rock.png");
	known.Files["/Project/Rock.png"] = SKnownFile { upper.Size, upper.MTime, upper.Identity };
	const SScanBatch second = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, &known);
	EXPECT_EQ(FindFile(second, "/Project/Rock.png")->State, EScanFileState::Unchanged);
	EXPECT_EQ(FindFile(second, "/Project/rock.png")->State, EScanFileState::New);
}

// Plain NTFS keeps U+212A KELVIN SIGN and 'k' apart (its upcase table maps neither to the other); the
// portable fold merges them, so the pair is PATH001 everywhere - also for the root of a subtree scan.
TEST(MountScannerTest, KelvinSignAndKAreAPath001PairOnPlainNtfs)
{
	CDiscoveryTree tree("Kelvin");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	const FStringView kelvin = "\xE2\x84\xAA" "elvin";
	ASSERT_TRUE(WriteText(content / FString::Format("{}.png", kelvin).ToPath()));
	ASSERT_TRUE(WriteText(content / "kelvin.png"));
	ASSERT_TRUE(WriteText(content / kelvin.ToPath() / "a.png"));
	ASSERT_TRUE(WriteText(content / "kelvin" / "b.png"));
	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
	if (FilePaths(batch).size() != 4u)
	{
		GTEST_SKIP() << "this volume treats the two names as one";
	}
	EXPECT_EQ(FindDiagnostics(batch, Path001).size(), 4u) << DumpBatch(batch).CStr();

	const SScanBatch subtree = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project/kelvin", membership, nullptr);
	const std::vector<const SDiagnostic*> rootCollision = FindDiagnostics(subtree, Path001);
	ASSERT_EQ(rootCollision.size(), 1u) << DumpBatch(subtree).CStr();
	EXPECT_EQ(rootCollision[0]->Subject, "/Project/kelvin");
	EXPECT_EQ(FilePaths(subtree), std::vector<FString> { "/Project/kelvin/b.png" });
}

TEST(MountScannerTest, NonPortableNamesAreRecordedUnderTheirSpellingAndReportedAsPath002)
{
	CDiscoveryTree tree("Path002");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "fine.png"));
	const std::filesystem::path lone = content / FStringView("Lone\xED\xB0\x80.png").ToPath();   // U+DC00 alone
	if (!test::discovery::WriteVerbatimFile(lone))
	{
		GTEST_SKIP() << "this file system refuses a name with a lone surrogate";
	}
	ASSERT_TRUE(test::discovery::WriteVerbatimFile(content / "aux.txt"));
	ASSERT_TRUE(test::discovery::WriteVerbatimFile(content / "Trailing."));
	ASSERT_TRUE(test::discovery::WriteVerbatimFile(content / "Space "));

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
	ASSERT_EQ(batch.Files.size(), 5u) << DumpBatch(batch).CStr();
	const FString loneName = FString::FromWide(lone.filename().native());
	EXPECT_FALSE(loneName.View().IsValidUtf8());
	EXPECT_NE(FindFile(batch, FString::Format("/Project/{}", loneName)), nullptr);   // the on-disk spelling, WTF-8

	const std::vector<const SDiagnostic*> names = FindDiagnostics(batch, Path002);
	ASSERT_EQ(names.size(), 4u) << DumpBatch(batch).CStr();
	// The Subject is the on-disk spelling, the key of the file's FILES row (the registry attaches the problem to the
	// record through it); the Message shows the name as text, U+FFFD for the surrogate.
	std::vector<FString> subjects;
	for (const SDiagnostic* diagnostic : names)
	{
		subjects.push_back(diagnostic->Subject);
		EXPECT_TRUE(diagnostic->Message.View().IsValidUtf8());
		EXPECT_EQ(diagnostic->Severity, ESeverity::Warning);
	}
	std::sort(subjects.begin(), subjects.end());
	EXPECT_EQ(subjects, (std::vector<FString> { FString::Format("/Project/{}", loneName), "/Project/Space ", "/Project/Trailing.", "/Project/aux.txt" }));
	const auto lonePath002 = std::find_if(names.begin(), names.end(), [&] (const SDiagnostic* D) { return D->Subject == FString::Format("/Project/{}", loneName); });
	ASSERT_NE(lonePath002, names.end());
	EXPECT_TRUE((*lonePath002)->Message.View().Contains(FStringView("Lone\xEF\xBF\xBD.png"))) << (*lonePath002)->Message.CStr();
	EXPECT_EQ(ToDisplayText((*lonePath002)->Subject), "/Project/Lone\xEF\xBF\xBD.png");
}

// Win32's path normalization drops a trailing dot or space and may read a device name as the device, so
// listing "Trailing." would list "Trailing": such a folder is failed (consumers keep what they knew), never
// listed, and its PATH002 stands; a subtree root below one fails too. Beside a "Trailing" with other files.
TEST(MountScannerTest, FoldersWhoseNamesWin32RewritesAreFailedNotListedAsAnotherFolder)
{
	CDiscoveryTree tree("Rewritten folders");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	ASSERT_TRUE(WriteText(content / "Trailing" / "inner.png"));
	for (const FStringView name : { FStringView("Trailing."), FStringView("Space "), FStringView("aux") })
	{
		ASSERT_TRUE(test::discovery::MakeVerbatimDirectory(content / name.ToPath()));
		ASSERT_TRUE(test::discovery::WriteVerbatimFile(content / name.ToPath() / "other.png"));
	}
	ASSERT_TRUE(test::discovery::MakeVerbatimDirectory(content / "Trailing." / "Sub"));
	ASSERT_TRUE(test::discovery::WriteVerbatimFile(content / "Trailing." / "Sub" / "deep.png"));

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SMount& project = *table.Find("Project");
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, nullptr);
	EXPECT_EQ(FolderPaths(batch), (std::vector<FString> { "/Project", "/Project/aux", "/Project/Space ", "/Project/Trailing", "/Project/Trailing." }));
	EXPECT_EQ(FilePaths(batch), std::vector<FString> { "/Project/Trailing/inner.png" }) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.FailedFolders, (std::vector<FString> { "/Project/aux", "/Project/Space ", "/Project/Trailing." }));
	EXPECT_EQ(FindDiagnostics(batch, Path002).size(), 3u) << DumpBatch(batch).CStr();
	EXPECT_EQ(batch.FoldersListed, 2u);

	// As a subtree root, and below one.
	const SScanBatch rooted = CMountScanner::ScanBlocking(*pool, project, "/Project/Trailing.", membership, nullptr);
	EXPECT_EQ(FolderPaths(rooted), std::vector<FString> { "/Project/Trailing." });
	EXPECT_TRUE(rooted.Files.empty());
	EXPECT_EQ(rooted.FailedFolders, std::vector<FString> { "/Project/Trailing." });
	const SScanBatch below = CMountScanner::ScanBlocking(*pool, project, "/Project/Trailing./Sub", membership, nullptr);
	EXPECT_TRUE(below.Folders.empty());
	EXPECT_TRUE(below.Files.empty());
	EXPECT_EQ(below.FailedFolders, std::vector<FString> { "/Project/Trailing./Sub" });
	ASSERT_EQ(below.Diagnostics.size(), 1u) << DumpBatch(below).CStr();
	EXPECT_EQ(below.Diagnostics[0].Severity, ESeverity::Warning);
}

TEST(MountScannerTest, LongRelativePathsAreReportedAsPath003)
{
	CDiscoveryTree tree("Path003");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	// 60 Cyrillic letters: 120 UTF-8 bytes, 60 UTF-16 units (the Windows path stays short).
	FString longName;
	for (uint32 i = 0u; i < 60u; ++i)
	{
		longName += "ж";
	}
	const FString folder = longName + "1";
	const FString file = FString::Format("{}2.png", longName);        // "<folder>/<file>": 121 + 1 + 125 bytes
	const FString shortFile = "s.png";
	ASSERT_TRUE(WriteText(content / folder.ToPath() / file.ToPath()));
	ASSERT_TRUE(WriteText(content / folder.ToPath() / shortFile.ToPath()));   // 121 + 1 + 5 = 127: fine

	const auto pool = MakePool(2u);
	const CDefaultScanMembership membership;
	const SScanBatch batch = CMountScanner::ScanBlocking(*pool, *table.Find("Project"), "/Project", membership, nullptr);
	const std::vector<const SDiagnostic*> longPaths = FindDiagnostics(batch, Path003);
	ASSERT_EQ(longPaths.size(), 1u) << DumpBatch(batch).CStr();
	EXPECT_EQ(longPaths[0]->Subject, FString::Format("/Project/{}/{}", folder, file));
	EXPECT_TRUE(longPaths[0]->Message.View().Contains(FStringView("247")));
}

TEST(MountScannerTest, NonPortableNameReasons)
{
	for (const FStringView good : { FStringView("Rock.png"), FStringView("Камінь 漢字 😀.png"), FStringView("COM0"), FStringView("COM10.txt"),
		FStringView("console.txt"), FStringView("nul_file"), FStringView("a.b.c"), FStringView("LPT"), FStringView(".hidden") })
	{
		EXPECT_TRUE(GetNonPortableNameReason(good).IsEmpty()) << good.GetData();
	}
	for (const FStringView bad : { FStringView("a<b"), FStringView("a>b"), FStringView("a:b"), FStringView("a\"b"), FStringView("a/b"),
		FStringView("a\\b"), FStringView("a|b"), FStringView("a?b"), FStringView("a*b"), FStringView("tab\there"), FStringView("Trailing."),
		FStringView("Space "), FStringView("CON"), FStringView("con.txt"), FStringView("Aux.tar.gz"), FStringView("nul"), FStringView("PRN.x"),
		FStringView("com1"), FStringView("COM9.png"), FStringView("lpt5"), FStringView("\xED\xA0\x80"), FStringView("\x7F\x01") })
	{
		EXPECT_FALSE(GetNonPortableNameReason(bad).IsEmpty()) << FString::FromUtf8Lossy(bad).CStr();
	}
}


// ---------------- Built-in excludes ----------------

TEST(MountScannerTest, BuiltinExcludesFollowTheSpecList)
{
	for (const FStringView excluded : { FStringView(".git"), FStringView(".frt-fence-3"), FStringView(".Rock.png.123-4.tmp"), FStringView("Rock.png~"),
		FStringView("Rock.kra~"), FStringView("~$Doc.docx"), FStringView("a.tmp"), FStringView("A.TMP"), FStringView("a.orig"), FStringView("Scene.blend1"),
		FStringView("Scene.blend12"), FStringView("Scene.BLEND3"), FStringView("Scene.blend@"), FStringView("Paint-autosave.kra"), FStringView("Thumbs.db"),
		FStringView("THUMBS.DB"), FStringView("desktop.ini"), FStringView("Rock.png.meta"), FStringView("Assets.yml"), FStringView("assets.YML") })
	{
		EXPECT_TRUE(IsBuiltinExcludedName(excluded, false)) << excluded.GetData();
	}
	for (const FStringView kept : { FStringView("Rock.png"), FStringView("Scene.blend"), FStringView("Scene.blend123"), FStringView("Scene.blendx"),
		FStringView("Paint.kra"), FStringView("meta"), FStringView("a.metadata"), FStringView("Assets.yml.bak"), FStringView("My Assets.yml"),
		FStringView("tmp"), FStringView("a~b"), FStringView("$~a"), FStringView("autosave.kra"), FStringView("Камінь.png") })
	{
		EXPECT_FALSE(IsBuiltinExcludedName(kept, false)) << kept.GetData();
	}
	// Only dot-names exclude a directory.
	EXPECT_TRUE(IsBuiltinExcludedName(".git", true));
	EXPECT_TRUE(IsBuiltinExcludedName(".vs", true));
	EXPECT_FALSE(IsBuiltinExcludedName("Old.tmp", true));
	EXPECT_FALSE(IsBuiltinExcludedName("Thumbs.db", true));
	EXPECT_FALSE(IsBuiltinExcludedName("Assets.yml", true));
	EXPECT_FALSE(IsBuiltinExcludedName("", false));
	EXPECT_TRUE(IsRuleFileName("ASSETS.yml"));
	EXPECT_FALSE(IsRuleFileName("Assets.yaml"));
}


// ---------------- The lazy hash ----------------

TEST(MountScannerTest, HashFileStableHashesTheWholeFileOrNothing)
{
	CDiscoveryTree tree("Hash");
	const std::filesystem::path file = tree / "Файл 文件.bin";
	FString bytes;
	for (uint32 i = 0u; i < 3000u; ++i)
	{
		bytes.AppendFormat("{} ", i);
	}
	ASSERT_TRUE(WriteText(file, bytes));
	hash::CContentHasher hasher;
	hasher.Update(bytes.View().AsBytes());
	EXPECT_EQ(HashFileStable(file), std::optional(hasher.Finalize()));
	EXPECT_FALSE(HashFileStable(tree / "Немає.bin").has_value());
	EXPECT_FALSE(HashFileStable(tree.Root).has_value());
}


// ---------------- Budget ----------------

// §5.11: a warm start with zero changes stats some 20k files in at most 60 ms. Optimized builds only
// (Debug scans 2k files and prints the numbers); twice the budget unless FRT_PERF_STRICT=1, as the suite runs it on a busy
// machine. The best of three warm scans, every file Unchanged against the first scan's records.
TEST(MountScannerTest, AWarmScanOf20kFilesStaysWithinItsBudget)
{
	constexpr bool bOptimized = !FRT_BUILD_DEBUG;
	const bool bStrict = platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1";
	const double slack = bStrict ? 1.0 : 2.0;
	const uint32 FolderCount = bOptimized ? 200u : 20u;
	constexpr uint32 FilesPerFolder = 100u;

	CDiscoveryTree tree("Budget");
	const CMountTable table = ResolveTree(tree);
	const std::filesystem::path content = tree.ProjectContent();
	for (uint32 folder = 0u; folder < FolderCount; ++folder)
	{
		const std::filesystem::path directory = content / FString::Format("Тека {:02}", folder / 20u).ToPath() / FString::Format("Folder {}", folder).ToPath();
		for (uint32 file = 0u; file < FilesPerFolder; ++file)
		{
			ASSERT_TRUE(WriteText(directory / FString::Format("Файл_{}.png", file).ToPath()));
		}
	}

	const auto pool = MakePool(jobs::CWorkerPool::GetDefaultThreadCount());
	const CDefaultScanMembership membership;
	const SMount& project = *table.Find("Project");
	const SScanBatch cold = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, nullptr);
	ASSERT_EQ(cold.Files.size(), FolderCount * FilesPerFolder);
	const CKnownMap known = KnowEverything(cold);

	double best = 1e9;
	for (uint32 attempt = 0u; attempt < 3u; ++attempt)
	{
		const uint64 start = platform::ReadTimestamp();
		const SScanBatch warm = CMountScanner::ScanBlocking(*pool, project, "/Project", membership, &known);
		const double ms = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		best = (std::min)(best, ms);
		ASSERT_EQ(warm.Files.size(), FolderCount * FilesPerFolder);
		const bool bAllUnchanged = std::all_of(warm.Files.begin(), warm.Files.end(), [] (const SScanFile& File) { return File.State == EScanFileState::Unchanged; });
		EXPECT_TRUE(bAllUnchanged);
	}
	std::printf("[ budget ] warm scan of %u files in %u folders, %u threads: %.2f ms (budget 60 ms)\n", FolderCount * FilesPerFolder,
		FolderCount + (FolderCount + 19u) / 20u + 1u, pool->GetThreadCount(), best);
	RecordProperty("Files", static_cast<int>(FolderCount * FilesPerFolder));
	RecordProperty("WarmScanMilliseconds", FString::Format("{:.2f}", best).CStr());
	if (bOptimized)
	{
		EXPECT_LE(best, 60.0 * slack) << (bStrict ? "the budget" : "twice the budget");
	}
}
