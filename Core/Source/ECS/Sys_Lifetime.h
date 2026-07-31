#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "System.h"


namespace frt
{
class CWorld;
class CCommandBuffer;


/**
 * Counts down Comp_Lifetime and queues expired entities for destruction.
 *
 * The first system to make structural changes, and the reason CCommandBuffer exists: the
 * countdown runs inside a view over the lifetime pool, so destroying in place would
 * swap-and-pop the very array being iterated. Commands are recorded here and applied by
 * CWorldScene at the end of the Update phase.
 *
 * Update rather than Finalize - this is simulation, and everything downstream should see
 * the resulting entity set. Sys_Transform runs in Finalize, after the flush, so it never
 * composes a transform for an entity that expired this frame.
 */
class FRT_CORE_API Sys_Lifetime : public ISystem
{
public:
	Sys_Lifetime (CWorld& InWorld, CCommandBuffer& InCommands);

	SFlags<EUpdatePhase>& GetPhases () override { return Phases; }

	void Update (const SUpdateContext& Context) override;

	/** How many entities expired on the last run. For tests and profiling. */
	uint32 GetLastExpiredCount () const { return LastExpiredCount; }

private:
	CWorld&         World;
	CCommandBuffer& Commands;

	SFlags<EUpdatePhase> Phases = EUpdatePhase::Update;

	uint32 LastExpiredCount = 0u;
};
}
