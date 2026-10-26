#pragma once

#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"


/**
 * The importers the asset system knows (design §3.5.2-§3.5.3 "extension claims live in code", §7.2, §7.11):
 * their names, the extensions they claim, the version of their Analyze step, their main type and their
 * settings tables. Data only - the Analyze code lives with each importer - and one immutable static table, so
 * the rules engine (validating `Importer:`, `For:` and preset names), discovery and the importers' workers read
 * it without a lock.
 *
 * Every importer of the v1 design is named, available or not. Naming an unavailable one in a rule file is
 * not RULE001; its files are Unclaimed in M3 because FindImporterForFile skips it.
 *
 * Settings (§7.5): a table per settings struct, fields in declaration order with their defaults. In M3 no
 * field is Analyze-relevant - Analyze outputs are facts of the source (§3.5.2) - so a settings edit never
 * re-analyzes a file. Texture.Psd and Texture.Krita share Texture's table, Model.Blender takes Model.Gltf's
 * (§3.5.2 "settings are typed by importer").
 */
namespace frt::assets
{
// One settings field: its name, value kind (Enum, Int64, Double, Bool, Name, String), default as rule-file
// text, the accepted names of an Enum field (empty otherwise), and whether its value changes Analyze's output.
struct SSettingsField
{
	FStringLiteral Name;
	ETagType Kind;
	FStringLiteral Default;
	std::span<const FStringLiteral> EnumValues;
	bool bAnalyzeRelevant;
};

struct SSettingsTable
{
	FStringLiteral Name;
	std::span<const SSettingsField> Fields;
};

/**
 * One importer. Extensions start with '.' and may have several dots (".frtmat.yml"). AnalyzeVersion is a
 * constant made with `FRT.bat assets newguid`, never a build stamp: a new GUID re-analyzes every file the
 * importer claims, on every machine. Settings is null for an importer without settings.
 */
struct SImporterDesc
{
	FStringLiteral Name;
	std::span<const FStringLiteral> Extensions;
	SGuid AnalyzeVersion;
	reflect::TypeId MainType;
	const SSettingsTable* Settings;
	bool bAvailable;

	// The main record's type for a file this importer reads: MainType, except where one extension decides
	// otherwise (Input.Legacy: .frtinputmap is an frt.InputActionMap, .frtinputaction an frt.InputAction).
	// The file name's extension is matched like FindImporterForFile does.
	[[nodiscard]] FRT_CORE_API reflect::TypeId GetMainType (FStringView FileName) const noexcept;
};

// Every importer, available ones first, in a fixed order.
[[nodiscard]] FRT_CORE_API std::span<const SImporterDesc> GetImporters () noexcept;

// By name under the portable fold ("Texture", "model.gltf"); unavailable importers included. Null if unknown.
[[nodiscard]] FRT_CORE_API const SImporterDesc* FindImporter (FStringView Name) noexcept;

// The available importer that claims the file's extension: the longest claimed extension the name ends with,
// under the portable fold (".frtmat.yml" before a shorter claim), with at least one character before it
// (a dot-name such as ".png" claims nothing). Null when no available importer claims it. FileName may be
// a bare name or a path; only its end is read.
[[nodiscard]] FRT_CORE_API const SImporterDesc* FindImporterForFile (FStringView FileName) noexcept;

// The stem of a file name, the record's $Name and an input map's PrimaryName: without the longest extension any
// importer of the table claims, available or not ("Wood.frtmat.yml" -> "Wood"), else without its last extension;
// a dot-name keeps its dot (".gitkeep"). Views LeafName.
[[nodiscard]] FRT_CORE_API FStringView GetAssetStem (FStringView LeafName) noexcept;

// The index of the settings field named Name in Table (portable fold), or -1.
[[nodiscard]] FRT_CORE_API int32 FindSettingsField (const SSettingsTable& Table, FStringView Name) noexcept;


// Where an effective value came from (design §3.5.2 "Provenance"): File per the diagnostics convention (the
// rule file relative to its mount's owner root, '/' separated: "Config/Assets.yml",
// "Content/Environment/Foliage/Assets.yml"), its 1-based Line, and the index of the `Rules:` entry that set
// it (-1 = not from a `Rules:` entry: an importer default, a preset definition, a Scan list). An empty File
// is the importer's default.
struct SProvenance { FString File; uint32 Line = 0; int32 RuleIndex = -1; };

// One field's effective value: Field indexes the table's Fields, Text is its value as rule-file text.
struct SSettingValue { uint16 Field = 0; FString Text; SProvenance Source; };

/**
 * A file's resolved importer settings: every field of Table present, defaults included, in field order.
 * AnalyzeHash covers the Analyze-relevant fields only - XXH3 of their name/value pairs in field order - so it
 * is 0 for a table without such fields (all of M3).
 */
struct SResolvedSettings { const SSettingsTable* Table = nullptr; std::vector<SSettingValue> Values; uint64 AnalyzeHash = 0; };

/**
 * A record's AnalyzeStamp (§5.2 ASSETS): XXH3_64 (seed 0) over the 16 bytes of AnalyzeVersion followed by
 * AnalyzeHash in little-endian. A file is re-analyzed when its stamp changes: a new importer version or a
 * changed Analyze-relevant setting.
 */
[[nodiscard]] FRT_CORE_API uint64 MakeAnalyzeStamp (const SGuid& AnalyzeVersion, uint64 AnalyzeHash) noexcept;
}
