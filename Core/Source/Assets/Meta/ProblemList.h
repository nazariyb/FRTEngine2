#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/LifecycleTypes.h"
#include "Assets/Meta/Validation.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


/**
 * The data of the editor's Asset problems panel (design §12.8, M4's H4; edits [14], [18], [33]), in Core so that Core-Test
 * checks what the panel shows and offers, and the editor only draws it.
 *
 * One list: RunValidation's (Validation.h), the very function `validate` prints, over every mount of the registry (what
 * `validate` and `validate --engine` list together), on a worker, again only when the revision moved and at most once a
 * second. Each problem gets its asset (the file it is about: a meta's source, a sub-entry's file) and that asset's type, so
 * the list can be filtered by severity, code, type and folder and grouped by asset.
 *
 * Fix-its are not stored on a diagnostic (edit [33]): GetProblemFixIts asks CAssetRepairs::GetFixIts, the lifecycle's own
 * actions, so one action type runs from automatic repair to button (ApplyFix), and adds what M4 can only show:
 *
 *   META001   create its meta; on /Engine in a process that does not own /Engine, the `FRT.bat assets fix --engine` text
 *   META003   give the copy a new GUID (ApplyFix re-GUIDs every copy of that GUID as one set)
 *   META004   keep A, alias B (one per GUID the broken meta held)
 *   META006   rename the meta to its source's case
 *   META008   a lost meta: restore the verified copy, or create it again with its GUID, and restore from the VCS (its
 *             command, shown and copied, never run: FRT never changes the work tree through the VCS beyond §12.9's
 *             operations); a meta swap's old source: relink the orphan that matches, or give it a new GUID (no VCS
 *             restore: it would bring back a second meta with a GUID another meta holds)
 *   META009   relink the orphan meta to the file it matches (a suggestion, never automatic)
 *   VCS002    retry the repair the VCS refused
 *   VCS003    the provider's add command, shown and copied (FRT never stages); absent under None
 *   VCS004    a message only: `git lfs pull` fetches the content
 *
 * An action is disabled, with the reason, in a mount this process does not write: a read-only editor, a guest, `/Engine`
 * outside engine-owner mode. In a live host ApplyFix only queues (AssetRepairs.h), so its outcome comes from the lifecycle
 * (GetFixResult) and a later list: SPendingFix follows it. PATH renames, reference relinks, hints and aliases are M11's.
 */
namespace frt::assets
{
class CAssetRegistry;
class CAssetRepairs;
class CMountTable;
class CRegistryView;

// The file Subject is about: a meta's source, a sub-entry's file ("<file>#<label>"), else Subject itself (a file, a folder,
// a mount root).
[[nodiscard]] FRT_CORE_API FStringView GetProblemAsset (FStringView Subject) noexcept;

struct SProblemEntry
{
	SDiagnostic Problem;
	FString Asset;          // GetProblemAsset(Problem.Subject)
	FString Type;           // the $Type of Asset's record; empty when Asset has none (an orphan meta, a folder, a root)
	bool bFolder = false;   // Asset is a folder or a mount root of the view
};

/**
 * The panel's options: every mount of Registry, the rule files' Validation: severities, and each mount's provider as the
 * lifecycle uses it (none without a lifecycle, so no VCS is asked). The smoke test validates with the same options and
 * compares. Owner thread; the options point into Registry, which must outlive their use.
 */
[[nodiscard]] FRT_CORE_API SValidateOptions MakeProblemListOptions (CAssetRegistry& Registry);

// RunValidation over View with Options, each problem with its asset and type, in RunValidation's order. Reads files and asks
// the providers: on a worker.
[[nodiscard]] FRT_CORE_API std::vector<SProblemEntry> ListAssetProblems (const CRegistryView& View, const SValidateOptions& Options);

struct SProblemFilter
{
	bool bErrors = true;
	bool bWarnings = true;
	bool bInfos = true;
	FString Code;     // a code or the start of one ("META", "meta00", "VCS003"), in any case; empty: any code, none included
	FString Type;     // a $Type ("Texture"), under the portable fold; empty: any
	FString Folder;   // a folder's mount path: the problems of assets at or below it, under the portable fold; empty: all

	[[nodiscard]] FRT_CORE_API bool Matches (const SProblemEntry& Entry) const;
};

// One asset's entries that match the filter, in the list's order.
struct SProblemGroup
{
	FString Asset;
	FString Type;
	ESeverity Worst = ESeverity::Info;
	std::vector<uint32> Entries;   // indices into the list
};

// The matching entries grouped by asset, the groups ordered by asset (portable fold, then bytes).
[[nodiscard]] FRT_CORE_API std::vector<SProblemGroup> GroupProblems (std::span<const SProblemEntry> Entries, const SProblemFilter& Filter);

// The distinct types of Entries, sorted (the type filter's choices).
[[nodiscard]] FRT_CORE_API std::vector<FString> GetProblemTypes (std::span<const SProblemEntry> Entries);

// "Go to": the Assets panel's query that lists Entry's asset alone (`$Path="<asset>"`), or for a folder or a mount root what
// lies in it (`dir:"<folder>"`), or for a file without a record (an orphan meta) its folder (`dir="<folder>"`).
[[nodiscard]] FRT_CORE_API FString MakeGoToQuery (const SProblemEntry& Entry);

// "Open file at line": the file Problem names (File, relative to the owner root of its subject's mount), else its asset on
// disk; nullopt when neither is a path of a mount.
[[nodiscard]] FRT_CORE_API std::optional<std::filesystem::path> GetProblemFilePath (const SDiagnostic& Problem, const CMountTable& Mounts);

enum class EProblemFixKind : uint8
{
	Apply,   // a lifecycle action: CAssetRepairs::ApplyFix(Action)
	Copy,    // a command for a person to run elsewhere: Text, shown and copied
	Note,    // a message only: Text
};

struct SProblemFixIt
{
	EProblemFixKind Kind = EProblemFixKind::Note;
	FString Label;            // the button's text
	FString Text;             // Copy: the command; Note: the message; Apply: what it does
	SLifecycleAction Action;  // Apply only
	FString Disabled;         // Apply: why this process cannot run it (empty: it can)
};

struct SProblemFixContext
{
	const CMountTable* Mounts = nullptr;   // required
	CAssetRepairs* Repairs = nullptr;      // null: no lifecycle runs here, so nothing to apply and no provider to ask
	const CRegistryView* View = nullptr;   // the live view: tells a lost meta's "create it again" from a new identity
};

// What Problem offers here (see the header comment). Owner thread: it asks Repairs.
[[nodiscard]] FRT_CORE_API std::vector<SProblemFixIt> GetProblemFixIts (const SDiagnostic& Problem, const SProblemFixContext& Context);

/**
 * A fix-it applied in a live host: ApplyFix queued it as Ticket, and the panel shows its outcome. The lifecycle says what
 * became of it (CAssetRepairs::GetFixResult): queued however long it waits for a quiet registry, refused (the VCS or the
 * writer; a VCS002 follows) or not applied, at once; once applied, Fixed from the first list made after that without the
 * problem (code and subject), NoChange from a list made FixOutcomeTimeoutMs after it that still has it.
 */
struct SPendingFix
{
	uint32 Code = 0u;
	FString Subject;
	FString Label;
	uint64 Ticket = 0u;   // SRepairReport::Ticket
	uint64 DoneMs = 0u;   // when the lifecycle was first seen to have applied it (0: not yet)
};

enum class EFixOutcome : uint8 { Pending, Fixed, Refused, NotApplied, NoChange };

inline constexpr uint64 FixOutcomeTimeoutMs = 30000u;

[[nodiscard]] FRT_CORE_API SPendingFix MakePendingFix (const SDiagnostic& Problem, FStringView Label, uint64 Ticket);
// Result and ResultMessage: GetFixResult(Fix.Ticket) now. Entries: the latest list, made from a view acquired at ListMs (the
// lifecycle's clock, GetLifecycleNowMs). OutMessage: a refusal's message, or why it was not applied. Sets Fix.DoneMs.
[[nodiscard]] FRT_CORE_API EFixOutcome ResolvePendingFix (SPendingFix& Fix, EFixResult Result, FStringView ResultMessage,
	std::span<const SProblemEntry> Entries, uint64 ListMs, uint64 NowMs, FString* OutMessage = nullptr);
}
