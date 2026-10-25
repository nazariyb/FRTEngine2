#pragma once

#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Containers/String.h"


/**
 * The builder source guard (design §7.3 "Code changed without a version bump", §4.8, BUILD003).
 *
 * A builder's version GUID is the promise that equal keys mean equal outputs. Someone who edits a
 * builder and forgets to bump its GUID breaks that promise silently: every key stays the same, so the
 * derived-data cache serves values the old code built - a stale hit, and in a cache shared by
 * worktrees on different branches, one that spreads. So every build hashes each builder's sources
 * (FRT.bat builder-hashes, a prebuild step of Core: Intermediate/Generated/BuilderSourceHashes.gen.h)
 * and development builds compare that hash with the one committed with the builder's version in
 * Assets/BuilderVersions.inl.
 *
 * A builder whose sources differ keeps working, because the edit-build-reload loop must stay correct
 * and fast: it runs with a local-only key discriminator - its current source hash, which the caller
 * writes as Discriminators.LocalSource - so its outputs never collide with the committed code's, and
 * it marks them local-only, like bDeterministic = false outputs: they stay in the local store and
 * never reach a shared one (§7.4 "Store chain"). The missing bump itself is an error where it belongs:
 * FRT.bat check-builders, in FRT.bat verify and the opt-in pre-commit hook.
 *
 * Usage, in the code that writes a builder's KeyBlob:
 *
 *   const SBuilderKeyVersion version = GetBuilderKeyVersion("Shader.Dxil");
 *   key.WriteGuid("Version", version.Version);
 *   if (version.bLocalOnly)          // with the builder's other discriminators, if any
 *   {
 *       key.BeginRecord("Discriminators");
 *       key.WriteStr(LocalSourceDiscriminator, version.LocalDiscriminator);
 *       key.EndRecord();
 *   }
 *   record.bDeterministic = record.bDeterministic && !version.bLocalOnly;   // never leaves this machine
 *
 * Release builds compare nothing: they have no developer tools and write no shared store, and their
 * keys (the cook's, M12) come from committed code only.
 */
namespace frt::assets
{
// The field of a key's "Discriminators" record that holds a local-only builder's source hash.
inline constexpr FStringLiteral LocalSourceDiscriminator = "LocalSource";

// What a builder's keys are written with (see above).
struct SBuilderKeyVersion
{
	SGuid Version;                // the key's "Version" field: the committed version GUID
	bool bLocalOnly = false;      // its outputs stay in the local store (and the key carries the discriminator)
	FString LocalDiscriminator;   // when bLocalOnly: the builder's current source hash (16 hex digits)

	[[nodiscard]] bool operator== (const SBuilderKeyVersion&) const = default;
};

// One row of Assets/BuilderVersions.inl, with the source hash this build was made from.
struct SBuilderInfo
{
	FStringLiteral Name;
	SGuid Version;
	FStringLiteral CommittedSourceHash;
	// The hash FRT.bat builder-hashes computed when this binary was built; the committed one in Release,
	// which does not compare.
	FStringLiteral CurrentSourceHash;
	FStringLiteral Globs;   // "glob;glob", engine-relative
};

// Every row of the table, in its order.
[[nodiscard]] FRT_CORE_API std::span<const SBuilderInfo> GetBuilders () noexcept;
// The row named Name (exact, case-sensitive: names are key content), or nullptr.
[[nodiscard]] FRT_CORE_API const SBuilderInfo* FindBuilder (FStringView Name) noexcept;

/**
 * The rule, as a pure function: local-only when the guard is on and the current source hash differs
 * from the committed one, with the current hash as the discriminator; otherwise the committed version
 * alone. Hashes are compared as text, ignoring ASCII case.
 */
[[nodiscard]] FRT_CORE_API SBuilderKeyVersion ResolveBuilderKeyVersion (const SGuid& Version, FStringView CommittedSourceHash,
	FStringView CurrentSourceHash, bool bGuardEnabled);

/**
 * The key version of the builder named Builder, for this build and the guard's current switch. The
 * first time a builder turns out local-only in this process it says so once in the log (BUILD003 is
 * information in the editor, §13.4). Callable from any thread: builders key their steps on workers.
 * A name that is not in the table is a programming error: it asserts, and the result is local-only
 * with an all-zero version, so nothing it builds is ever shared.
 */
[[nodiscard]] FRT_CORE_API SBuilderKeyVersion GetBuilderKeyVersion (FStringView Builder);

/**
 * Safety.BuilderSourceGuard (§4.8), on by default. Off, development builds key every builder by its
 * version GUID alone, so an edited builder can return stale values until its GUID is bumped; FRT.bat
 * check-builders still reports the missing bump. Config/Assets.yml sets it from M5 (the rules
 * config); tests set it here. Any thread.
 */
FRT_CORE_API void SetBuilderSourceGuardEnabled (bool bEnabled) noexcept;
[[nodiscard]] FRT_CORE_API bool IsBuilderSourceGuardEnabled () noexcept;
}
