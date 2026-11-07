#pragma once

#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Functional/UniqueFunction.h"


namespace frt::jobs
{
class CWorkerPool;
}


/**
 * One provider per mount (design §12.9 Detection), found once at start-up from the per-user `[AssetsEditor]
 * VcsProvider=Auto|None|Git` (MakeVcsProvider) and owned here for as long as the registry that uses them. Detect spawns git,
 * so a live host detects on a worker (DetectAsync) while its snapshot loads and hands each provider to the registry as it
 * arrives (CAssetRepairs::SetProvider, with SLifecycleDesc::bProvidersPending), and a tool host detects at once (DetectNow)
 * before it opens its registry. Hosts create providers only where they are used: a read-only verb or a scripted run makes
 * none, and spawns no git (edit [55]).
 */
namespace frt::assets
{
class CMountTable;

class FRT_CORE_API CVcsProviderSet
{
public:
	CVcsProviderSet ();
	~CVcsProviderSet ();
	CVcsProviderSet (const CVcsProviderSet&) = delete;
	CVcsProviderSet& operator= (const CVcsProviderSet&) = delete;

	// Detects the provider of each of Ids (every mount when empty) on the calling thread, from its content directory.
	void DetectNow (const CMountTable& Mounts, std::span<const MountId> Ids, FStringView Setting);
	/**
	 * The same, one task per mount on Pool. OnDetected runs on the owner thread once per mount, as its provider arrives (never
	 * null: None is a provider too). The set must outlive the completions, which a pool's drain discards unrun.
	 */
	void DetectAsync (jobs::CWorkerPool& Pool, const CMountTable& Mounts, std::span<const MountId> Ids, FStringView Setting,
		TUniqueFunction<void (MountId, IVcsProvider*)> OnDetected);

	// The mount's provider; null while it is being detected, or for a mount not asked for.
	[[nodiscard]] IVcsProvider* Find (MountId Mount) const noexcept;
	// Every provider found so far, as SLifecycleDesc::Providers and SAssetWriterDesc::Providers take them.
	[[nodiscard]] std::vector<std::pair<MountId, IVcsProvider*>> GetProviders () const;

private:
	struct SState;
	std::shared_ptr<SState> State;
};
}
