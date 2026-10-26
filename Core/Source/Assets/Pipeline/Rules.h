#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Discovery/BuiltinExcludes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Discovery/ScanTypes.h"
#include "Assets/Pipeline/RuleTypes.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Diagnostics/Diagnostics.h"


/**
 * The rules engine (design §3.5, §5.4 step 3, §13.4 RULE001/RULE002): the mount and folder `Assets.yml` files read
 * into an immutable form, the scan oracle the mount scanner asks (IScanMembership), and per file the effective
 * importer, class, settings, cook rule, chunk and labels with where each came from.
 *
 * Why it is shaped like this:
 *	- Rule files are hand-written, so a bad one must never stop anything: a file with any error (RULE001, with file,
 *	  line and column) is rejected whole and the last good parse of that file stays in force - or, at a cold start
 *	  with none, the file contributes nothing. Applying half a file could drop an Exclude and import junk.
 *	- A folder's rules are an immutable scope (SScanScope) that chains to its parent's: the scanner keeps it per
 *	  folder (SScanFolder::Scope), so a file's verdict and its effective settings come from one rules snapshot even
 *	  when an Assets.yml edit lands meanwhile, and re-evaluating a subtree after an edit is glob matching in memory.
 *	- Everything is std containers and FString (workers call every member concurrently).
 *
 * Semantics (§3.5.2, §3.5.4), for a file F of mount M:
 *	- The chain of F is the rule files from the deepest folder file above F up to M's content root
 *	  (`Content/Assets.yml`), then M's `Config/Assets.yml`, then - for every mount but /Engine - the engine's
 *	  `Core/Config/Assets.yml` as the trailing fallback. No other mount's rules ever apply.
 *	- Globs are CPathGlob patterns relative to the file's folder (a folder file) or to M's root (a mount file). In a
 *	  mount file a `/Engine/`, `/Project/` or `/Plugin.<Name>/` prefix makes a pattern absolute: in the engine's file
 *	  it may name any mount (the pattern applies to that mount only), in another mount's file only its own mount
 *	  (anything else would reach outside it: RULE001). A folder file may use no mount prefix (RULE001).
 *	- Membership: built-in excludes (IsBuiltinExcludedName), then `Scan.Include` (every chain file that lists
 *	  patterns for M must match: a deeper file narrows, never widens), then `Scan.Exclude`, which accumulates
 *	  over the chain and always wins: a folder an exclude covers is never descended, its rule files never read.
 *	- Importer: `Scan.Raw` (any chain file) gives `Raw`; else the first matching `Match`-only rule that sets
 *	  `Importer:`; else the extension claim (FindImporterForFile). An importer in `Importers.Exclude` (any chain
 *	  file) leaves the file Unclaimed and quiet (the user disabled it on purpose); a rule naming an importer that is
 *	  not available in M3 (Document, Texture.Psd, Texture.Krita, Model.Blender, DataTable.Csv) leaves it Unclaimed.
 *	  `Scan.Quiet` (any chain file) marks an Unclaimed file quiet: it raises no RULE002 (bQuiet is computed for
 *	  Unclaimed files only; a claimed file is never quiet).
 *	- Rules: evaluated deepest file first, inside a file in order; every matching rule contributes and, per field,
 *	  the first rule that sets it wins. A rule matches when all its selectors do: `Match` (any of its globs),
 *	  `Type` (the record's type, known after Analyze: SFileFacts) and `Label` (an authored label). `Type`/`Label`
 *	  rules may set only fields that do not change Analyze (no `Importer`, no Analyze-relevant setting: RULE001,
 *	  for a setting their preset carries when the file is bound into a scope).
 *	  Inside one rule a full-target `Targets` block beats a platform block, which beats the plain fields; the
 *	  rule's inline `Settings` beat its `Preset`. Settings apply only to files whose importer uses the same
 *	  settings table (Texture.Psd and Texture.Krita share Texture's, Model.Blender takes Model.Gltf's). Labels
 *	  accumulate. `Final: true` stops evaluation for that file, outer files included; the `Final` of a `Type` or
 *	  `Label` rule never stops the importer or an Analyze-relevant setting, so those never depend on Analyze.
 *	  Fields no rule sets take the importer's defaults (SSettingsTable).
 *	- Presets `<Importer>/<Name>` are typed by that importer's settings table; a rule's preset resolves to the
 *	  innermost definition along F's chain (a project preset with an engine preset's name replaces it in that
 *	  mount; Explain notes the shadowing). A preset a rule names must be defined in its own file or an outer one
 *	  (RULE001 when the file is bound into a scope).
 *	- `Types.Include` / `Types.Exclude` (IsTypeIncluded) and `Validation:` severities (ApplyValidation) follow
 *	  the chain the same way; `Validation.Naming`, `Cook`, `PrimaryAssetTypes` and `Safety` are validated and
 *	  listed, never acted on in M3. The mount-level sections in a folder file are RULE001.
 *	- Unknown keys, wrong value types, bad globs, unknown importers, settings fields, enum values, platforms or
 *	  targets, and `frt.` type names the engine does not know are RULE001. Other type names are kept (reflected
 *	  game types arrive with M5) and match nothing yet. Keys are case-sensitive (YAML); enum values, importer,
 *	  type, target and mount names compare under the portable fold and are listed in their canonical spelling.
 *
 * Diagnostics follow the registry's convention: RULE001's Subject is the folder of the rule file (the mount root
 * for a mount's `Config/Assets.yml`, "/Engine" for the engine's), File the rule file relative to its mount's
 * owner root ("Config/Assets.yml", "Content/Environment/Foliage/Assets.yml", "Core/Config/Assets.yml"); no
 * absolute path except in a Message. A scan reports each rule file's problems once: a folder file's from
 * EnterFolder, a mount file's from OpenScope of that mount's root.
 *
 * RulesHash (§5.2 RULEFILES, §5.3): XXH3 of the file's normalised rules (the canonical text of every listed entry
 * in order, without line numbers), so a comment, blank line or formatting edit changes nothing. Never 0 for a
 * file that parsed (0 means "no rules"). It covers the text only: what the importer table, this engine's version
 * and the target make of that text is CRulesEngine::GetSemanticsHash, which F3 persists and compares too.
 */
namespace frt::assets
{
enum class ERuleFileLevel : uint8 { Mount, Folder };

/**
 * Where a rule file sits. MountName is the mount it belongs to ("Engine", "Project", "Plugin.X"); FolderMountPath
 * is the folder its globs are relative to (a mount root for a mount file); File is the provenance text (relative to
 * the mount's owner root, '/' separated).
 */
struct SRuleFileOrigin
{
	FString MountName;
	FString FolderMountPath;
	FString File;
	ERuleFileLevel Level = ERuleFileLevel::Folder;

	bool operator== (const SRuleFileOrigin&) const = default;
};

/**
 * The registry's RULEFILES row of a rule file (RegistryDelta.h's spelling): a mount's Config/Assets.yml has an empty
 * FolderMountPath there, while an origin, a chain entry and Rules.cache name it by the mount root, as they name the
 * content root's Assets.yml (Level tells the two apart). Every RULEFILES write and lookup goes through this.
 */
[[nodiscard]] inline SRuleFileUpsert ToRuleFileUpsert (const SRuleFileOrigin& Origin, uint64 RulesHash)
{
	return SRuleFileUpsert { Origin.MountName, Origin.Level == ERuleFileLevel::Mount ? FString() : Origin.FolderMountPath, RulesHash };
}

/**
 * One normalised entry of a rule file, as `list-rules` and `Rules.cache` show it. Kind names the section
 * ("FormatVersion", "Scan.Include", "Scan.Exclude", "Scan.Quiet", "Scan.Raw", "Importers.Exclude", "Types.Include",
 * "Types.Exclude", "Preset", "Rule", "Validation", "Validation.Naming", "Cook.Mode", "Cook.Roots", "Cook.PathLookup",
 * "Cook.CookedTags", "Cook.Targets", "Cook.ContentId", "Cook.ContentVersion", "PrimaryAssetTypes", "Safety"); Text is
 * its canonical one-line form in YAML flow syntax (a glob as a double-quoted string, a rule as a flow map with its
 * keys in a fixed order: Match, Type, Label, Preset, For, Importer, Settings, Labels, Cook, Chunk, Targets, Final).
 * RuleIndex is the 0-based index of a `Rules:` entry (-1 otherwise; explain prints it 1-based).
 */
struct SRuleListEntry
{
	FString Kind;
	FString Text;
	uint32 Line = 0u;
	int32 RuleIndex = -1;

	bool operator== (const SRuleListEntry&) const = default;
};

/**
 * A parsed rule file: immutable once made, shared by every scope that chains to it. Parse is the only way to make
 * one, so every instance is a file without errors.
 */
class FRT_CORE_API CRuleFile
{
public:
	struct SData;   // the compiled rules (Rules.cpp)

	/**
	 * Parses an `Assets.yml` text (FormatVersion 1). nullptr when the text has any error - each one a RULE001 in
	 * OutDiagnostics with Subject = Origin.FolderMountPath, File = Origin.File, Line and Column - and the caller
	 * keeps the last good rules. A text of comments only is a valid file with no rules. Never throws, never reads
	 * the disk; safe on any thread.
	 */
	[[nodiscard]] static std::shared_ptr<const CRuleFile> Parse (FStringView Text, const SRuleFileOrigin& Origin, std::vector<SDiagnostic>& OutDiagnostics);

	CRuleFile (SRuleFileOrigin InOrigin, std::unique_ptr<const SData> InData, std::vector<SRuleListEntry> InEntries, uint64 InRulesHash) noexcept;
	~CRuleFile ();
	CRuleFile (const CRuleFile&) = delete;
	CRuleFile& operator= (const CRuleFile&) = delete;

	[[nodiscard]] const SRuleFileOrigin& GetOrigin () const noexcept { return Origin; }
	[[nodiscard]] uint64 GetRulesHash () const noexcept { return RulesHash; }
	[[nodiscard]] std::span<const SRuleListEntry> GetEntries () const noexcept { return Entries; }
	[[nodiscard]] const SData& GetData () const noexcept { return *Data; }

private:
	SRuleFileOrigin Origin;
	std::unique_ptr<const SData> Data;
	std::vector<SRuleListEntry> Entries;
	uint64 RulesHash = 0u;
};

/**
 * Every rule file the engine knows, for `list-rules`, `Rules.cache` and the editor's "Effective rules" view: each
 * mount's `Config/Assets.yml` that exists, then every folder file a scan has entered, by mount id, the mount file
 * first, then folders in SStringLessIgnoreCase order (ties by bytes). A malformed file lists its last good entries
 * and RulesHash (none and 0 at a cold start) with bMalformed set and the errors of its latest read.
 */
struct SRuleFileListing
{
	SRuleFileOrigin Origin;
	uint64 RulesHash = 0u;
	bool bMalformed = false;
	std::vector<SRuleListEntry> Entries;
	std::vector<SDiagnostic> Diagnostics;

	bool operator== (const SRuleFileListing&) const = default;
};

struct SRulesListing
{
	std::vector<SRuleFileListing> Files;

	bool operator== (const SRulesListing&) const = default;
};

// What Analyze learned about a file, for the `Type` and `Label` selectors. Type InvalidTypeId (before Analyze):
// Type rules match nothing. AuthoredLabels are the file's `.meta` labels (none in M3).
struct SFileFacts
{
	reflect::TypeId Type = reflect::InvalidTypeId;
	std::span<const FString> AuthoredLabels;
};

// How the engine resolves. Target: the target a host builds and loads for ("Win64-D3D12", "Win64-Headless"),
// whose `Targets:` blocks apply (its platform's blocks too); empty: none apply, only the plain fields.
struct SRulesOptions
{
	FString Target;
};

// A rule file a file's chain consulted, deepest first: what explain lists.
struct SRuleChainEntry
{
	SRuleFileOrigin Origin;
	uint64 RulesHash = 0u;
};

/**
 * Everything explain shows for one file. Verdict nullopt: excluded, and ExcludedBy says why ("built-in exclude",
 * "not in Scan.Include (Config/Assets.yml:4)", "Scan.Exclude \"Docs/**\" (Config/Assets.yml:7)"). Settings: the
 * effective settings with provenance (Settings.Class is Excluded for an excluded file). Notes: preset shadowing,
 * the Final that stopped evaluation, a type filtered out by Types (when Facts names a type).
 */
struct SRuleExplanation
{
	std::optional<SFileVerdict> Verdict;
	FString ExcludedBy;
	SEffectiveFileSettings Settings;
	std::vector<SRuleChainEntry> Chain;
	std::vector<FString> Notes;
	bool bTypeIncluded = true;
};

/**
 * The rules of a process's mounts, and the scan oracle over them. Construct it from the mount table (copied), then
 * LoadMountRules - OpenScope does that itself the first time. Every member is safe to call from any thread at
 * once: the parsed files and scopes are immutable, the small mutable state (each mount file's and each folder
 * file's last good parse) sits behind one mutex held only for a lookup or a store. Loads of the mount files are
 * serialised (concurrent LoadMountRules calls, and the first OpenScope's implicit load, run one after another), so
 * the newest read is the one that stays.
 *
 * Reading rule files: EnterFolder reads the folder's Assets.yml and re-parses it only when its text changed since
 * the last read; OpenScope of a subtree root reads the root's ancestors' files (never inside an excluded folder;
 * each found by listing its folder, as the scanner finds it, so a case-sensitive folder's "assets.yml" counts and
 * the recorded spelling is the on-disk one; a folder that cannot be listed keeps its last read);
 * LoadMountRules reads every mount's `Config/Assets.yml`. EnterFolderFromText and EnterFolderCached bind a folder
 * without I/O (an edit the watcher delivered; a subtree re-evaluated in memory after an outer file changed).
 */
class FRT_CORE_API CRulesEngine final : public IScanMembership
{
public:
	explicit CRulesEngine (const CMountTable& Mounts, SRulesOptions Options = {});
	~CRulesEngine () override;
	CRulesEngine (const CRulesEngine&) = delete;
	CRulesEngine& operator= (const CRulesEngine&) = delete;

	/**
	 * (Re)reads every mount's `Config/Assets.yml` (a missing one is no error: no rules) and rebuilds the mount
	 * scopes; scopes handed out earlier keep the rules they were made with. A file with errors keeps its last good
	 * rules. OutDiagnostics gets, per mount, the complete Rules list of the mount root's Subject: the mount file's
	 * RULE001s and those of the content root's Assets.yml as last read, bound against the new chain (the two files
	 * share the Subject, see below). OutChangedMounts gets the names of the mounts whose chain changed (their own
	 * file's RulesHash, or the engine file's for every mount but /Engine) - all of them at the first load. Call it
	 * when a watcher reports a mount rule file changed.
	 *
	 * The mount-root Subject: a mount's Config/Assets.yml and its content root's Assets.yml both report under the
	 * mount root ("/Project"), one (Subject, Rules) problem list. Every entry point that reports for that Subject -
	 * LoadMountRules, and EnterFolder, EnterFolderFromText, EnterFolderCached and OpenFolderScope of the mount root -
	 * returns both files' diagnostics, so its result is always the Subject's complete list and the caller replaces the
	 * list with it (SProblemsSet). OpenScope of a mount root reports nothing: the root's EnterFolder that follows it
	 * (the scan protocol, ScanTypes.h) does.
	 */
	void LoadMountRules (std::vector<SDiagnostic>* OutDiagnostics = nullptr, std::vector<FString>* OutChangedMounts = nullptr);
	// The RulesHash of a mount's `Config/Assets.yml` (RULEFILES' row with no folder); 0 when it has none or no good parse.
	[[nodiscard]] uint64 GetMountRulesHash (FStringView MountName) const;
	/**
	 * What a file's class, importer, settings, cook rule and chunk depend on besides the rule texts, which RulesHash
	 * alone covers: XXH3 of RulesEngineVersion (bumped when this engine's own semantics change - precedence, the
	 * meaning of a Cook: value, IsBuiltinExcludedName's list), every ImporterTable row (name, extensions, availability, main
	 * type, settings table with each field's name, kind, default, enum values and Analyze relevance), the targets
	 * and their platforms, and Options.Target. Never 0; fixed for the engine's lifetime.
	 *
	 * It goes into each snapshot file through GetRulesBasis (the snapshot header's RulesBasis, Snapshot.h): a later
	 * build that adds an extension claim, makes an importer available or changes a settings default must not leave
	 * unchanged files with their old class and settings. Re-analysis still follows each record's AnalyzeStamp, so
	 * this costs glob matching, not I/O. The registry should resolve with an empty Target, so the CookRule and Chunk
	 * it stores do not depend on which host wrote the snapshot (the editor and a headless host share one);
	 * per-target values are a cook-time query on an engine made with that target.
	 */
	[[nodiscard]] uint64 GetSemanticsHash () const noexcept { return SemanticsHash; }
	/**
	 * What a snapshot file's records were evaluated against besides the RULEFILES rows the file holds itself (its
	 * mounts' own rule files): the semantics hash, and for a file with any mount but /Engine also the engine
	 * file's RulesHash, since /Project and every plugin fall back to Core/Config/Assets.yml (§3.5.4) while that row
	 * lives only in Engine.snapshot, which editors never write. bWithEngineFallback: false for Engine.snapshot,
	 * true for AssetRegistry.snapshot. F3 passes it to WriteSnapshot and as SSnapshotSource::RulesBasis; a file
	 * whose stored basis differs loses its RULEFILES rows at load (bRuleFilesDropped), so its mounts re-evaluate.
	 * Never 0 (0 is "unknown" in the header).
	 */
	[[nodiscard]] uint64 GetRulesBasis (bool bWithEngineFallback) const;

	// ---- IScanMembership (ScanTypes.h has the protocol) ----

	// The scope of everything strictly above FolderMountPath. For a mount root, the mount's chain (the mount file's
	// diagnostics come with the root's EnterFolder, see LoadMountRules); for a subtree root, the ancestors' folder
	// files are read and bound too (their diagnostics belong to their own folders' scans and are not repeated). An
	// unknown mount gives a scope that excludes everything, with a code-0 error.
	std::shared_ptr<const SScanScope> OpenScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const override;
	std::shared_ptr<const SScanScope> EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		const std::filesystem::path* RuleFile, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const override;
	bool ShouldDescend (const SScanScope& Scope, FStringView FolderMountPath) const override;
	std::optional<SFileVerdict> Classify (const SScanScope& Scope, FStringView FileMountPath) const override;

	/**
	 * EnterFolder with the rule file's text given instead of read: Text nullopt = the folder has no Assets.yml.
	 * FileName is the rule file's on-disk spelling (for provenance). Updates the folder's last good parse like
	 * EnterFolder does.
	 */
	std::shared_ptr<const SScanScope> EnterFolderFromText (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		std::optional<FStringView> Text, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics, FStringView FileName = RuleFileName) const;
	/**
	 * EnterFolder from the folder's last read, no I/O: for re-evaluating a subtree in memory after an outer rule
	 * file changed (the folders' own files did not). Repeats that read's diagnostics. A folder never read, or
	 * whose file was gone at the last read, has no rules (Parent, RulesHash 0).
	 */
	std::shared_ptr<const SScanScope> EnterFolderCached (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const;
	/**
	 * The scope OF a folder, its own Assets.yml included, read from disk without a scan: OpenScope followed by the
	 * folder's own EnterFolder. For explain, list panels and a single file a watcher reports. OutDiagnostics gets the
	 * folder's own file's problems (and the mount file's for a mount root); an excluded folder gives a scope that
	 * excludes everything.
	 */
	[[nodiscard]] std::shared_ptr<const SScanScope> OpenFolderScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const;
	// Forgets the folder files at or below SubtreeMountPath (a folder removed from disk), so listings stop naming them.
	void ForgetFolders (FStringView SubtreeMountPath);

	// ---- Per file (Scope: the scope of the file's folder, SScanFolder::Scope) ----

	/**
	 * The effective settings of a file (§3.5.2): importer, class, quiet, cook rule, chunk, rule labels and every
	 * field of the importer's settings table, with provenance per field unless bProvenance is false (the cheap
	 * form for re-evaluating many files). An excluded file comes back with Class Excluded and nothing else. Facts
	 * feeds the Type and Label selectors; without it those rules match nothing.
	 */
	[[nodiscard]] SEffectiveFileSettings ResolveFile (const SScanScope& Scope, FStringView FileMountPath, const SFileFacts& Facts = {},
		bool bProvenance = true) const;
	// Whether a record or sub-asset of this type is registered (Types.Include / Types.Exclude along the chain).
	[[nodiscard]] bool IsTypeIncluded (const SScanScope& Scope, reflect::TypeId Type) const;
	// Applies the chain's `Validation:` override for Diagnostic.Code (the deepest file that lists it wins).
	void ApplyValidation (const SScanScope& Scope, SDiagnostic& Diagnostic) const;
	// The `explain <file>` data.
	[[nodiscard]] SRuleExplanation Explain (const SScanScope& Scope, FStringView FileMountPath, const SFileFacts& Facts = {}) const;

	// The `list-rules` data (see SRuleFileListing).
	[[nodiscard]] SRulesListing GetListing () const;

	[[nodiscard]] const CMountTable& GetMounts () const noexcept { return Mounts; }
	[[nodiscard]] const SRulesOptions& GetOptions () const noexcept { return Options; }

	struct SScope;   // the SScanScope this engine hands out (Rules.cpp)

private:
	struct SFileState
	{
		SRuleFileOrigin Origin;
		std::shared_ptr<const CRuleFile> LastGood;
		uint64 TextHash = 0u;
		bool bRead = false;          // a text was read and parsed (the hash is valid)
		bool bMalformed = false;     // the latest read failed (parse errors or unreadable)
		std::vector<SDiagnostic> Diagnostics;   // of the latest read
	};

	void EnsureLoaded () const;
	void LoadMountRulesLocked (std::vector<SDiagnostic>* OutDiagnostics, std::vector<FString>* OutChangedMounts);   // LoadMutex held
	[[nodiscard]] std::shared_ptr<const SScanScope> OpenScopeImpl (FStringView FolderMountPath, bool bIncludeSelf, std::vector<SDiagnostic>& OutDiagnostics) const;
	[[nodiscard]] std::shared_ptr<const SScanScope> BindFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
		std::optional<FStringView> Text, bool bReadFailed, FStringView ReadError, FStringView FileName, uint64& OutRulesHash,
		std::vector<SDiagnostic>* OutDiagnostics) const;
	[[nodiscard]] SRuleFileOrigin MakeFolderOrigin (uint32 MountIndex, FStringView FolderMountPath, FStringView FileName) const;
	[[nodiscard]] int32 FindMountIndex (FStringView MountName) const noexcept;
	void UpdateFileState (SFileState& State, const SRuleFileOrigin& Origin, std::optional<FStringView> Text, FStringView ReadError) const;
	// For a mount root: the mount file's diagnostics (parse and bind), which share the root's Subject with the
	// content root's Assets.yml.
	void AppendMountRootDiagnostics (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const;

	CMountTable Mounts;
	SRulesOptions Options;
	FString TargetPlatform;                     // Options.Target's platform ("Win64"), empty without a target
	uint64 SemanticsHash = 0u;                  // GetSemanticsHash

	// LoadMutex serialises LoadMountRules whole (read, parse, store), so an older read never lands after a newer one;
	// it is taken before Mutex, never while holding it. Mutex guards the state below for a lookup or a store only.
	mutable std::mutex LoadMutex;
	mutable std::mutex Mutex;
	mutable bool bLoaded = false;
	mutable std::vector<SFileState> MountFiles;                       // by mount index
	mutable std::vector<std::shared_ptr<const SScope>> MountScopes;   // by mount index
	mutable std::vector<std::vector<SDiagnostic>> MountDiagnostics;   // by mount index: parse + bind of its own file
	mutable TStringMap<SFileState> FolderFiles;                       // by folder mount path, byte-exact
};

/**
 * RULE002 (§3.5.2, §13.4): files still Unclaimed after Analyze (companions known), reported once per folder and
 * extension (folded: ".PNG" and ".png" are one group), never for a quiet file. Subject: the folder mount path; the
 * registry keeps them under their own problem source, EProblemSource::Unclaimed, which the caller clears and sets
 * only when it recomputes the unclaimed files - never in the Rules list, which other steps replace at other times.
 * Warning severity; apply a scope's Validation override with ApplyValidation. Sorted by folder
 * (SStringLessIgnoreCase, then bytes), then extension.
 */
struct SUnclaimedFile
{
	FStringView MountPath;
	bool bQuiet = false;
};

[[nodiscard]] FRT_CORE_API std::vector<SDiagnostic> ReportUnclaimedFiles (std::span<const SUnclaimedFile> Files);

/**
 * `Local/Assets/Rules.cache` (§3.5, §3.5.4; GetRulesCachePath): a listing flattened to JSON lines, for shell hooks,
 * the packager and Lua, which never parse YAML. The first line is {"format":"frt.rules.cache","version":1}; then
 * per rule file a line {"kind":"file", "mount", "folder", "file", "level", "hash" (16 hex digits), "malformed"}
 * followed by one line per entry {"kind":<entry kind>, "mount", "file", "line", "rule", "text"} - the same lines
 * `list-rules --json` prints. Every line is a Text/JsonWriter.h object (AssetTool's --json writer) and is read back
 * with reflect::ParseJson, its inverse, so a folder name with an unpaired surrogate round-trips. FormatRulesCache
 * makes the text; WriteRulesCache writes it atomically and leaves an identical file untouched; ReadRulesCache reads it
 * back (nullopt for anything malformed or another version). Diagnostics are not stored.
 */
[[nodiscard]] FRT_CORE_API FString FormatRulesCache (const SRulesListing& Listing);
FRT_CORE_API bool WriteRulesCache (const std::filesystem::path& Path, const SRulesListing& Listing, FString* OutError = nullptr);
[[nodiscard]] FRT_CORE_API std::optional<SRulesListing> ReadRulesCache (const std::filesystem::path& Path, FString* OutError = nullptr);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ERuleFileLevel,
	FRT_ENUM_ENTRY(frt::assets::ERuleFileLevel, Mount),
	FRT_ENUM_ENTRY(frt::assets::ERuleFileLevel, Folder));
