#pragma once

#include <new>
#include <utility>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "ECS/EntityId.h"
#include "ECS/World.h"
#include "Memory/Memory.h"


namespace frt
{
/**
 * Records structural changes so they can be applied at a safe point instead of in the
 * middle of iteration.
 *
 * Adding or removing a component reorders a pool's dense arrays, and destroying an entity
 * touches every pool. A view holds raw pool pointers and a dense index, so doing any of
 * that mid-loop invalidates the loop. Queue it here and Flush() once the phase is done.
 *
 *     for (auto [id, lifetime] : World.View<Comp_Lifetime>())
 *     {
 *         if (lifetime.Remaining <= 0.0f)
 *         {
 *             Commands.Destroy(id);   // safe: nothing has moved yet
 *         }
 *     }
 *
 *     Commands.Flush(World);
 *
 * Spawning is deliberately absent. CWorld::Spawn touches only the entity records and the
 * free list, never a pool, so it is already safe to call during iteration - the newly
 * spawned entity simply has no components until its queued Add commands are applied.
 *
 * Commands apply in the order they were recorded. Queuing an Add after a Destroy for the
 * same entity is not an error: the Add is skipped, because by then the handle no longer
 * refers to a live entity.
 *
 * NOT thread-safe, by design. Under multithreading each worker gets its own buffer and
 * they are merged at the phase barrier, which is also where the merge order has to become
 * deterministic - by worker index and sequence, not arrival - so that identical inputs
 * produce identical results.
 */
class FRT_CORE_API CCommandBuffer
{
public:
	CCommandBuffer () = default;
	~CCommandBuffer ();

	CCommandBuffer (const CCommandBuffer&) = delete;
	CCommandBuffer& operator= (const CCommandBuffer&) = delete;


	void Destroy (EntityId InEntity);

	/** The component is constructed now and moved into its pool at Flush. */
	template <class T, class... TArgs>
	void Add (EntityId InEntity, TArgs&&... InArgs)
	{
		void* payload = AllocatePayload(static_cast<uint32>(sizeof(T)), static_cast<uint32>(alignof(T)));
		new (payload) T{ std::forward<TArgs>(InArgs)... };

		SCommand& command = Commands.Add();
		command.Entity = InEntity;
		command.Payload = payload;

		command.Apply = [](CWorld& InWorld, EntityId InTarget, void* InPayload)
		{
			// The entity may have been destroyed by an earlier command in this same flush.
			if (InWorld.IsAlive(InTarget))
			{
				InWorld.Add<T>(InTarget, std::move(*static_cast<T*>(InPayload)));
			}
		};

		// Runs even when Apply skipped the command, and after Apply moved out of it - a
		// moved-from object still has to be destroyed.
		command.DestroyPayload = [](void* InPayload)
		{
			static_cast<T*>(InPayload)->~T();
		};
	}

	template <class T>
	void Remove (EntityId InEntity)
	{
		SCommand& command = Commands.Add();
		command.Entity = InEntity;

		command.Apply = [](CWorld& InWorld, EntityId InTarget, void*)
		{
			if (InWorld.IsAlive(InTarget))
			{
				InWorld.Remove<T>(InTarget);
			}
		};
	}


	/** Applies every queued command in order, then clears. */
	void Flush (CWorld& InWorld);

	/** Discards without applying. Queued Add payloads are still destroyed. */
	void Clear ();

	bool   IsEmpty () const { return Commands.IsEmpty(); }
	uint32 Count () const   { return Commands.Count(); }

private:
	struct SCommand
	{
		EntityId Entity  = InvalidEntity;
		void*    Payload = nullptr;

		void (*Apply)(CWorld&, EntityId, void*) = nullptr;
		void (*DestroyPayload)(void*) = nullptr;
	};

	/**
	 * Payloads live in chunks that are never relocated, so a recorded pointer stays valid
	 * however many more commands are queued afterwards. Growing a single flat buffer would
	 * mean raw-moving whatever the payloads contain, which is exactly the assumption a
	 * non-trivially-copyable component breaks.
	 */
	void* AllocatePayload (uint32 InSize, uint32 InAlignment);

	void DestroyPayloads ();
	void FreeChunks ();

	static constexpr uint32 ChunkSize = 4096u;

#pragma warning(push)
#pragma warning(disable: 4251)
	TArray<SCommand> Commands;
	TArray<uint8*>   Chunks;
#pragma warning(pop)

	uint32 CurrentChunk     = 0u;
	uint32 CurrentChunkUsed = 0u;
};
}
