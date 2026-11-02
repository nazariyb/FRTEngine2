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

// <MountRoot>/Local/Assets/OpJournal.log: the operation journal of the root's multi-file commands (design §4.6;
// Assets/Meta/OpJournal.h). One per owner root, because a project and /Engine are separate roots; only the writer-lock
// holder writes it, and it is empty whenever no operation is in flight.
[[nodiscard]] inline std::filesystem::path GetOpJournalPath (const std::filesystem::path& MountRoot)
{
	return GetMountAssetsLocalDir(MountRoot) / "OpJournal.log";
}

// <MountRoot>/Local/Assets/ProjectId: the owner root's random id, generated once (design §5.3), which every
// registry snapshot and journal of that root carries so a file copied from elsewhere is not taken for its own
// (Assets/Registry/Snapshot.h ReadProjectId, ReadOrCreateProjectId). Machine-local, never in the .frtproject.
[[nodiscard]] inline std::filesystem::path GetProjectIdPath (const std::filesystem::path& MountRoot)
{
	return GetMountAssetsLocalDir(MountRoot) / "ProjectId";
}

// <EngineDir>/Local/Registry: the engine's registry snapshot and its journal (design §3.3 layout, §5.3). Apart from
// Local/Assets so that every project's editor reads it and only the engine's owner (FRT.bat assets scan --engine in
// M3) writes it.
[[nodiscard]] inline std::filesystem::path GetEngineRegistryDir (const std::filesystem::path& EngineDir)
{
	return EngineDir / "Local" / "Registry";
}

// <EngineDir>/Local/Registry/Engine.snapshot: /Engine's records (Assets/Registry/Snapshot.h); its journal is
// GetJournalPath of it. Only a holder of <EngineDir>/Local/Assets/Owner.lock writes either.
[[nodiscard]] inline std::filesystem::path GetEngineSnapshotPath (const std::filesystem::path& EngineDir)
{
	return GetEngineRegistryDir(EngineDir) / "Engine.snapshot";
}

// <ProjectDir>/Local/Assets/AssetRegistry.snapshot: /Project's and its plugins' records. Only the holder of the
// project's Owner.lock writes it and its journal.
[[nodiscard]] inline std::filesystem::path GetRegistrySnapshotPath (const std::filesystem::path& ProjectDir)
{
	return GetMountAssetsLocalDir(ProjectDir) / "AssetRegistry.snapshot";
}
}
