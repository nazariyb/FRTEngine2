#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


namespace frt::config
{
class CConfig;
}


/**
 * The derived-data cache (design §7.4): the machine-wide, content-addressed store of everything a
 * build step produces, so that a value is built once per machine - not once per project, worktree or
 * branch - and a warm start builds nothing.
 *
 * Keys are content only. A step's StaticKey (Assets/KeyWriter.h, MakeStaticKey) hashes everything
 * known before it runs; its discovered inputs (shader includes, glTF side files) are known only after.
 * So the lookup has two steps, as in Unity's ADBv2 and ccache's direct mode: the manifest of a
 * StaticKey holds up to 4 recent sets of discovered inputs, each with the Key it produced
 * (ComputeKey(StaticKey, Inputs)); the first set whose inputs all still have their recorded hashes
 * gives the Key, and the Key names the record. A fresh clone, another worktree or a branch switch
 * back therefore hits what was built before.
 *
 * Layout under the root (UE's FileSystemCacheStore, Stride's ObjectDatabase):
 *
 *   FORMAT                                        "FRT-DDC 1"
 *   Manifests/<k0k1>/<static40hex>.man            up to 4 input sets -> Key, most recent first
 *   Records/<Bucket>/<k0k1>/<key40hex>.rec        a step's output: builder, version, values (small ones
 *                                                 inline), log hash, build time, determinism
 *   Values/<h0h1>/<h2h3>/<hash40hex>.val          one large value, named by the BLAKE3-160 of its raw bytes
 *   Tmp/                                          every write, before it is moved into place
 *   Access/<yyyymmdd>.log                         records used that day (UTC), for LRU garbage collection
 *   ToolVerdicts.txt                              external tool versions that failed a determinism check
 *
 * <k0k1> is the key's first byte in hex (256 directories), <h0h1>/<h2h3> its first two bytes. Bucket
 * groups the records of one client ("Shader", "Texture"): letters, digits, '.', '-' and '_'.
 *
 * File formats (all integers little-endian; varint = unsigned LEB128; Str = varint length + UTF-8;
 * Hash = the 20 bytes of an SContentHash):
 *
 *   Manifest = "FRTM", u32 Revision, Hash StaticKey, varint SetCount (1-4),
 *              Set * SetCount: Hash Key, varint InputCount, Input * InputCount,
 *              u64 XXH3_64 of every byte before it
 *   Input    = Str Kind, Str Name, Hash Hash, varint Size
 *   Record   = "FRTR", u32 Revision, Hash Key, Str Builder, 16 bytes Version (SGuid), varint ValueCount,
 *              Value * ValueCount, Hash LogHash, varint BuildMs, u8 bDeterministic,
 *              u64 XXH3_64 of every byte before it
 *   Value    = Str Name, Hash RawHash, varint RawSize, u8 Flags (1 Inline, 2 Zstd),
 *              if Inline: varint StoredSize, StoredSize bytes (raw, or one zstd frame with Zstd)
 *   .val     = "FRTV", u8 Codec (0 None, 1 Zstd), 3 zero bytes, u64 RawSize, Hash RawHash, payload
 *
 * Revision is the number in FORMAT (1). Every engine revision on the machine shares the store, so
 * these layouts and the limits readers check (DdcMaxManifestSets) belong to FORMAT: any change to
 * them is a new FORMAT, which engines of the other revision refuse to open - never a per-file version
 * that two revisions sharing one store would each judge corrupt in the other's files and delete.
 *
 * Values are stored as one zstd frame at level 3, or raw when that saves less than 5%. A value is
 * inlined into its record when its stored form is at most 64 KiB and the record stays within
 * 256 KiB: data assets, materials, mesh headers and mip tails then cost one open. While a record is
 * too large, its largest inline value moves out to Values/ (UE: MaxRecordSizeKB = 256).
 *
 * Crash and corruption rules. Every write goes to Tmp/, is flushed, and is moved into place with
 * platform::ReplaceFileAtomic, in the order large values, record, manifest: a crash leaves only
 * unreferenced files, never a dangling reference. Manifests and records end in an XXH3 of their bytes,
 * so a file that a power loss left zero-filled or truncated is detected. Any parse or checksum failure
 * is a miss, and the file is deleted; a value whose raw hash does not match is a miss and deleted too
 * (checked on every read in Debug and Development builds, SetVerifyValues). Writes are atomic and
 * idempotent: the editor, AssetTool and a second editor share one store without locks, and the last
 * writer of a key simply wins with equal bytes. A reader that loses a race with a writer or a
 * collector sees a miss, never a bad hit.
 *
 * Thread-safe once open: build steps read and write it from worker threads (§7.8), and calls on
 * different manifests never wait for each other. Open, Close and destruction are not: they change the
 * root every other call reads, so none of them may overlap any other call - the owner drains the work
 * that uses the cache first (CWorkerPool::CancelAllAndDrain), and builds with assertions catch a call
 * still in flight. Values and records are std::vector (CRT heap), never the engine's memory pool.
 * Reads are synchronous (CBlockingFileReader).
 */
namespace frt::assets
{
// The content of FORMAT. A store with another format is refused, never converted.
inline constexpr FStringLiteral DdcFormat = "FRT-DDC 1";

// The BLAKE3 derive_key context of ComputeKey.
inline constexpr FStringLiteral DdcKeyContext = "FRT DDC key v1";

// Inlining limits: a value whose stored form is at most this many bytes may live in its record ...
inline constexpr uint64 DdcMaxInlineValueBytes = 64u * 1024u;
// ... as long as the record file stays within this size.
inline constexpr uint64 DdcMaxRecordBytes = 256u * 1024u;
// A manifest keeps this many input sets, the most recent first. Readers refuse more: part of FORMAT.
inline constexpr uint32 DdcMaxManifestSets = 4u;

// The size garbage collection trims the store to unless [DerivedDataCache] MaxGB says otherwise (GiB of
// content: manifests, records, values). From the size model (§7.4 "GC", §11.7): it holds the projects of
// the Demo template with RGBA8 textures; revisited with BCn (M13).
inline constexpr double DdcDefaultMaxGB = 20.0;

// Whether CollectGarbage deletes, or only says what it would delete (`assets ddc gc --ci`).
enum class EDdcGcMode : uint8
{
	Delete,
	DryRun,
};


// One discovered input of a build step (§7.6): a file a step read through ReadFile, a file an
// external tool reported, an undeclared upstream output, a config value, a legacy path resolution.
// Kind names what Name is ("File", "Config", "Upstream", "Path"); Name is a mount path or a config
// key, never an absolute path. Hash and Size describe the content the step consumed.
struct SDdcInput
{
	FString Kind;
	FString Name;
	hash::SContentHash Hash;
	uint64 Size = 0u;

	[[nodiscard]] bool operator== (const SDdcInput&) const = default;
};

// One named output of a record ("Dxil", "Mip0"). Bytes are the raw bytes; RawHash and RawSize describe
// them, filled by the cache (on Put) and checked by it (on Get). Bytes stay empty when a record is
// read without its values.
struct SDdcValue
{
	FString Name;
	hash::SContentHash RawHash;
	uint64 RawSize = 0u;
	bool bInline = false;   // where the record that was read keeps it (informational; Put decides anew)
	std::vector<uint8> Bytes;
};

// What a build step produced, as one record. LogHash is the hash of the step's log (its text lives in
// the step's diagnostics); BuildMs how long the build took, for reports; bDeterministic false for
// outputs that must never leave the local store (§7.7).
struct SDdcRecord
{
	FString Builder;
	SGuid Version;
	std::vector<SDdcValue> Values;
	hash::SContentHash LogHash;
	uint64 BuildMs = 0u;
	bool bDeterministic = true;

	// The value named Name, or nullptr.
	[[nodiscard]] FRT_CORE_API const SDdcValue* FindValue (FStringView Name) const noexcept;
};

// Whether a read loads the values' bytes, or only what the record says about them (names, hashes,
// sizes: enough to chain on an upstream's output hash, or to check that a value exists).
enum class EDdcRead : uint8
{
	Values,
	MetadataOnly,
};

// The manifest's answer: the input set that is still current, and the Key it gave.
struct SDdcManifestMatch
{
	hash::SContentHash Key;
	std::vector<SDdcInput> Inputs;
};

// Files and bytes of one kind of file in the store.
struct SDdcFileStats
{
	uint64 Files = 0u;
	uint64 Bytes = 0u;
};

struct SDdcStats
{
	SDdcFileStats Manifests;
	SDdcFileStats Records;
	SDdcFileStats Values;
	SDdcFileStats Tmp;
	SDdcFileStats Access;
	SDdcFileStats Other;   // FORMAT, ToolVerdicts.txt, anything unexpected

	// What garbage collection trims: manifests, records and values.
	[[nodiscard]] uint64 GetContentBytes () const noexcept { return Manifests.Bytes + Records.Bytes + Values.Bytes; }
};

// What this instance did since it was opened (or the counters were reset). Plain numbers for tests,
// `assets ddc stats` and the shader cache's "warm start compiles nothing" check.
struct SDdcCounters
{
	uint64 RecordHits = 0u;
	uint64 RecordMisses = 0u;
	uint64 ManifestHits = 0u;     // a manifest had a current input set
	uint64 ManifestMisses = 0u;   // no manifest, or none of its sets is current
	uint64 RecordWrites = 0u;
	uint64 ValueWrites = 0u;
	uint64 ValueWritesSkipped = 0u;   // the content-addressed file was already there
	uint64 ManifestWrites = 0u;
	uint64 CorruptFilesDeleted = 0u;
};

struct SDdcGcResult
{
	uint64 ContentBytesBefore = 0u;   // manifests + records + values
	uint64 ContentBytesAfter = 0u;
	uint64 PinnedBytes = 0u;          // records named by PinnedKeys and the values they reference
	uint32 RecordsEvicted = 0u;
	uint32 ValuesEvicted = 0u;        // values no remaining record references
	uint32 ManifestsEvicted = 0u;     // manifests none of whose keys has a record any more
	uint32 TmpFilesDeleted = 0u;
	uint32 AccessLogsDeleted = 0u;
	uint32 RecordsUnreadable = 0u;    // records that exist but could not be read now (in use, no access)
	// False when the content still exceeds MaxBytes: pinned content alone does, or !bComplete.
	bool bWithinBudget = true;
	// False when a record could not be read or the records could not all be listed: then which values
	// are referenced is unknown, and only Tmp/ leftovers, old access logs and corrupt files were deleted.
	// A later collection does the rest.
	bool bComplete = true;
	// EDdcGcMode::DryRun: the counts and sizes say what a collection would have deleted; nothing was.
	bool bDryRun = false;
};

struct SDdcVerifyResult
{
	uint32 ManifestsChecked = 0u;
	uint32 RecordsChecked = 0u;
	uint32 ValuesChecked = 0u;
	uint32 CorruptManifests = 0u;   // deleted
	uint32 CorruptRecords = 0u;     // deleted
	uint32 CorruptValues = 0u;      // deleted
	uint32 DanglingRecords = 0u;    // records whose external value is missing or corrupt: deleted
	uint64 BytesDeleted = 0u;

	[[nodiscard]] uint32 GetProblemCount () const noexcept
	{
		return CorruptManifests + CorruptRecords + CorruptValues + DanglingRecords;
	}
};


class FRT_CORE_API CDerivedDataCache
{
public:
	CDerivedDataCache ();
	~CDerivedDataCache ();

	CDerivedDataCache (const CDerivedDataCache&) = delete;
	CDerivedDataCache& operator= (const CDerivedDataCache&) = delete;

	/**
	 * Where the machine's store is: FRT_DDC_DIR when set, else [DerivedDataCache] Path= of Config (a
	 * relative path is taken relative to the per-user data directory), else
	 * project::GetUserDataDir()/DerivedDataCache - %LOCALAPPDATA%\FRTEngine\DerivedDataCache, or under
	 * FRT_USER_DATA_DIR, which isolates tests. Keys are content only, so one store serves every project,
	 * template clone and worktree on the machine.
	 */
	[[nodiscard]] static std::filesystem::path GetDefaultRoot (const config::CConfig* Config = nullptr);

	// Opens the store at Root, creating it (and FORMAT) if needed. False, with the reason, when Root
	// cannot be created, holds a store of another format, or its FORMAT exists but cannot be read now
	// (it is left alone: it may say another format). Closes whatever was open. Must not overlap any
	// other call (see the class comment), nor may Close or the destructor.
	bool Open (const std::filesystem::path& Root, FString* OutError = nullptr);
	// Open(GetDefaultRoot(Config)).
	bool OpenDefault (const config::CConfig* Config = nullptr, FString* OutError = nullptr);
	void Close ();

	[[nodiscard]] bool IsOpen () const noexcept { return !Root.empty(); }
	[[nodiscard]] const std::filesystem::path& GetRoot () const noexcept { return Root; }

	/**
	 * The Key of a step: BLAKE3-160 derive_key(DdcKeyContext, EncodeKeyMaterial(StaticKey, Inputs)),
	 *
	 *   KeyMaterial = StaticKey (20 bytes), varint InputCount,
	 *                 Input * InputCount: varint len(Name), Name, varint len(Kind), Kind, Hash (20 bytes),
	 *                                     u64 Size (8 bytes little-endian)
	 *
	 * with the inputs in ascending byte order of Name, then Kind, Hash and Size (memcmp, a prefix
	 * first), exact duplicates once. So the order in which a step discovered its inputs, or reading one
	 * twice, changes nothing; the length prefixes keep ("ab", "c") and ("a", "bc") apart. Frozen: the
	 * golden test in Core-Test/Assets/DerivedDataCacheTest.cpp pins its bytes and its output, and a
	 * change needs a new context.
	 */
	[[nodiscard]] static hash::SContentHash ComputeKey (const hash::SContentHash& StaticKey, std::span<const SDdcInput> Inputs);
	[[nodiscard]] static std::vector<uint8> EncodeKeyMaterial (const hash::SContentHash& StaticKey, std::span<const SDdcInput> Inputs);
	// Inputs sorted and deduplicated as ComputeKey orders them (what manifests store).
	[[nodiscard]] static std::vector<SDdcInput> NormalizeInputs (std::span<const SDdcInput> Inputs);

	// A valid bucket name: 1-64 letters, digits, '.', '-' or '_', and not "." or "..".
	[[nodiscard]] static bool IsValidBucket (FStringView Bucket) noexcept;

	// ---- The two-step lookup ----

	/**
	 * Lookup step 2 (§7.4): the first input set of StaticKey's manifest, most recent first, whose every
	 * input IsCurrent says still has its recorded content; nullopt when there is no manifest or no set
	 * is current. IsCurrent compares the recorded hash (and size) with the input's current one.
	 */
	[[nodiscard]] std::optional<SDdcManifestMatch> FindKey (const hash::SContentHash& StaticKey,
		TFunctionRef<bool (const SDdcInput& Recorded)> IsCurrent);

	// FindKey, then GetRecord of the Key it found. OutMatch, when given, receives the match (the
	// inputs to record in BuildState).
	[[nodiscard]] std::optional<SDdcRecord> Get (FStringView Bucket, const hash::SContentHash& StaticKey,
		TFunctionRef<bool (const SDdcInput& Recorded)> IsCurrent, EDdcRead Read = EDdcRead::Values,
		SDdcManifestMatch* OutMatch = nullptr);

	// After a miss: Key = ComputeKey(StaticKey, Inputs), then PutRecord(Bucket, Key, Record) and
	// AddToManifest(StaticKey, Key, Inputs), in that order. The Key, or nullopt (with the reason)
	// when a write failed.
	std::optional<hash::SContentHash> Put (FStringView Bucket, const hash::SContentHash& StaticKey, std::span<const SDdcInput> Inputs,
		const SDdcRecord& Record, FString* OutError = nullptr);

	// ---- Records, values and manifests one by one ----

	// The record of Key, with its values' bytes unless MetadataOnly. nullopt on a miss: no record, a
	// corrupt one (deleted), or a value that is missing or corrupt (the value is deleted; the record
	// stays, and the next Put rewrites the value).
	[[nodiscard]] std::optional<SDdcRecord> GetRecord (FStringView Bucket, const hash::SContentHash& Key, EDdcRead Read = EDdcRead::Values);
	// Writes Record's values (the large ones as content-addressed files, unless already there), then
	// the record. Value names must be unique and non-empty. Returns false, with the reason, if a write
	// failed or the record is invalid; a failed write leaves at most unreferenced files.
	bool PutRecord (FStringView Bucket, const hash::SContentHash& Key, const SDdcRecord& Record, FString* OutError = nullptr);

	// The raw bytes of a large value by its raw hash (RawSize as the record says), checked like
	// GetRecord's values. nullopt when missing or corrupt (then deleted).
	[[nodiscard]] std::optional<std::vector<uint8>> ReadValue (const hash::SContentHash& RawHash, uint64 RawSize);

	// Puts (Key, Inputs) first in StaticKey's manifest: any older set with the same Key goes, and at
	// most DdcMaxManifestSets stay. Read-modify-write, serialized per manifest within the process (other
	// manifests do not wait): two processes adding to one manifest at once may lose one of the two sets
	// (a later miss rebuilds it), never corrupt the file.
	bool AddToManifest (const hash::SContentHash& StaticKey, const hash::SContentHash& Key, std::span<const SDdcInput> Inputs,
		FString* OutError = nullptr);
	// The manifest's sets, most recent first; empty when missing or corrupt (then deleted).
	[[nodiscard]] std::vector<SDdcManifestMatch> ReadManifest (const hash::SContentHash& StaticKey);

	// ---- Maintenance ----

	// Files and bytes per kind, from one walk of the store.
	[[nodiscard]] SDdcStats GetStats () const;
	[[nodiscard]] SDdcCounters GetCounters () const noexcept;
	void ResetCounters () noexcept;

	/**
	 * Trims the store to MaxBytes of content (manifests, records, values) in least-recently-used order,
	 * judged from the access logs (a record without a logged use counts from its file's write time;
	 * NTFS last-access times are unreliable, §7.4). Never evicts a record named in PinnedKeys - what
	 * any open project's BuildState points at - nor a value such a record references. A value goes
	 * when no remaining record references it; a manifest when none of its keys has a record left.
	 * Leftovers that a crashed writer may have left - Tmp/ files, and values no record references -
	 * are deleted only once they are older than an hour, so a write in progress in another process is
	 * never pulled from under it. A writer that reuses a value already in the store refreshes its write
	 * time before writing the record that names it, and a value whose time changed since the walk is
	 * kept, so a record written while a collection runs keeps its values (a lost race in the last
	 * microseconds is a miss and a rebuild, never a bad hit). Which values are referenced is known only
	 * when every record was read: if one could not be (a scanner holding it) or the records could not
	 * all be listed, nothing but leftovers in Tmp/ goes (bComplete false). Access logs older than 30
	 * days are deleted.
	 *
	 * EDdcGcMode::DryRun deletes nothing - not even a corrupt file it meets - and reports what the same
	 * collection would delete now: for a read-only run (`assets ddc gc --ci`).
	 */
	SDdcGcResult CollectGarbage (uint64 MaxBytes, std::span<const hash::SContentHash> PinnedKeys = {},
		EDdcGcMode Mode = EDdcGcMode::Delete);

	// Re-checks every manifest, record and value (checksums, raw hashes, and that every external
	// value a record references is there and intact), deleting what fails. Values are checked here
	// whatever SetVerifyValues says.
	SDdcVerifyResult Verify ();

	// Whether reads check each value's raw hash: on by default in Debug and Development builds, off
	// in Release (§7.4: "Dev builds verify the raw hash on read"). Records and manifests are always
	// checked (their XXH3 is cheap).
	void SetVerifyValues (bool bVerify) noexcept { bVerifyValues.store(bVerify, std::memory_order_relaxed); }
	[[nodiscard]] bool GetVerifyValues () const noexcept { return bVerifyValues.load(std::memory_order_relaxed); }

	// ---- Paths (for tools and tests) ----

	[[nodiscard]] std::filesystem::path GetManifestPath (const hash::SContentHash& StaticKey) const;
	[[nodiscard]] std::filesystem::path GetRecordPath (FStringView Bucket, const hash::SContentHash& Key) const;
	[[nodiscard]] std::filesystem::path GetValuePath (const hash::SContentHash& RawHash) const;
	[[nodiscard]] std::filesystem::path GetAccessLogPath (FStringView Day) const;
	[[nodiscard]] std::filesystem::path GetTmpDir () const;

	// ToolVerdicts.txt: one external tool version per line ("Blender 4.2.3"), for tools that failed a
	// determinism check (§7.7). Read only here: empty when the file is missing.
	[[nodiscard]] std::vector<FString> ReadToolVerdicts () const;

private:
	struct SAtomicCounters
	{
		std::atomic<uint64> RecordHits = 0u;
		std::atomic<uint64> RecordMisses = 0u;
		std::atomic<uint64> ManifestHits = 0u;
		std::atomic<uint64> ManifestMisses = 0u;
		std::atomic<uint64> RecordWrites = 0u;
		std::atomic<uint64> ValueWrites = 0u;
		std::atomic<uint64> ValueWritesSkipped = 0u;
		std::atomic<uint64> ManifestWrites = 0u;
		std::atomic<uint64> CorruptFilesDeleted = 0u;
	};

	struct SKeyHash
	{
		size_t operator() (const hash::SContentHash& Key) const noexcept;
	};

	std::filesystem::path Root;
	std::atomic<bool> bVerifyValues;
	mutable SAtomicCounters Counters;

	// Unique names in Tmp/: a random prefix per instance (other processes) and a counter (threads).
	FString TmpPrefix;
	std::atomic<uint64> TmpCounter = 0u;

	// Read-modify-write of manifests within this process, striped by StaticKey: one manifest's update
	// (a read, a flushed write and a write-through rename) never makes another manifest wait.
	static constexpr uint32 ManifestLockCount = 64u;
	std::array<std::mutex, ManifestLockCount> ManifestMutexes;

	// Calls in progress, counted in builds with assertions: Open and Close assert that there are none,
	// since they change Root under them (see the class comment).
	mutable std::atomic<uint32> CallsInFlight = 0u;
	struct SCallScope;

	// The access log of the current day, and the records this instance already logged in it: each
	// record is logged once per instance and day, which is all an LRU by session needs.
	std::mutex AccessMutex;
	FString AccessDay;
	platform::CAppendFile AccessLog;
	std::unordered_set<hash::SContentHash, SKeyHash> AccessLogged;

	// Writes Bytes to Path through a new file in Tmp/.
	bool WriteThroughTmp (const std::filesystem::path& Path, std::span<const uint8> Bytes, FString* OutError);
	// Deletes a file found corrupt, and counts it.
	void DeleteCorrupt (const std::filesystem::path& Path);
	// Appends "<time> <Bucket> <key>" to today's access log, once per key and day.
	void LogAccess (FStringView Bucket, const hash::SContentHash& Key);
	// Reads and checks a .val file; nullopt (and the file deleted when it exists but is corrupt).
	std::optional<std::vector<uint8>> ReadValueFile (const hash::SContentHash& RawHash, uint64 RawSize, bool bVerify);
};
}
