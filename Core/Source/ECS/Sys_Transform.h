#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/EntityId.h"
#include "System.h"


namespace frt
{
class CWorld;


/**
 * Composes Comp_LocalTransform into Comp_WorldTransform, honouring parent hierarchies.
 *
 * Two passes. Roots (no Comp_Parent) are a flat, order-independent sweep. Everything else
 * is walked in depth order, so a parent's world transform is always final before any of
 * its children compose against it - that invariant is the whole reason CHierarchy keeps a
 * depth-ordered index.
 *
 * The system is internally sequenced and must stay one scheduler node. It both reads and
 * writes Comp_WorldTransform (children read their parents'), which is only safe because
 * of the depth ordering - a scheduler cannot infer that, so it must not try to split this
 * into parallel pieces. Within a single depth level entities ARE independent, which is
 * where parallelism will go later.
 *
 * Work is skipped where nothing changed. An entity recomputes only if its own local
 * transform was touched, or its parent's world transform changed this frame. Local
 * changes are detected at the pool's block granularity, so the skip is conservative -
 * it can do redundant work, never miss required work.
 */
class FRT_CORE_API Sys_Transform : public ISystem
{
public:
	explicit Sys_Transform(CWorld& InWorld);

	SFlags<EUpdatePhase>& GetPhases() override { return Phases; }

	void Finalize(const SUpdateContext& Context) override;

	/** Runs a pass directly. Finalize() is this plus the phase plumbing. */
	void Run();

	/** Forces the next run to recompute everything, ignoring change tracking. */
	void Invalidate() { bInvalidated = true; }

	/** How many world transforms the last run actually wrote. For tests and profiling. */
	uint32 GetLastUpdatedCount() const { return LastUpdatedCount; }

private:
	void ResetChangedFlags(uint32 InEntityCount);
	void SetChanged(EntityId InEntity);
	bool IsChanged(EntityId InEntity) const;

	CWorld& World;

	// Exact per-entity record of which world transforms changed this frame.
	//
	// The pool's block versions are too coarse here: dirtiness propagates down a
	// hierarchy, and at 64-entity block granularity one changed entity would drag its
	// whole block's subtrees along with it. One bit per entity is ~128 KiB at the full
	// 20-bit index space and makes the propagation exact.
#pragma warning(push)
#pragma warning(disable: 4251)
	TArray<uint64> ChangedWords;
#pragma warning(pop)

	// Finalize, not Update: any Update-phase system may write Comp_LocalTransform, so
	// reading them all at the end avoids declaring an ordering against each one.
	SFlags<EUpdatePhase> Phases = EUpdatePhase::Finalize;

	uint64 LastLocalVersion    = 0ull;
	uint64 LastTopologyVersion = 0ull;
	uint32 LastUpdatedCount    = 0u;
	bool   bInvalidated        = true;
};
}
