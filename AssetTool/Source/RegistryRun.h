#pragma once

#include <memory>
#include <vector>

#include "Assets/AssetRegistry.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Registry/RegistryView.h"
#include "Assets/Vcs/VcsProviderSet.h"

#include "Commandlet.h"


namespace frt::assettool
{
/**
 * What a registry verb opens (RegistryCommandlets.cpp): a writer only while the host holds the mount's writer lock;
 * providers only for the verbs that ask the VCS (status names them, the meta verbs write and validate through them: edit
 * [55]), so the read verbs spawn no git. Every registry runs the meta lifecycle (SLifecycleDesc, design §4.7), so a verb
 * sees what gathering reports (META001 and the rest): the meta verbs (validate, fix, new-meta, migrate, resave) run it
 * themselves (bLifecycle: CAssetRepairs::RunNow with their own options); for every other verb OpenRegistry runs it once
 * after the initial scan, reporting only over every mount, with no provider but the ones bProviders asked for. scan
 * never writes a meta (edit [66]): only the meta verbs' lifecycle repairs.
 */
struct SRegistryOpenOptions
{
	bool bWriter = false;
	bool bProviders = false;     // detect the providers of the mounts (every mount; with bLifecycle the run's own)
	bool bLifecycle = false;     // the verb runs the lifecycle itself (RunNow); false: OpenRegistry runs it, reporting
	bool bRepairs = false;       // the lifecycle repairs (an owner's writing verb); false: it reports only
};

/**
 * The registry of one run, built over the run's mounts, started, waited for, and shut down when this goes (a writer
 * writes its snapshot then). The providers and the lifecycle desc outlive the registry, which keeps pointers to both.
 */
struct SRegistryRun
{
	std::shared_ptr<const assets::CMountTable> Mounts;
	std::unique_ptr<assets::CVcsProviderSet> Providers;
	std::unique_ptr<assets::SLifecycleDesc> Lifecycle;
	std::unique_ptr<assets::CAssetRegistry> Registry;
	// The run's own mount set: /Engine with --engine, else /Project and its plugins. The meta verbs act on these alone.
	std::vector<assets::MountId> OwnMounts;

	SRegistryRun () = default;
	SRegistryRun (const SRegistryRun&) = delete;
	SRegistryRun& operator= (const SRegistryRun&) = delete;
	~SRegistryRun ()
	{
		if (Registry)
		{
			Registry->ShutDown();
		}
	}

	[[nodiscard]] const assets::CRegistryView& View () const { return Registry->Live(); }
	// The mount's provider (null: none was detected for it, or None).
	[[nodiscard]] assets::IVcsProvider* FindProvider (assets::MountId Mount) const { return Providers ? Providers->Find(Mount) : nullptr; }
	// The root mount paths of OwnMounts ("/Project", "/Plugin.X").
	[[nodiscard]] std::vector<FString> GetOwnRoots () const;
};

// The mounts of the run: /Engine and, unless --engine, the project's /Project and plugins (Diagnostics: what Resolve
// reported). OpenRegistry's, and the host's for a roll-forward before the verb runs (AssetTool.cpp).
[[nodiscard]] assets::CMountTable ResolveRunMounts (const CCommandletContext& Context, std::vector<SDiagnostic>* Diagnostics = nullptr);

// Builds the registry of the run and waits for its initial scan. ExitSuccess, or the exit code to end with (reported).
int32 OpenRegistry (CCommandletContext& Context, FStringView Verb, const SRegistryOpenOptions& Options, SRegistryRun& Out);

// "/Engine, /Project, /Plugin.X".
[[nodiscard]] FString DescribeMounts (const assets::CMountTable& Mounts);

/**
 * A file argument as a mount path: one ("/Project/Textures/Rock.png"), or a path on disk, absolute or relative to the working
 * directory. nullopt, with the usage error reported (OutExit), when it names nothing in the mounts or names a mount root.
 */
[[nodiscard]] std::optional<FString> ResolveFileArgument (const CCommandletContext& Context, FStringView Verb, const assets::CMountTable& Mounts,
	FStringView Argument, int32& OutExit);
}
