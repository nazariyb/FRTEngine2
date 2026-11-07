#pragma once

#include <optional>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Discovery/ScanTypes.h"
#include "Assets/Pipeline/RuleTypes.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Registry/RegistryView.h"
#include "Containers/String.h"


/**
 * What the registry service (Assets/AssetRegistry.h) reads back from a view to decide a file's work after a scan
 * (design §5.3, §5.4 step 5): whether its record must be analyzed again (no record, StaleTags, another AnalyzeStamp
 * or importer), only re-evaluated in memory (its rule outputs changed, its stamp did not), or nothing. Re-evaluation
 * must not touch the disk, so a record whose rule outputs changed is committed again from what the view holds -
 * tags, sub-entries, edges, companions - with the new rule outputs (MakeRecordUpsertFromView). Lives here, beside
 * the tables it reads (RegistryTables.h), so the service needs no table access of its own.
 *
 * Every function reads one immutable view and is safe on any thread (the service calls them on workers).
 */
namespace frt::assets
{
// A FILES row and, when it has one, its record.
struct SStoredFile
{
	uint32 FileRow = InvalidRow;
	EFileClass Class = EFileClass::Unclaimed;       // effective (Companion while a record names the file)
	EFileClass BaseClass = EFileClass::Unclaimed;   // as the last SFileUpsert gave it
	SKnownFile Stat;                                // what a scan's Unchanged test compares with (ClassifyKnown)
	EMetaState MetaState = EMetaState::PendingMeta; // the row's meta columns (RegistryDelta.h SFileMetaState)
	uint64 MetaHash = 0u;
	SRecordId Record;                              // invalid without a record
	SGuid Guid;                                     // the record's
	reflect::TypeId Type = reflect::InvalidTypeId;
	ERecordFlags Flags = ERecordFlags::None;        // the producer's flags (HasProblems left out)
	FStringView Importer;                           // views the view's importer table
	EImportStatus ImportStatus = EImportStatus::NotAnalyzed;
	uint64 AnalyzeStamp = 0u;
};

// The file of MountPath, matched byte-exact (what a scan compares against); nullopt when the view has none.
[[nodiscard]] FRT_CORE_API std::optional<SStoredFile> FindStoredFile (const CRegistryView& View, FStringView MountPath);

// Whether the record's stored rule outputs - cook rule, chunk, rule labels, authored labels (the meta's) and the
// TypeExcluded flag - differ from Settings and Flags.
[[nodiscard]] FRT_CORE_API bool RuleOutputsDiffer (const CRegistryView& View, SRecordId Record, const SEffectiveFileSettings& Settings,
	ERecordFlags Flags);

/**
 * The record again as an upsert - its type, importer, status, AnalyzeStamp, primary name, tags, sub-entries, edges
 * (as declared: by id or by path, with their field paths) and companions as the view holds them - with the rule
 * outputs of Settings (cook rule, chunk, rule labels, authored labels) and Flags as its flags. Committing it changes
 * nothing else; the caller sets another Guid when the meta's changed.
 */
[[nodiscard]] FRT_CORE_API SRecordUpsert MakeRecordUpsertFromView (const CRegistryView& View, SRecordId Record, const SEffectiveFileSettings& Settings,
	ERecordFlags Flags);
}
