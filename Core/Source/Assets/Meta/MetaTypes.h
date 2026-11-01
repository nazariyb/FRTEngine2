#pragma once

#include <optional>
#include <vector>

#include "CoreTypes.h"
#include "Enum.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Reflect/DataTree.h"


/**
 * A `.meta` file as data (design §3.2): the one vocabulary the meta reader and writer (MetaFile.h), the registry and
 * the lifecycle share, so none of them invents its own idea of what a meta holds.
 *
 * A meta holds overrides only: a present key is an explicit override even when it equals the default, and "reset to
 * default" deletes the key. That is why every optional key below can say "absent" (an empty tree, an empty list,
 * nullopt) rather than holding a default value, and why the writer never adds a key the file did not have.
 */
namespace frt::assets
{
// The top-level keys of §3.2, in the order the canonical emitter writes them. Unknown keys follow, in file order.
inline constexpr FStringLiteral MetaTopLevelKeys[] = {
	"FormatVersion", "Guid", "Importer", "SettingsVersion", "Settings", "Targets", "SubAssets", "Labels", "FormerNames",
	"Aliases", "FormerPaths", "Cook", "Chunk", "RebuildNonce",
};

// A key of a meta this build does not know: preserved and reported, never dropped (§3.2). `$note` lives here too, unreported.
struct SMetaUnknownKey
{
	FString Key;
	reflect::CDataTree Value;
};

// One meta. Only FormatVersion and Guid are required. The trees hold their key's value as a whole: empty = absent.
struct SAssetMeta
{
	uint32 FormatVersion = 1u;
	SGuid Guid;
	FString Importer;                            // the "Import as" pin; empty: the rules decide
	std::optional<uint32> SettingsVersion;       // present only with Settings
	reflect::CDataTree Settings;
	reflect::CDataTree Targets;
	reflect::CDataTree SubAssets;                // Renamed, Remap, Overrides
	std::vector<FString> Labels;                 // sorted when written
	std::vector<FString> FormerNames;            // parsed and preserved; nothing indexes them before M10 (§10.2)
	std::vector<SGuid> Aliases;
	std::vector<FString> FormerPaths;            // mount paths
	std::optional<ECookRule> Cook;
	std::optional<uint16> Chunk;
	std::optional<SGuid> RebuildNonce;
	std::vector<SMetaUnknownKey> UnknownKeys;    // in file order
};

/**
 * Ok: Meta is the file's content. Broken: a parse error, conflict markers or two `Guid:` lines (META004); Meta is
 * meaningless, and what identity survives is in SalvagedGuids.
 */
enum class EMetaParseState : uint8 { Ok, Broken };

/**
 * What reading a meta found. The byte-level facts are kept here rather than hidden by a text reader, because META005
 * (would be rewritten) and VCS001 (CRLF in a hashed text file) are about the file's bytes, not its meaning.
 */
struct SMetaParseResult
{
	EMetaParseState State = EMetaParseState::Broken;
	SAssetMeta Meta;
	std::vector<SGuid> SalvagedGuids;            // Broken: what the `Guid:` lines say, distinct, in file order (two: "keep A, alias B")
	std::vector<SDiagnostic> Diagnostics;        // why it is Broken; unknown keys
	bool bHasBom = false;
	bool bHasCrLf = false;
	bool bInvalidUtf8 = false;
	bool bCanonical = false;                     // re-emitting would reproduce these bytes exactly
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EMetaParseState,
	FRT_ENUM_ENTRY(frt::assets::EMetaParseState, Ok),
	FRT_ENUM_ENTRY(frt::assets::EMetaParseState, Broken));
