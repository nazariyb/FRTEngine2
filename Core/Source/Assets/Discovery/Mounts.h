#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Platform/FileSystem.h"


/**
 * The mounts of a process (design §3.1, §5.4 step 1): /Engine, /Project and one /Plugin.<Name> for every
 * plugin, mod or DLC under development in <project>/Plugins/<Name>/. They follow from the layout, never
 * from a configured search path, and they are disjoint: no search order, no overlay, so a mount path names
 * at most one file. CMountTable is resolved once at start-up and immutable afterwards, so any thread may
 * read it; it is the one place that turns an absolute path into a mount path and back (paths::ToMountPath
 * and paths::FromMountPath in EnginePaths.h are thin wrappers over the table the registry installs).
 *
 * Case. Each mount root's volume facts are queried once, at Resolve (platform::QueryVolumeInfo): whether
 * lookups inside it are case-sensitive decides how the registry's FILES index compares (§5.2) and how a
 * scan matches names below the root; in M3 the rule is per mount root, not per directory. The root's own
 * prefix (its ancestors and its own name, looked up by their parents' rule) always matches by NTFS's
 * rule (ToMountPath). The remote and cloud bits are recorded for the warning of §3.1, which nothing acts
 * on yet.
 *
 * Spelling. A mount path's relative part is the on-disk spelling (canonical WTF-8, '/' separated), never
 * folded: the registry folds for lookups and keeps the spelling for display and for the provisional id.
 */
namespace frt::assets
{
/**
 * A name or mount path as spelled on disk, made fit to show: each WTF-8 surrogate (the 3-byte form of a
 * lone UTF-16 surrogate, which Windows allows in names and EnumerateDirectory keeps) becomes one U+FFFD,
 * and any other ill-formed sequence what FString::FromUtf8Lossy makes of it. Valid UTF-8 comes back as
 * it is. For diagnostics' Subject and Message (§13.4: a non-UTF-8 name is PATH002); never for a key.
 */
[[nodiscard]] FRT_CORE_API FString ToDisplayText (FStringView OnDiskSpelling);

enum class EMountKind : uint8 { Engine, Project, Plugin };

/**
 * One mount. RulesFile is its rule file, which may not exist: <engine config dir>/Assets.yml for /Engine,
 * <project>/Config/Assets.yml for /Project, <project>/Plugins/<Name>/Config/Assets.yml for a plugin.
 * OwnerRoot is the directory whose Local/Assets holds the mount's machine-local state and lock
 * (GetMountAssetsLocalDir) and against which diagnostics name rule files ("Core/Config/Assets.yml",
 * "Plugins/X/Config/Assets.yml"): the engine checkout for /Engine, the project directory for /Project and
 * for every plugin. Every path is absolute and normalized (platform::NormalizePath).
 */
struct SMount
{
	FString Name;                       // "Engine", "Project", "Plugin.<Name>" (the plugin folder's spelling)
	MountId Id = InvalidMountId;
	EMountKind Kind = EMountKind::Engine;
	std::filesystem::path ContentDir;   // the mount root
	std::filesystem::path RulesFile;
	std::filesystem::path OwnerRoot;
	platform::SVolumeInfo Volume;       // of ContentDir (defaults when it does not exist)
	// ContentDir relative to OwnerRoot, '/' separated ("Core/Content", "Content", "Plugins/X/Content"; ContentDir's
	// own name when it does not lie below OwnerRoot): the head of a diagnostic's File for anything in the mount.
	FString ContentPrefix;

	// "/Engine", "/Project", "/Plugin.<Name>".
	[[nodiscard]] FString GetRootMountPath () const { return JoinMountPath(Name, FStringView()); }
	/**
	 * The absolute path of a relative part of this mount's paths (what SplitMountPath gives; empty = the root), or
	 * nullopt when it cannot name something inside the mount: an empty, "." or ".." segment, a '\' or ':' (which
	 * Windows reads as a separator or a drive, so "..\x" or "C:x" would leave the mount), a NUL, or text that is
	 * not canonical WTF-8. The one mount-path-to-disk conversion: CMountTable::FromMountPath, the importers' input
	 * and the rules engine use it.
	 */
	[[nodiscard]] FRT_CORE_API std::optional<std::filesystem::path> ToAbsolute (FStringView Relative) const;
	// The File of a diagnostic about Relative in this mount (§13.4 convention): ContentPrefix + "/" + Relative.
	[[nodiscard]] FRT_CORE_API FString GetSourceFile (FStringView Relative) const;
};

// Where the mounts come from. EngineDir is /Engine's OwnerRoot (the checkout, or a package's Engine/).
struct SMountRoots
{
	std::filesystem::path EngineDir;
	std::filesystem::path EngineContentDir;   // paths::GetEngineContentDir()
	std::filesystem::path EngineConfigDir;    // paths::GetEngineConfigPath().parent_path()
	std::filesystem::path ProjectDir;         // paths::GetProjectDir(); empty: no project, so no /Project and no plugins
};

class FRT_CORE_API CMountTable
{
public:
	/**
	 * The mounts of these roots, ids per the MountId rule (RegistryTypes.h): /Engine = 0, /Project = 1,
	 * then the plugins from 2 in SStringLessIgnoreCase order of their names, ties by bytes. /Engine is
	 * always mounted and /Project whenever ProjectDir is given, even when their content directory does not
	 * exist (a warning then: the mount is empty); a plugin only when <project>/Plugins/<Name>/Content is a
	 * directory. Plugin folders with a dot-name are skipped. One whose name is not portable
	 * (GetNonPortableNameReason: not valid UTF-8, a trailing dot or space, a device name, ...) is never
	 * mounted, Content or not, and reported as PATH002: a mount name must be text, and Win32 would look up
	 * "Foo./Content" as "Foo/Content". Plugin folders equal under the portable fold ("Foo" and "foo" in a
	 * case-sensitive Plugins folder; "Kelvin" and U+212A KELVIN SIGN + "elvin" on plain NTFS) are all
	 * mounted, each reported as PATH001: Find by a typed name reaches only the first. Problems go to *Out
	 * (when given): those, and warnings with code 0 for a missing content directory or an unlistable
	 * Plugins folder; Subject is the mount's root mount path. Reads the disk: one listing of Plugins/ and a
	 * stat and volume query per mount.
	 */
	[[nodiscard]] static CMountTable Resolve (const SMountRoots& Roots, std::vector<SDiagnostic>* Out);
	/**
	 * The same, with /Engine's OwnerRoot found from the two engine directories: paths::GetEngineDir() when
	 * both lie at or below it (every checkout and package of this engine), else their deepest common
	 * directory (a generated tree: <root>/Content and <root>/Config give <root>).
	 */
	[[nodiscard]] static CMountTable Resolve (const std::filesystem::path& EngineContentDir, const std::filesystem::path& EngineConfigDir,
		const std::filesystem::path& ProjectDir, std::vector<SDiagnostic>* Out);

	// Every mount, in id order: GetMounts()[Id].Id == Id.
	[[nodiscard]] std::span<const SMount> GetMounts () const noexcept { return Mounts; }
	// By name: the byte-exact spelling first, else the one equal under the portable fold (mount names are
	// typed by people, in queries and rule files). nullptr when none.
	[[nodiscard]] const SMount* Find (FStringView Name) const noexcept;
	[[nodiscard]] const SMount* Find (MountId Id) const noexcept;

	/**
	 * The mount path of an absolute path at or below a mount root ("/Project/Textures/Rock.png"; a root
	 * gives "/Project"), or nullopt when it is in no mount. The path is normalized first; the root prefix
	 * matches by NTFS's upcase comparison whatever the mount's own rule, since the directories above a
	 * mount root are not the mount's ("p:\" against "P:\", or a case-sensitive Content under a
	 * case-insensitive parent, still match); the remainder keeps the path's own spelling - it is not looked
	 * up on disk, so pass a listed path when the spelling matters (the registry keys by it). Lexical: links
	 * are not followed.
	 */
	[[nodiscard]] std::optional<FString> ToMountPath (const std::filesystem::path& Absolute) const;
	/**
	 * The absolute path of a file or folder mount path, or nullopt when it is not well-formed (no leading
	 * '/', a trailing '/', an empty, "." or ".." segment: IsWellFormedMountPath), names no mount, holds a
	 * '\' or ':' (which Windows would read as a separator or a drive and so leave the mount), or is not
	 * canonical WTF-8. The mount name matches as Find does; the relative part is used as spelled.
	 */
	[[nodiscard]] std::optional<std::filesystem::path> FromMountPath (FStringView MountPath) const;
	// The mount whose root is at or above Absolute (by NTFS's rule: the longest root, then a byte-exact
	// one between roots equal under the rule); nullptr when none.
	[[nodiscard]] const SMount* FindContaining (const std::filesystem::path& Absolute) const;

	template <platform::NarrowText T>
	std::optional<FString> ToMountPath (const T&) const = delete;

private:
	std::vector<SMount> Mounts;
};
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EMountKind,
	FRT_ENUM_ENTRY(frt::assets::EMountKind, Engine),
	FRT_ENUM_ENTRY(frt::assets::EMountKind, Project),
	FRT_ENUM_ENTRY(frt::assets::EMountKind, Plugin));
