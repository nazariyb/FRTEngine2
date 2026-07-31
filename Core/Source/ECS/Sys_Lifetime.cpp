#include "ECS/Sys_Lifetime.h"

#include "ECS/CommandBuffer.h"
#include "ECS/Comp_Lifetime.h"
#include "ECS/World.h"


namespace frt
{
Sys_Lifetime::Sys_Lifetime (CWorld& InWorld, CCommandBuffer& InCommands)
	: World(InWorld)
	, Commands(InCommands)
{
}

void Sys_Lifetime::Update (const SUpdateContext& Context)
{
	LastExpiredCount = 0u;

	const Real delta = static_cast<Real>(Context.DeltaSeconds);

	// Non-const view: the countdown writes, so this correctly marks the pool changed.
	for (auto [id, lifetime] : World.View<Comp_Lifetime>())
	{
		lifetime.Remaining -= delta;

		if (lifetime.Remaining <= Real(0))
		{
			// Queued, not destroyed: Destroy strips every pool including this one, which
			// would swap-and-pop the array this loop is walking.
			Commands.Destroy(id);
			++LastExpiredCount;
		}
	}
}
}
