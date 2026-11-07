#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "CoreTypes.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Platform/FileSystem.h"


/**
 * What the mount scanner (design §5.4 steps 1 and 4) takes and produces. The scanner lists directories and
 * stats nothing beyond the listing; it never opens or hashes a file. Two seams keep it independent of the
 * registry and of the rules engine:
 *	- IKnownFiles answers "what did we know about this file": size, mtime and identity, for the Unchanged test
 *	  (ClassifyKnown). The registry passes none and applies the same test to its view as it converts a batch.
 *	- IScanMembership is the oracle that decides, per folder and file, what the rules say (the rules engine
 *	  implements it; CDefaultScanMembership applies the built-in excludes only).
 *
 * Thread safety: the scanner calls both interfaces concurrently from worker threads, so implementations are
 * immutable or lock internally. Everything here is std containers and FString (never the engine's memory pool).
 *
 * Determinism: a batch is the same whatever the thread count or scheduling. Folders are in pre-order DFS from
 * the scan root, siblings ordered by SStringLessIgnoreCase with ties broken by byte order; files follow their
 * folder's order and, inside a folder, the same name order.
 */
namespace frt::assets
{
struct SKnownFile { uint64 Size = 0; int64 MTime = 0; platform::SFileIdentity Identity; };
/**
 * Find matches the mount path byte-exact, as spelled on disk - never by the FILES index's NTFS rule or the
 * portable fold. So after an offline case-only rename (Rock.png -> rock.png, e.g. by a git checkout) the
 * listed rock.png is New, and the consumer, which computes removals byte-exact as well, removes the
 * Rock.png row: the stored spelling, its provisional id and the dump follow the disk.
 */
class IKnownFiles   // called concurrently from workers
{
public:
	virtual ~IKnownFiles () = default;
	virtual std::optional<SKnownFile> Find (FStringView FileMountPath) const = 0;   // tools and tests: a map
};
struct SScanScope { virtual ~SScanScope () = default; };          // the oracle's immutable per-folder state (its rule chain)
struct SFileVerdict   // Asset (claimed), Raw or Unclaimed
{
	EFileClass Class = EFileClass::Asset; bool bQuiet = false;
	bool operator== (const SFileVerdict&) const = default;
};
/**
 * The scan protocol, the same for a mount scan and a subtree rescan (a watcher Rescan or Moved):
 *	1. Scope(above root) = OpenScope(root): every rule that applies strictly above the root - the engine
 *	   file, the mount's Config/Assets.yml, the rule files of the root's ancestor folders, which the oracle
 *	   reads itself. The root's own Assets.yml is not part of it.
 *	2. The root is then handled like every other folder, with Scope(above root) as its parent scope. For a
 *	   folder D whose parent scope is P (P = Scope(F) for a child D of folder F):
 *	   a. ShouldDescend(P, D) before D is listed; false: D is not listed, its rule file never read, nothing
 *	      at or below it is in the batch (an excluded root yields a batch with empty Folders and Files).
 *	   b. List D; Scope(D) = EnterFolder(P, D, D's Assets.yml or nullptr, ...), which fills the folder's
 *	      RulesHash - the root's included, so Folders[0] carries it like every other folder.
 *	   c. Classify(Scope(D), file) for each file of D; ShouldDescend(Scope(D), child) for each subfolder.
 * Scope(D) is kept in the batch (SScanFolder::Scope): the per-file settings (the rules engine's resolution
 * of an SEffectiveFileSettings) and the Types filter after Analyze take that scope, so a file's verdict and
 * its effective settings always come from one rules snapshot, even when an Assets.yml edit lands meanwhile.
 */
class IScanMembership     // every member is called concurrently from workers; implementations are immutable or locked
{
public:
	virtual ~IScanMembership () = default;
	// Step 1: the scope of everything strictly above FolderMountPath (a mount root or a subtree root).
	virtual std::shared_ptr<const SScanScope> OpenScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const = 0;
	// Step 2b, for every folder the scan lists, the root included, before its entries are classified. RuleFile: its
	// Assets.yml when the listing holds one, else nullptr. Returns the folder's scope (Parent when nothing changes)
	// and that file's RulesHash (0 = none). A malformed file keeps that folder's last good rules (Parent at a cold
	// start with none), sets OutRulesHash to the last good RulesHash (0 at a cold start), and adds RULE001.
	virtual std::shared_ptr<const SScanScope> EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		const std::filesystem::path* RuleFile, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const = 0;
	// Step 2a: Scope is the parent folder's (Scope(above root) for the root), so an excluded folder's rules are never read.
	virtual bool ShouldDescend (const SScanScope& Scope, FStringView FolderMountPath) const = 0;
	// nullopt: excluded (Scan.Exclude or a built-in exclude): not in the batch, never in FILES.
	virtual std::optional<SFileVerdict> Classify (const SScanScope& Scope, FStringView FileMountPath) const = 0;
};
enum class EScanFileState : uint8 { New, Changed, IdentityChanged, Unchanged };   // Changed: size or mtime differ (inequality)
/**
 * The Unchanged test (§5.4 step 4), the one implementation of it: a listed file against what is known of it (nullopt:
 * nothing, New). Changed when its size or mtime differs - inequality, never "newer than": tunnelling and CopyFile
 * restore older times; IdentityChanged when only its identity does (refreshed without a hash). The scanner applies it
 * through IKnownFiles, the registry against its view.
 */
[[nodiscard]] inline EScanFileState ClassifyKnown (const std::optional<SKnownFile>& Known, uint64 Size, int64 MTime,
	const platform::SFileIdentity& Identity) noexcept
{
	if (!Known.has_value())
	{
		return EScanFileState::New;
	}
	if (Known->Size != Size || Known->MTime != MTime)
	{
		return EScanFileState::Changed;
	}
	return Known->Identity == Identity ? EScanFileState::Unchanged : EScanFileState::IdentityChanged;
}
struct SScanFolder   // Parent: index; ~0u = root. Scope: what EnterFolder returned for it (immutable, shareable)
{
	FString MountPath; uint32 Parent = ~0u; bool bHasRuleFile = false; uint64 RulesHash = 0;
	std::shared_ptr<const SScanScope> Scope;
};
struct SScanFile { uint32 Folder = 0; FString Name; uint64 Size = 0; int64 MTime = 0; platform::SFileIdentity Identity;
	SFileVerdict Verdict; EScanFileState State = EScanFileState::New; };
/**
 * A `.meta` of the listing (design §3.2, §5.4 step 4). `*.meta` stays a built-in exclude, so no rule ever classifies a
 * meta: the scanner splits metas out of each listing before Classify, as it does with the folder's Assets.yml, and
 * records them here with the spelling the listing has. SourceFile pairs it with the file it describes, the meta's name
 * without ".meta": byte-exact first, else the first equal under the portable fold (a meta spelled otherwise, META006).
 * ~0u: no such file is in Files - an orphan, or a meta whose source the listing holds but Classify excluded
 * (bSourceExcluded: an excluded file that still has a meta, META007; a folder rule file counts as excluded). A name that
 * is a dot-name or nothing but ".meta" is no meta: the built-in excludes drop it unseen, as before.
 */
struct SScanMeta
{
	uint32 Folder = 0; FString Name; uint64 Size = 0; int64 MTime = 0; platform::SFileIdentity Identity;
	uint32 SourceFile = ~0u;          // index in SScanBatch::Files
	bool bSourceExcluded = false;
};
/**
 * One scan's result. The oracle never emits RULE002: whether a file stays Unclaimed is decided after Analyze,
 * once companions are known. Removals (known files at or below the root missing from the batch, outside
 * FailedFolders) are computed by the consumer, not the scanner.
 */
struct SScanBatch
{
	FString RootMountPath;                     // Folders[0] is this folder (a mount root or the subtree root); none when excluded
	std::vector<SScanFolder> Folders;          // pre-order DFS; siblings by SStringLessIgnoreCase, then bytes
	std::vector<SScanFile> Files;              // every file Classify kept, Unchanged included; ordered like the folders, then by name
	std::vector<SScanMeta> Metas;              // every meta listed; ordered like Files; the same with or without IKnownFiles
	std::vector<FString> FailedFolders;        // listing failed: consumers keep everything they know at or below them
	std::vector<FString> SkippedLinks;         // name-surrogate reparse points (symlinks, junctions): never descended
	std::vector<SDiagnostic> Diagnostics;      // the scan's own (problem source Scan): PATH001-003 (metas' names included),
	                                           // unlistable folders, skipped links
	std::vector<SDiagnostic> RuleDiagnostics;  // what the oracle's OpenScope and EnterFolder returned (source Rules): RULE001 and its
	                                           // code-0 warnings; the consumer routes by this member, never by code
	uint32 FoldersListed = 0; double Milliseconds = 0.0;
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EScanFileState,
	FRT_ENUM_ENTRY(frt::assets::EScanFileState, New),
	FRT_ENUM_ENTRY(frt::assets::EScanFileState, Changed),
	FRT_ENUM_ENTRY(frt::assets::EScanFileState, IdentityChanged),
	FRT_ENUM_ENTRY(frt::assets::EScanFileState, Unchanged));
