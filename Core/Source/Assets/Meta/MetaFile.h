#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/MetaTypes.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"


/**
 * The `.meta` file's bytes <-> SAssetMeta (design §3.2). A pure function of bytes in both directions: no file is touched
 * here, because the callers differ in how they must read and write. The registry reads on a worker with
 * platform::ReadFileChunksStable (Changed means retry, as Analyze does), and only the asset writer ever writes.
 *
 * ParseMeta takes the file's **raw bytes**, never what a text reader made of them: ReadFileText converts UTF-16 and strips
 * a BOM, which would hide exactly what META005 and VCS001 are about, and would hash different bytes than the file holds.
 *
 * What it reports, with Subject and File left for the caller to fill (like ParseYaml; Line and Column count in the file):
 * - **META004** (error), with State Broken: the bytes are not valid UTF-8, a merge conflict's markers, two `Guid:` lines at
 *   the top level, a YAML error, or a required or known key that does not hold what §3.2 says. A file this build cannot
 *   read in full is never rewritten: rewriting would drop whatever it failed to understand. So a wrong type in a single
 *   optional key breaks the meta instead of dropping the key. SalvagedGuids holds what each top-level `Guid:` line says, by
 *   a scan of the lines that needs no parser, so identity survives (the lifecycle's "keep A, alias B").
 * - **VCS001** (error) when the bytes hold a CRLF, whatever else they hold.
 * - **META005** (warning) with State Ok and bCanonical false: the file reads, and the next write would change its bytes.
 * - An unknown key: a warning with Code 0 (§13.4 names none) and FieldPath set; the key stays in UnknownKeys and is written
 *   back after the known ones. So is a FormatVersion newer than this build's: the file is kept, nothing is migrated.
 * Callers keep what their context raises: gathering takes META004 and VCS001, `validate` adds META005 (§13.4 "Runs").
 *
 * EmitMeta writes the §3.2 layout through the canonical emitter (Reflect/CanonicalEmitter.h): the fixed top-level order
 * of MetaTopLevelKeys, then unknown keys in file order; sorted, unique labels; a GUID as 32 lowercase hex, however it was
 * spelled when read; `SettingsVersion` only with `Settings`. Settings, Targets and SubAssets keep their keys in the
 * order read, in canonical style: their declared order arrives with reflection (M5), when something first writes them.
 * An absent key stays absent, since a present key is an override (§3.2). Re-emitting a canonical file reproduces its
 * bytes: bCanonical is that comparison.
 *
 * A key that holds nothing (`Labels:`, `Cook: ~`) reads as absent and goes on the next write, as an empty `Labels: []` does.
 *
 * Safe on a worker.
 */
namespace frt::assets
{
// Replaces Out with what Bytes hold. Never fails as a call: a bad file is State Broken and says why.
FRT_CORE_API void ParseMeta (FStringView Bytes, SMetaParseResult& Out);

/**
 * Meta's canonical bytes into Out (replacing it). False with Out empty, and the reason in OutError, when it cannot be
 * written: an invalid Guid, Alias or RebuildNonce, a FormatVersion of 0, a Settings, Targets or SubAssets that is not a
 * map, an unknown key that repeats a known one, or anything the emitter refuses (invalid UTF-8).
 */
[[nodiscard]] FRT_CORE_API bool EmitMeta (const SAssetMeta& Meta, FString& Out, SDiagnostic* OutError = nullptr);

// FILES' MetaHash and the check of a METAS copy: of the file's raw bytes, so that a hand-edited meta's copy verifies and
// restores byte for byte (§4.6). The caller stores 0 for no file at all.
[[nodiscard]] inline uint64 HashMetaBytes (FStringView Bytes) noexcept
{
	return hash::Xxh3_64(Bytes);
}
}
