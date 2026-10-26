#pragma once

#include <filesystem>


namespace frt::assets
{
/**
 * <MountRoot>/Local/Assets: where a mount's machine-local asset state lives (design §3.3) - its writer
 * lock (Owner.lock, §5.9), BuildState.log and BuildJournal.log (§7.4, §7.9). MountRoot is the mount's
 * root: a project's directory for /Project, the engine checkout for /Engine. The one place that spells
 * the location: every file in it is named from here (GetOwnerLockPath, GetBuildStatePath,
 * GetBuildJournalPath), so a caller holding a mount root passes it as it is.
 */
[[nodiscard]] inline std::filesystem::path GetMountAssetsLocalDir (const std::filesystem::path& MountRoot)
{
	return MountRoot / "Local" / "Assets";
}

// <MountRoot>/Local/Assets/Rules.cache: every rule file the process sees, flattened to JSON lines for hooks, the
// packager and Lua (design §3.5, §3.5.4; Assets/Pipeline/Rules.h WriteRulesCache). Only the writer-lock holder writes it.
[[nodiscard]] inline std::filesystem::path GetRulesCachePath (const std::filesystem::path& MountRoot)
{
	return GetMountAssetsLocalDir(MountRoot) / "Rules.cache";
}
}
