#include "Assets/Vcs/VcsProviderSet.h"

#include <mutex>

#include "Assets/Discovery/Mounts.h"
#include "Jobs/WorkerPool.h"


namespace frt::assets
{
// Shared with the detection tasks, so a task that finishes after the set has gone writes into nothing that is freed.
struct CVcsProviderSet::SState
{
	mutable std::mutex Mutex;
	std::vector<std::pair<MountId, std::unique_ptr<IVcsProvider>>> Providers;
};

namespace
{
[[nodiscard]] std::vector<MountId> GetIds (const CMountTable& Mounts, std::span<const MountId> Ids)
{
	if (!Ids.empty())
	{
		return std::vector<MountId>(Ids.begin(), Ids.end());
	}
	std::vector<MountId> all;
	for (const SMount& mount : Mounts.GetMounts())
	{
		all.push_back(mount.Id);
	}
	return all;
}
}


CVcsProviderSet::CVcsProviderSet ()
	: State(std::make_shared<SState>())
{}

CVcsProviderSet::~CVcsProviderSet () = default;

void CVcsProviderSet::DetectNow (const CMountTable& Mounts, std::span<const MountId> Ids, FStringView Setting)
{
	for (const MountId id : GetIds(Mounts, Ids))
	{
		const SMount* mount = Mounts.Find(id);
		if (mount == nullptr)
		{
			continue;
		}
		std::unique_ptr<IVcsProvider> provider = MakeVcsProvider(Setting, FString::FromPath(mount->ContentDir));
		const std::lock_guard lock(State->Mutex);
		State->Providers.emplace_back(id, std::move(provider));
	}
}

void CVcsProviderSet::DetectAsync (jobs::CWorkerPool& Pool, const CMountTable& Mounts, std::span<const MountId> Ids, FStringView Setting,
	TUniqueFunction<void (MountId, IVcsProvider*)> OnDetected)
{
	auto callback = std::make_shared<TUniqueFunction<void (MountId, IVcsProvider*)>>(std::move(OnDetected));
	for (const MountId id : GetIds(Mounts, Ids))
	{
		const SMount* mount = Mounts.Find(id);
		if (mount == nullptr)
		{
			continue;
		}
		auto found = std::make_shared<std::unique_ptr<IVcsProvider>>();
		Pool.Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup,
			[found, setting = FString(Setting), root = FString::FromPath(mount->ContentDir)] ()
			{
				*found = MakeVcsProvider(setting, root);
			},
			[state = State, found, id, callback] ()
			{
				IVcsProvider* provider = found->get();
				{
					const std::lock_guard lock(state->Mutex);
					state->Providers.emplace_back(id, std::move(*found));
				}
				if (*callback)
				{
					(*callback)(id, provider);
				}
			});
	}
}

IVcsProvider* CVcsProviderSet::Find (MountId Mount) const noexcept
{
	const std::lock_guard lock(State->Mutex);
	for (const auto& [id, provider] : State->Providers)
	{
		if (id == Mount)
		{
			return provider.get();
		}
	}
	return nullptr;
}

std::vector<std::pair<MountId, IVcsProvider*>> CVcsProviderSet::GetProviders () const
{
	const std::lock_guard lock(State->Mutex);
	std::vector<std::pair<MountId, IVcsProvider*>> providers;
	for (const auto& [id, provider] : State->Providers)
	{
		providers.emplace_back(id, provider.get());
	}
	return providers;
}
}
