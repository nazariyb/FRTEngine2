#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/DerivedDataCache.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Hash/StableHash.h"
#include "Platform/BlockingFileReader.h"
#include "Platform/FileSystem.h"


/**
 * Per-project build state (design §3.3, §7.4 "BuildState storage", §7.9): what the last build of
 * every step was, so that a warm start verifies steps locally - recompute the StaticKey, compare the
 * recorded inputs' hashes - without touching the derived-data cache, and so that a step that crashed
 * its process is not run again blindly.
 *
 * Both files live in the project's Local/Assets/ and belong to the process that owns the project
 * (Owner.lock, §5.9): one writer, so neither needs a lock across processes. Both can be deleted at
 * any time: losing BuildState.log costs key recomputation and DDC lookups, never a rebuild; losing
 * BuildJournal.log forgets which steps crashed.
 */
namespace frt::assets
{
inline constexpr FStringLiteral BuildStateFileName = "BuildState.log";
inline constexpr FStringLiteral BuildJournalFileName = "BuildJournal.log";

// <LocalDir>/Assets/BuildState.log and <LocalDir>/Assets/BuildJournal.log, LocalDir being the
// project's Local/ (paths::GetLocalDir()).
[[nodiscard]] FRT_CORE_API std::filesystem::path GetBuildStatePath (const std::filesystem::path& LocalDir);
[[nodiscard]] FRT_CORE_API std::filesystem::path GetBuildJournalPath (const std::filesystem::path& LocalDir);


// How a step's last build ended. Crashed: it took its process down (the journal saw it begin and
// never end); it is skipped with an error until the user retries or its source changes.
enum class EBuildStatus : uint8
{
	Ok = 0,
	Failed = 1,
	Crashed = 2,
};

// "Ok", "Failed", "Crashed".
[[nodiscard]] FRT_CORE_API FStringLiteral GetBuildStatusName (EBuildStatus Status) noexcept;
[[nodiscard]] FRT_CORE_API std::optional<EBuildStatus> ParseBuildStatus (FStringView Name) noexcept;


/**
 * Which step: one per asset, builder, sub-asset label and value of the builder's Target key field
 * (§7.13) - one for a data asset whatever the target, one per RHI for a texture. Asset is the asset's
 * id as text; Label is empty for the main asset, Target empty for builders without a target facet.
 *
 * Text form (the journal's <step> token): the four fields joined by '|', each with '%', '|', spaces,
 * control characters and DEL written as %XX (uppercase hex) - one token without spaces.
 */
struct SBuildStepId
{
	FString Asset;
	FString Builder;
	FString Label;
	FString Target;

	[[nodiscard]] FRT_CORE_API FString ToString () const;
	// The text form back; nullopt unless it has exactly four fields and valid escapes.
	[[nodiscard]] FRT_CORE_API static std::optional<SBuildStepId> Parse (FStringView Text);

	[[nodiscard]] bool operator== (const SBuildStepId&) const = default;
};


// A step's last build. KeyBlob is the canonical KeyBlob it was keyed by (Assets/KeyWriter.h): kept so
// a rebuild can say which field changed (§7.4 "Rebuild reasons"); StaticKey = MakeStaticKey(KeyBlob).
// Inputs are its discovered inputs, Key its DDC key (CDerivedDataCache::ComputeKey). Diagnostics are
// the step's problems as text lines until structured diagnostics are stored per step.
struct SBuildStateEntry
{
	SBuildStepId Step;
	hash::SContentHash StaticKey;
	std::vector<uint8> KeyBlob;
	std::vector<SDdcInput> Inputs;
	hash::SContentHash Key;
	EBuildStatus Status = EBuildStatus::Ok;
	std::vector<FString> Diagnostics;
};

// Whether Find reads the step's KeyBlob too (a second record): only a rebuild-reason report needs it.
enum class EBuildStateRead : uint8
{
	WithoutKeyBlob,
	WithKeyBlob,
};

struct SBuildStateLogStats
{
	uint64 FileBytes = 0u;
	uint32 Steps = 0u;          // distinct steps with a valid record
	uint32 KeyBlobs = 0u;       // distinct KeyBlobs
	uint32 Records = 0u;        // valid records in the file, superseded ones included
	uint32 BadRanges = 0u;      // torn or corrupt stretches the index load skipped
	uint64 DeadBytes = 0u;      // what Compact would drop: superseded records, unused blobs, bad ranges
	double IndexLoadMs = 0.0;   // the last index load
};


/**
 * Local/Assets/BuildState.log: an append-only log of the latest build of each step.
 *
 * Every record is framed and checksummed:
 *
 *   File   = "FRTBSLG1", Record*
 *   Record = FE 42 53 A7 (magic), u32 PayloadSize, u8 Kind, Payload, u64 XXH3_64 of (PayloadSize, Kind, Payload)
 *   Kind 1 (step)    = Str Asset, Str Builder, Str Label, Str Target, Hash StaticKey, Hash Key, u8 Status,
 *                      varint InputCount, (Str Kind, Str Name, Hash, varint Size) * InputCount,
 *                      varint DiagnosticCount, Str * DiagnosticCount
 *   Kind 2 (KeyBlob) = Hash StaticKey, the KeyBlob's bytes (the rest of the payload)
 *
 * (integers little-endian, varint unsigned LEB128, Str = varint length + UTF-8, Hash = 20 bytes).
 * KeyBlobs are stored once per distinct blob - most steps share builder, version and settings blocks
 * - and a step record names its blob by StaticKey, which is the blob's BLAKE3 derivation: the side
 * table is the index of blob records by StaticKey.
 *
 * The index (a 128-bit hash of the step -> offset of its latest record, StaticKey -> offset of its
 * blob) is built lazily, on the first Find or Append, from one sequential read of the log that
 * checks every record: the records themselves are read on demand. A record that fails its checksum, a torn tail, or garbage
 * of any kind is skipped by searching for the next magic number, so a bad record is a miss - the
 * step's previous record, or nothing - never a hit. Budgets (§7.4), measured by Core-Test in
 * optimized builds: index load <= 50 ms at 100k records, Append <= 20 us.
 *
 * Compact rewrites the log atomically with only each step's latest record and the blobs they name
 * (on exit or idle). Thread-safe (one mutex); std containers only.
 */
class FRT_CORE_API CBuildStateLog
{
public:
	CBuildStateLog () = default;
	~CBuildStateLog ();

	CBuildStateLog (const CBuildStateLog&) = delete;
	CBuildStateLog& operator= (const CBuildStateLog&) = delete;

	// Opens or creates the log at File (and its directory). A file that is not a build state log of
	// this format is started anew: it is rebuildable. The index is not loaded yet.
	bool Open (const std::filesystem::path& File, FString* OutError = nullptr);
	void Close ();
	[[nodiscard]] bool IsOpen () const;
	[[nodiscard]] std::filesystem::path GetPath () const;

	// Appends Entry as the step's latest record (and its KeyBlob, if this blob is new to the log),
	// in one write. With a KeyBlob, the recorded StaticKey is MakeStaticKey(KeyBlob); without one,
	// Entry.StaticKey as given. A record holds at most 16 MiB, the most a reader accepts: diagnostics
	// beyond that are cut (the first lines kept whole, then one line saying how many were left out);
	// a KeyBlob or discovered inputs beyond it are refused (false, nothing written).
	bool Append (const SBuildStateEntry& Entry, FString* OutError = nullptr);

	// The step's latest valid record; nullopt when there is none (a miss).
	[[nodiscard]] std::optional<SBuildStateEntry> Find (const SBuildStepId& Step, EBuildStateRead Read = EBuildStateRead::WithoutKeyBlob);
	// A KeyBlob by its StaticKey, if the log holds it.
	[[nodiscard]] std::optional<std::vector<uint8>> FindKeyBlob (const hash::SContentHash& StaticKey);

	// Every step's latest record, in byte order of the step's fields (KeyBlobs not read).
	void ForEachStep (TFunctionRef<void (const SBuildStateEntry&)> Visit);
	// The DDC keys of every step's latest record: what garbage collection must not evict (§7.4).
	[[nodiscard]] std::vector<hash::SContentHash> CollectKeys ();
	// CollectKeys of the log at File, read once and never opened for writing: for another mount's log,
	// which its owner process may be appending to (ddc gc keeps what every known project's log names).
	// A file that is missing or not a log of this format is left as it is - nullopt, OutError saying
	// why - where Open would start it anew.
	[[nodiscard]] static std::optional<std::vector<hash::SContentHash>> ReadKeys (const std::filesystem::path& File,
		FString* OutError = nullptr);

	// Rewrites the log with only the latest record of each step and the KeyBlobs those name.
	bool Compact (FString* OutError = nullptr);

	// Builds the index now instead of on first use; false if the log cannot be read.
	bool LoadIndex (FString* OutError = nullptr);
	[[nodiscard]] SBuildStateLogStats GetStats ();

private:
	struct SLocation
	{
		uint64 Offset = 0u;   // of the record's magic
		uint32 Size = 0u;     // the whole record, framing included
	};

	// 128 bits of XXH3 of the step's fields as a record encodes them (four length-prefixed strings):
	// no allocation per step, so 100k of them index in a few milliseconds. A record found by it is
	// compared field by field with the step asked for, so even a collision is a miss, never a hit.
	struct SStepHash
	{
		uint64 Low = 0u;
		uint64 High = 0u;

		[[nodiscard]] bool operator== (const SStepHash&) const = default;
	};

	struct SStepHashHasher
	{
		size_t operator() (const SStepHash& Hash) const noexcept { return static_cast<size_t>(Hash.Low); }
	};

	struct SKeyHash
	{
		size_t operator() (const hash::SContentHash& Key) const noexcept;
	};

	mutable std::mutex Mutex;
	std::filesystem::path Path;
	platform::CAppendFile Writer;
	platform::CBlockingFileReader Reader;
	bool bIndexLoaded = false;
	uint64 FileSize = 0u;   // where the next record lands (this process is the only writer)
	std::unordered_map<SStepHash, SLocation, SStepHashHasher> Steps;   // step -> its latest record
	std::unordered_map<hash::SContentHash, SLocation, SKeyHash> KeyBlobs;
	SBuildStateLogStats Stats;

	[[nodiscard]] static SStepHash HashStep (std::span<const uint8> StepKey) noexcept;
	bool LoadIndexLocked (FString* OutError);
	// Indexes the records of Bytes (the whole file).
	void IndexBytes (std::span<const uint8> Bytes);
	// The payload of the record at Location, re-checked; nullopt if it no longer checks out.
	std::optional<std::vector<uint8>> ReadRecordLocked (const SLocation& Location, uint8 ExpectedKind);
	std::optional<SBuildStateEntry> FindLocked (const SBuildStepId& Step, EBuildStateRead Read);
	// Every step's latest record, decoded, in byte order of the step's fields.
	std::vector<std::pair<SBuildStateEntry, SLocation>> CollectStepsLocked ();
	bool OpenHandlesLocked (FString* OutError);
};


/**
 * Local/Assets/BuildJournal.log, the crash journal (§7.9): "B <step> <time>" is appended before a step
 * runs and "E <step> <result>" after it, so a step that takes the process down leaves a B without an
 * E. Open reports every such step as crashed and keeps it so - across restarts too - until EndStep or
 * Retry clears it: a crashing importer cannot crash-loop the editor.
 *
 * Lines are text: <step> is SBuildStepId's text form, <time> UTC as 2026-10-24T12:34:56Z, <result> a
 * status name or "Retry". A last line without its line break (torn by the crash) is ignored. Open
 * rewrites the file with only the still-open B lines, so it never grows past the steps in flight.
 * Appends reach the OS before the step runs, so they survive the process dying (not a power loss,
 * which the journal does not need to survive). Thread-safe: steps begin and end on worker threads.
 */
class FRT_CORE_API CBuildJournal
{
public:
	CBuildJournal () = default;
	~CBuildJournal ();

	CBuildJournal (const CBuildJournal&) = delete;
	CBuildJournal& operator= (const CBuildJournal&) = delete;

	// Reads the journal (creating it if missing), finds the crashed steps and rewrites it with only
	// their B lines.
	bool Open (const std::filesystem::path& File, FString* OutError = nullptr);
	void Close ();
	[[nodiscard]] bool IsOpen () const;

	// The steps that began and never ended, in byte order of their text form.
	[[nodiscard]] std::vector<SBuildStepId> GetCrashedSteps () const;
	[[nodiscard]] bool IsCrashed (const SBuildStepId& Step) const;

	bool BeginStep (const SBuildStepId& Step, FString* OutError = nullptr);
	// Clears the step's crashed state too (it ran to its end).
	bool EndStep (const SBuildStepId& Step, EBuildStatus Result, FString* OutError = nullptr);
	// The user's Retry: the step is no longer crashed.
	bool Retry (const SBuildStepId& Step, FString* OutError = nullptr);

private:
	mutable std::mutex Mutex;
	std::filesystem::path Path;
	platform::CAppendFile Writer;
	std::vector<FString> Crashed;   // text forms, sorted

	bool AppendLineLocked (FStringView Line, FString* OutError);
	void ClearCrashedLocked (const FString& StepText);
};
}
