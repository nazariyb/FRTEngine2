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
 *	- IKnownFiles answers "what did we know about this file": size, mtime and identity, for the Unchanged test.
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
class IKnownFiles   // called concurrently from workers
{
public:
	virtual ~IKnownFiles () = default;
	virtual std::optional<SKnownFile> Find (FStringView FileMountPath) const = 0;   // F3: over an acquired view; tests: a map
};
struct SScanScope { virtual ~SScanScope () = default; };          // the oracle's immutable per-folder state (its rule chain)
struct SFileVerdict { EFileClass Class = EFileClass::Asset; bool bQuiet = false; };  // Asset (claimed), Raw or Unclaimed
class IScanMembership     // every member is called concurrently from workers; implementations are immutable or locked
{
public:
	virtual ~IScanMembership () = default;
	// The scope of a scan root (a mount root or a subtree root); the oracle reads the ancestors' rule files itself.
	virtual std::shared_ptr<const SScanScope> OpenScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const = 0;
	// For every folder below the root, before its entries are classified. RuleFile: its Assets.yml when the listing
	// holds one, else nullptr. Returns the child scope (the parent's when nothing changes) and that file's RulesHash
	// (0 = none); a malformed file keeps the parent's scope and adds RULE001.
	virtual std::shared_ptr<const SScanScope> EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		const std::filesystem::path* RuleFile, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const = 0;
	virtual bool ShouldDescend (const SScanScope& Scope, FStringView FolderMountPath) const = 0;
	// nullopt: excluded (Scan.Exclude or a built-in exclude): not in the batch, never in FILES.
	virtual std::optional<SFileVerdict> Classify (const SScanScope& Scope, FStringView FileMountPath) const = 0;
};
enum class EScanFileState : uint8 { New, Changed, IdentityChanged, Unchanged };   // Changed: size or mtime differ (inequality)
struct SScanFolder { FString MountPath; uint32 Parent = ~0u; bool bHasRuleFile = false; uint64 RulesHash = 0; };  // Parent: index; ~0u = root
struct SScanFile { uint32 Folder = 0; FString Name; uint64 Size = 0; int64 MTime = 0; platform::SFileIdentity Identity;
	SFileVerdict Verdict; EScanFileState State = EScanFileState::New; };
/**
 * One scan's result. The oracle never emits RULE002: whether a file stays Unclaimed is decided after Analyze,
 * once companions are known. Removals (known files at or below the root missing from the batch, outside
 * FailedFolders) are computed by the consumer, not the scanner.
 */
struct SScanBatch
{
	FString RootMountPath;                     // Folders[0] is this folder (a mount root or the subtree root)
	std::vector<SScanFolder> Folders;          // pre-order DFS; siblings by SStringLessIgnoreCase, then bytes
	std::vector<SScanFile> Files;              // every file Classify kept, Unchanged included; ordered like the folders, then by name
	std::vector<FString> FailedFolders;        // listing failed: consumers keep everything they know at or below them
	std::vector<FString> SkippedLinks;         // name-surrogate reparse points (symlinks, junctions): never descended
	std::vector<SDiagnostic> Diagnostics;      // PATH001-003, the oracle's RULE001, unlistable folders
	uint32 FoldersListed = 0; double Milliseconds = 0.0;
};
}
