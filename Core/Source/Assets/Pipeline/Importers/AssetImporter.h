#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetTypes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/RuleTypes.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Functional/FunctionRef.h"
#include "Hash/StableHash.h"


/**
 * The importers' Analyze step (design §5.5, §7.2, §7.11): per file, the facts the registry records without
 * building anything - the record's type, typed tags, sub-entries (glTF meshes, materials, images; table rows),
 * declared edges (glTF URIs, legacy texture paths, input actions, $asset references), companions (glTF buffers and unclaimed images: no edge, no record of their own)
 * and diagnostics. One importer per available row of ImporterTable: Texture, Model.Gltf, Material.Legacy,
 * Input.Legacy, Raw.
 *
 * Analyze outputs are facts of the source (§3.5.2): no M3 settings field is Analyze-relevant, so the settings are
 * passed for the interface's sake and the AnalyzeStamp (MakeAnalyzeStamp of the importer's AnalyzeVersion and the
 * settings' AnalyzeHash) changes only with a new importer version.
 *
 * Threading: importers are stateless singletons; Analyze is const and runs on CWorkerPool workers, many at once.
 * Everything here is std containers and FString (the CRT heap), never the engine's memory pool: the legacy readers
 * are ports of the runtime's line readers, not calls into them (those fill TArray-based structs). Nothing reads
 * or writes registry state; the owner thread commits what AppendRecordUpsert puts in a delta.
 *
 * What an importer reads: Texture a header prefix (stb_image's info functions on bytes, never a file name);
 * Model.Gltf the JSON only (a .gltf whole, a .glb's JSON chunk alone - never its BIN chunk, never a buffer); the
 * legacy text formats whole. Importers that read the whole file (Material.Legacy, Input.Legacy, .gltf) hash it in
 * the same pass through platform::ReadFileChunksStable and return the hash in SAnalyzeResult::ContentHash, so the
 * registry need not read the file a second time.
 */
namespace frt::reflect
{
class CDataTree;
}


namespace frt::assets
{
struct SAssetMeta;

/**
 * The file to analyze. FileMountPath is its mount path as spelled on disk ("/Project/Models/Duck/Duck.gltf");
 * Path the absolute path of the same file; SourceFile its path relative to its mount's owner root, '/' separated
 * ("Content/Models/Duck/Duck.gltf", "Core/Content/..."), which diagnostics carry as File (the diagnostics
 * convention: dumps never hold a machine path). FromMount makes all three from a mount and a mount path.
 */
struct SAnalyzeInput
{
	FString FileMountPath;
	std::filesystem::path Path;
	FString SourceFile;

	// nullopt when FileMountPath is not a well-formed file mount path of Mount.
	[[nodiscard]] FRT_CORE_API static std::optional<SAnalyzeInput> FromMount (const SMount& Mount, FStringView FileMountPath);
};

/**
 * What Analyze found. Type is the record's type (the importer's main type for the file even when Analyze fails,
 * so `type:Texture` still lists a texture stb cannot read). Tags use exactly the keys of Type in
 * CAssetTypeRegistry. Sub-entries carry their label, SubId (MakeSubId of the label, or a row's pinned id), type
 * and tags; edges come from the record (FromSub 0) or one of those SubIds. Diagnostics name the file (Subject =
 * FileMountPath, or FileMountPath#label for a sub-entry; File = SAnalyzeInput::SourceFile).
 *
 * ContentHash: set when the importer read the whole file in one stable pass (else nullopt: the registry hashes
 * lazily if it needs to). bSourceChanged: the file changed while it was read (platform::EStableRead::Changed);
 * the result describes no version of the file, so the caller retries after the watcher's settle window rather
 * than committing it (committing it anyway records AnalyzeFailed, which a later change of the file repairs).
 */
struct SAnalyzeResult
{
	reflect::TypeId Type = asset_types::Unknown;
	EImportStatus Status = EImportStatus::NotAnalyzed;
	ERecordFlags Flags = ERecordFlags::None;
	FString PrimaryName;                       // set with ERecordFlags::Primary (input maps)
	std::vector<STagDecl> Tags;
	std::vector<SSubEntryDecl> SubEntries;
	std::vector<SEdgeDecl> Edges;
	std::vector<FString> Companions;           // mount paths, no duplicates
	std::vector<SDiagnostic> Diagnostics;
	std::optional<hash::SContentHash> ContentHash;
	bool bSourceChanged = false;

	void Reset () noexcept { *this = SAnalyzeResult {}; }
};

/**
 * One importer (§7.2's subset for M3: no GetBuilds, no Translate). GetInfo is its ImporterTable row. Analyze
 * fills Out (which the caller reset and gave the main type of the file) and returns false when the file cannot
 * be analyzed - unreadable, not the format the importer reads, changed while read - after adding a diagnostic
 * saying why. Use AnalyzeFile rather than calling it directly: it applies the conventions above.
 */
class IAssetImporter
{
public:
	virtual ~IAssetImporter () = default;
	[[nodiscard]] virtual const SImporterDesc& GetInfo () const noexcept = 0;
	virtual bool Analyze (const SAnalyzeInput& In, const SResolvedSettings& Settings, SAnalyzeResult& Out) const = 0;
};

// Every importer with an Analyze implementation, in ImporterTable's order (the available rows).
[[nodiscard]] FRT_CORE_API std::span<const IAssetImporter* const> GetAssetImporters () noexcept;
// The implementation of an ImporterTable row; null for an unavailable importer (Document, Texture.Psd, ...).
[[nodiscard]] FRT_CORE_API const IAssetImporter* FindAssetImporter (const SImporterDesc& Importer) noexcept;
// By name under the portable fold; null when unknown or unavailable.
[[nodiscard]] FRT_CORE_API const IAssetImporter* FindAssetImporter (FStringView Name) noexcept;

/**
 * Runs the file's importer (Settings.Importer, as the rules engine resolved it) and fills Out: Status Analyzed or
 * AnalyzeFailed (the record is kept; a failure always carries an error with code 0, or a warning when the
 * runtime still reads the file), or ImporterUnavailable for a row without an implementation; NotAnalyzed when
 * Settings has no importer (an Unclaimed or Excluded file has no record). Diagnostics without a Subject or File
 * get the file's. Returns whether Status is Analyzed. Safe on workers.
 */
FRT_CORE_API bool AnalyzeFile (const SAnalyzeInput& In, const SEffectiveFileSettings& Settings, SAnalyzeResult& Out);

/**
 * Removes the sub-entries whose type Keep rejects - the rules engine's Types filter after Analyze (§3.5.2: "an
 * excluded sub-asset type is not registered"; Keep = CRulesEngine::IsTypeIncluded with the file's folder scope) -
 * so the result stays consistent: an edge a removed sub-entry declared moves to the record (FromSub 0; the file
 * still reads that reference, so the dependency stays), and the diagnostics about a removed sub-entry (Subject
 * FileMountPath#label) go with it. A sub-entry removed by hand would leave edges from a SubId the record does not
 * declare, which the store drops with a contract violation. The caller does this before AppendRecordUpsert.
 */
FRT_CORE_API void RemoveSubEntries (SAnalyzeResult& Result, FStringView FileMountPath, TFunctionRef<bool (reflect::TypeId)> Keep);

/**
 * The file's meta `SubAssets` (§3.2, §4.3) over Analyze's sub-entries, after RemoveSubEntries and before
 * AppendRecordUpsert:
 * - Renamed (new label: old label): the sub-entry of the new label keeps the old label's id (MakeSubId of it), and
 *   the edges it declares move with it; an id another sub-entry has is a warning and the rename is skipped;
 * - Remap (label: external `$asset` reference): the sub-entry gains a Hard | Declared edge to that asset, FieldPath
 *   "SubAssets.Remap.<label>" (an extracted material replacing an imported one is used where the model is);
 * - Overrides (label: settings): settings of that sub-asset's build; no sub-entry tag of M4 is a setting, so they only
 *   enter the AnalyzeStamp (Rules.h).
 * A label the file has no sub-entry for, and a malformed entry, is a warning (code 0) naming the meta (MetaFile, as
 * provenance names it) with its line: the rest applies. Safe on workers.
 */
FRT_CORE_API void ApplySubAssetOverrides (SAnalyzeResult& Result, const SAnalyzeInput& In, const SAssetMeta& Meta, FStringView MetaFile);

/**
 * Appends the record of FileMountPath to Delta: an SRecordUpsert (Guid - the meta's; invalid: MakeProvisionalGuid of
 * the path, for a file with no meta yet -, Type, importer name, flags, cook rule, chunk, rule and authored labels from
 * Settings; status, AnalyzeStamp, tags, sub-entries, edges and companions from Result; aliases - the renamed sub-entries'
 * (AppendRenamedSubAliases), then Meta's Aliases - and Meta's FormerPaths, M4), then the file's Analyze problems - an SProblemsClear of the Analyze source at the file (its sub-entries'
 * subjects included), and one SProblemsSet per subject Result's diagnostics name - so problems an earlier Analyze
 * reported do not survive. File rows are the caller's (Result.ContentHash can fill the SFileUpsert). The rvalue
 * form moves Result's vectors instead of copying them. Severities are stored as given: the caller applies a rule
 * file's Validation: overrides to Result.Diagnostics first (CRulesEngine::ApplyValidation with the file's folder
 * scope), as it does for the scan's and RULE002 diagnostics.
 */
FRT_CORE_API void AppendRecordUpsert (const SAnalyzeResult& Result, const SEffectiveFileSettings& Settings, FStringView FileMountPath, SRegistryDelta& Delta,
	const SGuid& Guid = SGuid {}, const SAssetMeta* Meta = nullptr);
FRT_CORE_API void AppendRecordUpsert (SAnalyzeResult&& Result, const SEffectiveFileSettings& Settings, FStringView FileMountPath, SRegistryDelta& Delta,
	const SGuid& Guid = SGuid {}, const SAssetMeta* Meta = nullptr);

/**
 * The schema-free scan of §5.5 step 4 over a parsed document, run on every CDataTree an importer produces (the
 * legacy material and input trees in M3; the Document importer's from M5):
 * - every map with a `$asset` key is a reference (§4.4): `$asset` the GUID, `sub` an importer label (holds ':')
 *   or `$` + 16 hex digits (a native id), `row` a row key with an optional `id` pin (`$` + 16 hex), `path` the
 *   mount-path hint. It becomes an edge to SAssetId{GUID, SubId} with EDepFlags::Schemaless (plus Row for a row),
 *   FieldPath the map's path in the tree ("Icon", "Rows.Rifle556.Tracer"), TargetPath the hint when it is a
 *   well-formed mount path. A `$asset: null` is no reference; a malformed one is a warning and no edge.
 * - a document whose root has `$type: frt.DataTable` and a `Rows:` map gets one sub-entry per row (§3.6, §4.3):
 *   Label the row key, SubId its `$id` pin or MakeSubId("row:" + key), type frt.Unknown, flags Schemaless
 *   (+ PinnedId). Keys that break §3.6's pattern, case-only duplicates and SubId collisions are errors and the
 *   row is skipped. References inside a row come from the row (FromSub = its SubId).
 * Appends to Out.Edges, Out.SubEntries and Out.Diagnostics; In names the file in diagnostics.
 */
FRT_CORE_API void GatherSchemaFree (const reflect::CDataTree& Tree, const SAnalyzeInput& In, SAnalyzeResult& Out);
}
