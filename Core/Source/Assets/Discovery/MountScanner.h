#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Discovery/ScanTypes.h"
#include "Containers/String.h"
#include "Functional/UniqueFunction.h"


namespace frt::jobs
{
class CWorkerPool;
}


/**
 * The stat scan (design §5.4 step 4): a mount, or one subtree of it, listed top-down and in parallel
 * across directories with platform::EnumerateDirectory, which gives name, size, times and identity per
 * entry in one call. Nothing is opened or hashed: a file is compared with what IKnownFiles knew (size and
 * mtime equal: Unchanged; only the identity differs: IdentityChanged), so a warm start with no changes
 * reads directory listings and the few rule files and nothing else. The membership oracle (ScanTypes.h)
 * decides per folder and file, by the protocol written there: the root's scope from OpenScope,
 * ShouldDescend before a folder is listed (a rule file inside an excluded folder is never read),
 * EnterFolder with the folder's Assets.yml before its entries are classified.
 *
 * Work. One task per directory on the caller's jobs::CWorkerPool, submitted from the worker that listed
 * its parent; an atomic count of outstanding directories tells the last task to finish that the tree is
 * done, and it assembles the batch on its worker. The batch is deterministic whatever the thread count or
 * scheduling (the order ScanTypes.h states): each directory's result depends only on its own listing, and
 * the assembly walks the tree in sorted order.
 *
 * What a batch holds besides the tree:
 *	- Links. Name-surrogate reparse points (symbolic links, junctions; SDirectoryEntry::bNameSurrogate) are
 *	  never followed, files or directories: they go to SkippedLinks with an info diagnostic and nothing at
 *	  or below them is recorded. Every other reparse point (a OneDrive placeholder, a deduplicated file) is
 *	  an ordinary entry, stat-scanned from the listing like any other, so a cloud placeholder is never
 *	  opened or downloaded by a scan.
 *	- Rule files. A folder's Assets.yml (RuleFileName, under the portable fold; the byte-exact spelling
 *	  wins when a case-sensitive folder holds two) is handed to EnterFolder and never appears in Files.
 *	- Portable names (§3.1, §13.4), warnings until M4, over the entries the batch records (files Classify
 *	  kept and folders it entered): PATH001 for every member of a group of siblings equal under the
 *	  portable fold (Rock.png and rock.png in a case-sensitive folder; U+212A KELVIN SIGN and 'k' on any
 *	  NTFS folder) - a sibling check, so a subtree scan reports its part completely; PATH002 for a name
 *	  that is illegal on a supported platform or not valid UTF-8 (GetNonPortableNameReason); PATH003 for
 *	  a mount-relative path over PortablePathMaxBytes. A subtree root is checked too, against its siblings
 *	  in its parent's listing. Entries keep their on-disk spelling everywhere; a diagnostic's Subject is
 *	  the mount path with every ill-formed sequence shown as U+FFFD (FString::FromUtf8Lossy).
 *	- Failures. A folder whose listing fails is in Folders (it exists: its parent listed it) with no files
 *	  or subfolders and its parent's scope, and in FailedFolders, with a warning (code 0); consumers keep
 *	  what they knew at or below it. A root that does not exist gives a batch with no folders and nothing
 *	  failed: nothing is there.
 *	- Removals are not here: known files at or below the root missing from the batch, outside
 *	  FailedFolders, are the consumer's to compute (F3), byte-exact like IKnownFiles::Find.
 *
 * Watcher fit (M2's FileWatcherService reports directory changes only as Rescan, Moved and Removed with
 * bDirectory): Rescan scans that subtree; Moved scans the new subtree (the old path may lie outside the
 * root, and ids are path-derived in M3, so the old subtree is removed); Removed drops everything at or
 * below without a scan. A directory that appears arrives as one Added per file; an empty new folder is
 * not reported and reaches FOLDERS at the next scan of a subtree that holds it.
 */
namespace frt::assets
{
// §3.1's portable limit on a mount-relative path, in UTF-8 bytes (PATH003 above it).
inline constexpr uint32 PortablePathMaxBytes = 180u;

/**
 * Why Name (one path segment as listed) is not portable, or an empty text when it is: not valid UTF-8 (a
 * lone UTF-16 surrogate in WTF-8), one of < > : " / \ | ? *, a control character (U+0000-U+001F), a
 * trailing dot or space, or a reserved device name - CON PRN AUX NUL COM1-COM9 LPT1-LPT9, in any case and
 * with any extension ("con.txt", "Aux.tar.gz"). The PATH002 rule.
 */
[[nodiscard]] FRT_CORE_API FStringView GetNonPortableNameReason (FStringView Name) noexcept;

/**
 * The membership of a scan without rules: the built-in excludes (IsBuiltinExcludedName) and nothing else,
 * every other file an Asset. Rule files are reported but never parsed (RulesHash 0), and every folder has
 * the one empty scope. For tools and tests that need no rules engine; the rules engine's own oracle
 * replaces it in the registry. Immutable, so safe from every worker.
 */
class FRT_CORE_API CDefaultScanMembership final : public IScanMembership
{
public:
	CDefaultScanMembership ();

	std::shared_ptr<const SScanScope> OpenScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const override;
	std::shared_ptr<const SScanScope> EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		const std::filesystem::path* RuleFile, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const override;
	bool ShouldDescend (const SScanScope& Scope, FStringView FolderMountPath) const override;
	std::optional<SFileVerdict> Classify (const SScanScope& Scope, FStringView FileMountPath) const override;

private:
	std::shared_ptr<const SScanScope> Scope;
};

/**
 * Scans Mount at or below RootMountPath (its root mount path "/Project", or a folder in it, as spelled on
 * disk: the mount name byte-exact). Membership and Known (nullptr: every file is New) are called
 * concurrently from the pool's workers and must outlive the scan.
 */
class FRT_CORE_API CMountScanner
{
public:
	/**
	 * Starts the scan and returns at once. OnDone receives the batch as a completion of Pool, so on its
	 * owner thread, in a later PumpCompletions. An invalid RootMountPath (not well-formed, another mount's)
	 * still completes: a batch with the root in FailedFolders and an error diagnostic. If the pool is
	 * drained (CancelAllAndDrain) before the scan ends, OnDone is destroyed without running, possibly on a
	 * worker: capture nothing whose destruction needs the owner thread.
	 */
	static void ScanAsync (jobs::CWorkerPool& Pool, const SMount& Mount, FStringView RootMountPath, const IScanMembership& Membership,
		const IKnownFiles* Known, TUniqueFunction<void (SScanBatch&&)> OnDone);
	// ScanAsync, then Pool.PumpUntil it is done: for tools and tests, on the pool's owner thread (other
	// completions pumped meanwhile run too).
	[[nodiscard]] static SScanBatch ScanBlocking (jobs::CWorkerPool& Pool, const SMount& Mount, FStringView RootMountPath,
		const IScanMembership& Membership, const IKnownFiles* Known);
};
}
