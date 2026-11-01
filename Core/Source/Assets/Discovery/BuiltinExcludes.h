#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * The names no mount ever registers, whatever its rules say (design §3.5.2, membership step 2): editor and
 * OS litter, backups and half-written saves of the DCC tools, FRT's own sidecars and temporary files. They
 * are excluded before any rule is consulted and no rule can re-include them, so a scan never records
 * them, never reads their bytes and never reports them as unclaimed.
 *
 *	dot-names          ".git", ".vs", ".frt-fence-3", ".<name>.<pid>-<n>.tmp" (WriteFileAtomic): files and directories
 *	*~  ~$*            editor backups (Krita's *.kra~ included), Office lock files
 *	*.tmp  *.orig      temporary files, merge leftovers
 *	*.blend[0-9]  *.blend[0-9][0-9]  *.blend@     Blender's numbered backups and its save in progress
 *	*-autosave.kra     Krita's visible autosaves
 *	Thumbs.db  desktop.ini                         Explorer's files
 *	*.meta             asset metadata (M4): the sidecar of a source, never an asset of its own
 *	Assets.yml         a folder rule file (§3.5.4): the scanner reports it as the folder's rule file instead
 *
 * Only dot-names exclude a directory; every other pattern names files (a folder called "Old.tmp" is
 * walked). Names compare under the portable fold (Unicode simple case folding, as every glob does,
 * §3.5.2), so "THUMBS.DB" and "Rock.TMP" are excluded on every host. A name is one path segment as
 * listed (WTF-8), never a path. The rules engine reuses this for its built-in excludes, and the default
 * scan membership (MountScanner.h) applies it alone.
 */
namespace frt::assets
{
[[nodiscard]] FRT_CORE_API bool IsBuiltinExcludedName (FStringView Name, bool bDirectory) noexcept;

// The reserved name of a folder rule file. A listing entry is the folder's rule file when its name equals
// this under the portable fold (IsRuleFileName).
inline constexpr FStringLiteral RuleFileName = "Assets.yml";

[[nodiscard]] inline bool IsRuleFileName (FStringView Name) noexcept
{
	return Name.Equals<ESearchCase::IgnoreCase>(RuleFileName);
}

/**
 * Which of a folder's rule-file spellings is its rule file (a case-sensitive folder may hold "ASSETS.YML" and
 * "assets.yml"): whether Candidate (an IsRuleFileName entry of the listing) replaces Current (the pick so far;
 * empty: none yet). The reserved spelling always wins; otherwise the first under SStringLessIgnoreCase, ties by
 * bytes. Any listing order gives the same pick, so the scanner and the rules engine's ancestor walk, which both
 * decide with this, always bind the same file.
 */
[[nodiscard]] FRT_CORE_API bool IsPreferredRuleFileName (FStringView Candidate, FStringView Current) noexcept;
}
