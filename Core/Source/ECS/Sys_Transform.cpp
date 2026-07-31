#include "ECS/Sys_Transform.h"

#include "ECS/CoreComponents.h"
#include "ECS/World.h"


namespace frt
{
Sys_Transform::Sys_Transform(CWorld& InWorld)
	: World(InWorld)
{
}

void Sys_Transform::Finalize(const SUpdateContext& Context)
{
	(void)Context;
	Run();
}

void Sys_Transform::Run()
{
	CHierarchy& hierarchy = World.GetHierarchy();
	hierarchy.RefreshDepthOrder();

	TComponentPool<Comp_LocalTransform>& localPool = World.Pool<Comp_LocalTransform>();
	TComponentPool<Comp_WorldTransform>& worldPool = World.Pool<Comp_WorldTransform>();

	const TComponentPool<Comp_LocalTransform>& constLocalPool = localPool;
	const TComponentPool<Comp_WorldTransform>& constWorldPool = worldPool;

	const uint64 topologyVersion = hierarchy.GetTopologyVersion();

	// A reparent changes an entity's world transform without touching its local one, and
	// it shifts depths across a whole subtree. Rather than tracking which entities a
	// topology change affected, recompute everything - restructuring is rare, and being
	// wrong here means stale transforms that are very hard to trace back.
	const bool bForceAll = bInvalidated || topologyVersion != LastTopologyVersion;

	ResetChangedFlags(World.GetRecordCount());
	LastUpdatedCount = 0u;

	// ----- Pass 1: roots -----
	// Iterated const so that merely inspecting an entity does not mark its world
	// transform dirty; only the entities actually rewritten below bump the pool version,
	// which is what makes that version meaningful to bounds, culling and extraction.
	for (auto [id, local] : World.View<const Comp_LocalTransform>().Exclude<Comp_Parent>())
	{
		if (!worldPool.Contains(id))
		{
			continue;
		}

		if (!bForceAll && !constLocalPool.IsDirtySince(id, LastLocalVersion))
		{
			continue;
		}

		worldPool.Get(id) = Comp_WorldTransform::FromLocal(local);

		SetChanged(id);
		++LastUpdatedCount;
	}

	// ----- Pass 2: depth levels 1..N -----
	// The index covers only entities that have a parent, so roots are already done. Each
	// level is a contiguous range, and within a level entities are independent.
	const TArray<EntityId>& depthOrdered = hierarchy.GetDepthOrdered();

	for (uint16 depth = 1u; depth <= hierarchy.GetMaxDepth(); ++depth)
	{
		uint32 start = 0u;
		uint32 end = 0u;
		hierarchy.GetLevelRange(depth, start, end);

		for (uint32 i = start; i < end; ++i)
		{
			const EntityId id = depthOrdered[i];

			if (!worldPool.Contains(id) || !constLocalPool.Contains(id))
			{
				continue;
			}

			const EntityId parent = hierarchy.GetParent(id);

			// The parent sits at depth-1, so its world transform is already final and
			// its changed bit already set if it moved.
			const bool bParentMoved = IsChanged(parent);
			const bool bLocalMoved = bForceAll || constLocalPool.IsDirtySince(id, LastLocalVersion);

			if (!bParentMoved && !bLocalMoved)
			{
				continue;
			}

			const Comp_LocalTransform& local = constLocalPool.Get(id);
			const Comp_WorldTransform* parentWorld = constWorldPool.TryGet(parent);

			// A parent without a world transform of its own cannot contribute one, so the
			// child behaves as if it were a root rather than inheriting garbage.
			const Comp_WorldTransform composed = parentWorld != nullptr
				? Comp_WorldTransform::Compose(*parentWorld, local)
				: Comp_WorldTransform::FromLocal(local);

			worldPool.Get(id) = composed;

			SetChanged(id);
			++LastUpdatedCount;
		}
	}

	LastLocalVersion = constLocalPool.GetVersion();
	LastTopologyVersion = topologyVersion;
	bInvalidated = false;
}


void Sys_Transform::ResetChangedFlags(uint32 InEntityCount)
{
	const uint32 wordCount = (InEntityCount + 63u) / 64u;

	while (ChangedWords.Count() < wordCount)
	{
		ChangedWords.Add(0ull);
	}

	for (uint32 i = 0u; i < ChangedWords.Count(); ++i)
	{
		ChangedWords[i] = 0ull;
	}
}

void Sys_Transform::SetChanged(EntityId InEntity)
{
	const uint32 index = InEntity.GetIndex();
	const uint32 word = index / 64u;

	if (word >= ChangedWords.Count())
	{
		return;
	}

	ChangedWords[word] |= (1ull << (index % 64u));
}

bool Sys_Transform::IsChanged(EntityId InEntity) const
{
	if (!InEntity.IsValid())
	{
		return false;
	}

	const uint32 index = InEntity.GetIndex();
	const uint32 word = index / 64u;

	if (word >= ChangedWords.Count())
	{
		return false;
	}

	return (ChangedWords[word] & (1ull << (index % 64u))) != 0ull;
}
}
