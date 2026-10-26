#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTables.h"
#include "Containers/String.h"


/**
 * The registry's persistent snapshot (design §5.3): the tables of a set of mounts written from an immutable
 * view, and loaded back - several files merged into one store - with every index rebuilt by the store
 * (CRegistryStore::AdoptImage). A snapshot only saves work: whatever it lacks, the start-up scan, rules and
 * Analyze rebuild, so nothing here ever fails start-up; a bad file or section is reported and dropped.
 *
 * File layout (little-endian): the 48-byte header - Magic "FRTAREG1", FormatVersion, SectionCount,
 * ProjectId, JournalBase, RulesBasis (u64; 0 = unknown, what files written before it held) - then SectionCount entries { Id u32; LayoutVersion u32; Offset u64;
 * Size u64; Xxh3 u64 }, then the sections at 16-byte aligned offsets. FormatVersion gates the container only;
 * each section has its own LayoutVersion and checksum (ESnapshotSection lists them; ALIASES, FORMERPATHS,
 * METAS and TOMBSTONES are written empty in M3). The image's string tables are written as they are (NAMES,
 * STRINGS, TYPES by stable type name), so ids of the writing process never leak into a file.
 *
 * Validity is per section, never all-or-nothing (§5.3): a section whose LayoutVersion differs or whose
 * checksum fails is dropped with what depends on it, and what was lost is what the start-up rebuilds:
 *	- MOUNTS, NAMES, FOLDERS or FILES lost: the file is not used (its mounts start cold, every file hashed
 *	  again). FILES names its files by FOLDERS and NAMES indices, as §5.2's layout does, so it cannot outlive
 *	  them; §5.3's "FILES survives everything except a FILES layout change" holds for code and config changes,
 *	  which leave those sections alone, not for a layout change or damage in NAMES or FOLDERS.
 *	- ASSETS lost: files keep their stat and hashes (no re-hash) but have no records: F3 analyzes them.
 *	- Analyze outputs lost - SUBENTRY, DEPS (edges and companions), a type table (TYPETABLES, per type: one
 *	  bad table drops only its type; a TagSchemaHash that differs from this build's drops it too), TYPES or
 *	  PROBLEMS: the records that had them are flagged ERecordFlags::StaleTags with AnalyzeStamp 0, the state
 *	  "Analyze again" has in §5.3. F3 re-analyzes every StaleTags record whose type is known. (A type name
 *	  this build does not know at all leaves its records frt.Unknown with Schemaless | StaleTags and their
 *	  stamps: they wait for their type, AdoptImage's rule.)
 *	- Rule outputs lost - LABELS or PROBLEMS: the RULEFILES rows are dropped too, and every record of those
 *	  mounts is re-evaluated (the file's journal is replayed without its rule-file upserts and removes, which
 *	  would make some folders look current again).
 *	- STRINGS lost: DEPS and PROBLEMS go with it (and type tables with a String key).
 *	- RULEFILES lost alone: every record of the file's mounts is re-evaluated.
 *	- RulesBasis changed: the header's RulesBasis is what the records were evaluated against besides the file's
 *	  own RULEFILES rows - CRulesEngine::GetRulesBasis: the rules semantics hash (importer table, rules engine
 *	  version, target) and, in AssetRegistry.snapshot, the RulesHash of the engine file /Project and the plugins
 *	  fall back to. When SSnapshotSource::RulesBasis is set and differs from the stored one (0, unknown, always
 *	  differs), the file is treated as having lost RULEFILES (bRuleFilesDropped, bRulesBasisChanged).
 * Whenever report.bRuleFilesDropped is set, F3 re-evaluates every record of that file's mounts. It never infers
 * this from a missing RULEFILES row: a folder without an Assets.yml, and a mount without a Config/Assets.yml, have
 * no row either, so a missing row cannot tell a lost hash from no file.
 *
 * Mount sets. WriteSnapshot writes the given mounts only; an edge that leaves the set is stored by the target's
 * mount path and re-resolved wherever the file is loaded (ExportImage). ReadSnapshots merges several files
 * (Engine.snapshot: /Engine; AssetRegistry.snapshot: /Project and its plugins) into one image, remapping each
 * file's mounts by name to this process's MountIds; a mount that is not mounted now, whose volume's case rule
 * changed, or that an earlier file already gave, is skipped (it starts cold).
 *
 * Each snapshot file has a journal beside it (GetJournalPath, Journal.h): the deltas committed since the
 * snapshot's JournalBase. ReadSnapshots replays it when its header names the snapshot's ProjectId and
 * JournalBase, and drops it otherwise; a torn tail is dropped. Loading is two steps so the reading runs on
 * a worker and the store keeps its owner thread: ReadSnapshots (read, checksum, decode; any thread) and
 * AdoptSnapshots (AdoptImage, then the journal's deltas committed in order; the store's owner thread).
 * LoadSnapshots does both.
 *
 * ProjectId: random, generated once per owner root into <root>/Local/Assets/ProjectId (never in the
 * .frtproject), so a snapshot copied from another project or engine checkout is not taken for this one's.
 */
namespace frt::assets
{
class CRegistryStore;
class CRegistryView;

inline constexpr uint8 SnapshotMagic[8] = { 'F', 'R', 'T', 'A', 'R', 'E', 'G', '1' };
inline constexpr uint32 SnapshotFormatVersion = 1u;
inline constexpr uint32 SnapshotHeaderBytes = 48u;
inline constexpr uint32 SnapshotSectionEntryBytes = 32u;

// Section ids, fixed in the file format (never renumbered; a removed section's id stays retired).
enum class ESnapshotSection : uint32
{
	Mounts = 1,          // name, MountId, case rule per mount
	Strings = 2,         // free text: target paths, companions, problem subjects, String tag values
	Names = 3,           // interned names: path segments, labels, tag keys, Name tag values, field paths
	Folders = 4,
	Files = 5,
	RuleFiles = 6,       // mount, folder and RulesHash only
	Assets = 7,          // records (and the importer-name table)
	SubEntry = 8,
	Labels = 9,
	Types = 10,          // per-file type index -> stable type name
	TypeTables = 11,     // per type: name, TagSchemaHash, keys by name, columns
	Deps = 12,           // edges and companions
	Aliases = 13,        // empty in M3
	FormerPaths = 14,    // empty in M3
	Metas = 15,          // empty in M3
	Tombstones = 16,     // empty in M3
	Problems = 17,
};

// <OwnerRoot>/Local/Assets/ProjectId as written: nullopt when there is none or it does not parse.
[[nodiscard]] FRT_CORE_API std::optional<SGuid> ReadProjectId (const std::filesystem::path& OwnerRoot);
// The id, created (a random GUID, written atomically) when there is none yet. Only a writer-lock holder
// calls it; nullopt (with the reason) when the file cannot be written.
[[nodiscard]] FRT_CORE_API std::optional<SGuid> ReadOrCreateProjectId (const std::filesystem::path& OwnerRoot, FString* OutError = nullptr);

// The journal that belongs to a snapshot file: the same path with the extension ".journal"
// (AssetRegistry.snapshot -> AssetRegistry.journal).
[[nodiscard]] FRT_CORE_API std::filesystem::path GetJournalPath (const std::filesystem::path& SnapshotPath);


// ---------------- Writing ----------------

struct SSnapshotWriteStats
{
	double ExportMs = 0.0;       // CRegistryView::ExportImage
	double EncodeMs = 0.0;       // sections, checksums
	double WriteMs = 0.0;        // WriteFileAtomic (flushed to disk)
	uint64 Bytes = 0u;
	uint64 JournalBase = 0u;     // the view's revision
	uint32 Records = 0u;
};

// The file's bytes for an image (EncodeSnapshot) - deterministic: one image gives one byte sequence.
[[nodiscard]] FRT_CORE_API std::vector<uint8> EncodeSnapshot (const SRegistryImage& Image, const SGuid& ProjectId, uint64 JournalBase,
	uint64 RulesBasis);

/**
 * Writes the given mounts of a view (every mount when empty) to Path through WriteFileAtomic, with
 * JournalBase = the view's revision. RulesBasis: CRulesEngine::GetRulesBasis of the engine the records were
 * evaluated with (false for Engine.snapshot, true for AssetRegistry.snapshot); 0 writes "unknown". Callable on
 * any thread (it only reads the view). The caller then starts the snapshot's journal again at that revision
 * (CRegistryJournal::Restart), on the store's owner thread.
 */
FRT_CORE_API bool WriteSnapshot (const CRegistryView& View, std::span<const MountId> Mounts, const std::filesystem::path& Path,
	const SGuid& ProjectId, uint64 RulesBasis, FString* OutError = nullptr, SSnapshotWriteStats* OutStats = nullptr);


// ---------------- Loading ----------------

/**
 * ProjectId is the owner root's id (ReadProjectId). A process that has none - ReadProjectId returned nullopt:
 * no <root>/Local/Assets/ProjectId yet, or an unreadable one - passes an invalid id and loads no snapshot of
 * that root (a cold start): a file it cannot tie to the root may be another project's or checkout's.
 * bAnyProject takes the file whatever project it names: tests and tools inspecting a given file only.
 * RulesBasis: this process's CRulesEngine::GetRulesBasis for the file (see the header comment); a file that
 * stored another one loads without its RULEFILES rows. 0: not compared (tools and tests inspecting a file).
 */
struct SSnapshotSource
{
	std::filesystem::path Path;   // the snapshot; its journal is GetJournalPath(Path)
	SGuid ProjectId;
	bool bAnyProject = false;
	uint64 RulesBasis = 0u;
};

enum class ESnapshotFileState : uint8
{
	Loaded,         // some of it is in the image (see the report for what was dropped)
	Missing,        // no file (a first start)
	Unreadable,     // the file could not be read
	Rejected,       // not a snapshot of this format or project, or a section every other one needs is bad
};

struct SSnapshotFileReport
{
	std::filesystem::path Path;
	ESnapshotFileState State = ESnapshotFileState::Missing;
	FString Message;                                   // why it was not loaded, or what was dropped
	uint64 Bytes = 0u;
	uint64 JournalBase = 0u;
	uint64 RulesBasis = 0u;                            // as the header stores it (0 = unknown)
	std::vector<FString> Mounts;                       // mounts loaded from this file
	std::vector<FString> SkippedMounts;                // in the file, not loaded (see Message)
	std::vector<ESnapshotSection> DroppedSections;     // bad, or lost with a section they depend on
	std::vector<FString> DroppedTypeTables;            // type tables dropped alone (bad, or another TagSchemaHash)
	uint32 Records = 0u;
	uint32 StaleRecords = 0u;                          // flagged StaleTags with AnalyzeStamp 0 (see the header comment)
	bool bRuleFilesDropped = false;                    // no RulesHash survived: the mounts re-evaluate every record
	bool bRulesBasisChanged = false;                   // why, when the cause was SSnapshotSource::RulesBasis

	// The journal beside the snapshot.
	bool bJournalFound = false;
	bool bJournalUsed = false;                         // its header named this snapshot (ProjectId, JournalBase)
	bool bJournalTornTail = false;                     // bytes after the last whole commit were dropped
	uint32 JournalCommits = 0u;                        // commits replayed
	uint64 JournalBytes = 0u;
	FString JournalMessage;
};

// What ReadSnapshots hands to AdoptSnapshots: one merged image, the journals' deltas in replay order and the
// revision to adopt the image at (the newest JournalBase).
struct SLoadedSnapshots
{
	SRegistryImage Image;
	std::vector<SRegistryDelta> JournalDeltas;
	uint64 Revision = 0u;
	std::vector<SSnapshotFileReport> Files;
	double ReadMs = 0.0;         // file reads
	double DecodeMs = 0.0;       // checksums, sections, merge, journals decoded
};

struct SSnapshotLoadResult
{
	std::vector<SSnapshotFileReport> Files;
	double ReadMs = 0.0;
	double DecodeMs = 0.0;
	double AdoptMs = 0.0;        // CRegistryStore::AdoptImage (every index rebuilt, targets resolved, CSR built)
	double ReplayMs = 0.0;       // the journals' deltas committed
	double TotalMs = 0.0;
	bool bAdoptedInBulk = false; // AdoptImage took the fresh-store path (false: replayed, or nothing to adopt)
	uint32 Records = 0u;         // records in the store afterwards

	[[nodiscard]] bool AnyLoaded () const noexcept
	{
		for (const SSnapshotFileReport& file : Files)
		{
			if (file.State == ESnapshotFileState::Loaded)
			{
				return true;
			}
		}
		return false;
	}
};

/**
 * Reads, checks and decodes the snapshot files and their journals into one image. Mounts: this process's
 * mounts (CMountTable's names, ids and case rules); each file's mounts are remapped to them by name. Empty:
 * the files' own ids are kept (tools and tests; a mount two files share is taken from the first). Pure: any
 * thread, never the store.
 */
[[nodiscard]] FRT_CORE_API SLoadedSnapshots ReadSnapshots (std::span<const SSnapshotSource> Sources, std::span<const SMountUpsert> Mounts = {});

// Adopts the image (when any file was loaded) and commits the journals' deltas in order. The store's owner
// thread; meant for an empty store (a store that holds records gets the image replayed, which is slower).
FRT_CORE_API SSnapshotLoadResult AdoptSnapshots (CRegistryStore& Store, SLoadedSnapshots&& Loaded);

// ReadSnapshots, then AdoptSnapshots, timed as one load (§5.11 "snapshot + journal load, all indices").
FRT_CORE_API SSnapshotLoadResult LoadSnapshots (CRegistryStore& Store, std::span<const SSnapshotSource> Sources,
	std::span<const SMountUpsert> Mounts = {});
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ESnapshotSection,
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Mounts, "MOUNTS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Strings, "STRINGS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Names, "NAMES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Folders, "FOLDERS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Files, "FILES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, RuleFiles, "RULEFILES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Assets, "ASSETS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, SubEntry, "SUBENTRY"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Labels, "LABELS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Types, "TYPES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, TypeTables, "TYPETABLES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Deps, "DEPS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Aliases, "ALIASES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, FormerPaths, "FORMERPATHS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Metas, "METAS"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Tombstones, "TOMBSTONES"),
	FRT_ENUM_ENTRY_NAME(frt::assets::ESnapshotSection, Problems, "PROBLEMS"));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ESnapshotFileState,
	FRT_ENUM_ENTRY(frt::assets::ESnapshotFileState, Loaded),
	FRT_ENUM_ENTRY(frt::assets::ESnapshotFileState, Missing),
	FRT_ENUM_ENTRY(frt::assets::ESnapshotFileState, Unreadable),
	FRT_ENUM_ENTRY(frt::assets::ESnapshotFileState, Rejected));
