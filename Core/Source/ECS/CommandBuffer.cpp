#include "ECS/CommandBuffer.h"


namespace frt
{
CCommandBuffer::~CCommandBuffer ()
{
	DestroyPayloads();
	FreeChunks();
}

void CCommandBuffer::Destroy (EntityId InEntity)
{
	SCommand& command = Commands.Add();
	command.Entity = InEntity;

	// CWorld::Destroy already ignores a handle that is not alive, so a duplicate Destroy
	// or one following another command's destruction needs no guard here.
	command.Apply = [](CWorld& InWorld, EntityId InTarget, void*)
	{
		InWorld.Destroy(InTarget);
	};
}

void CCommandBuffer::Flush (CWorld& InWorld)
{
	// Indexed rather than range-based: applying a command can reallocate nothing here,
	// but recording during a flush would, and this makes that read as a bug rather than
	// silently iterating a stale pointer.
	for (uint32 i = 0u; i < Commands.Count(); ++i)
	{
		const SCommand& command = Commands[i];
		command.Apply(InWorld, command.Entity, command.Payload);
	}

	Clear();
}

void CCommandBuffer::Clear ()
{
	DestroyPayloads();
	Commands.Clear();

	// Chunks are kept, not freed: a command buffer is flushed every frame and would
	// otherwise churn the same allocations forever.
	CurrentChunk = 0u;
	CurrentChunkUsed = 0u;
}

void* CCommandBuffer::AllocatePayload (uint32 InSize, uint32 InAlignment)
{
	frt_assert(InSize <= ChunkSize);

	if (Chunks.IsEmpty())
	{
		Chunks.Add(static_cast<uint8*>(memory::NewUnmanaged(ChunkSize)));
		frt_assert(Chunks.Last() != nullptr);
	}

	uint8* chunk = Chunks[CurrentChunk];
	uint64 aligned = memory::AlignAddress(reinterpret_cast<uint64>(chunk) + CurrentChunkUsed, InAlignment);
	uint32 offset = static_cast<uint32>(aligned - reinterpret_cast<uint64>(chunk));

	if (offset + InSize > ChunkSize)
	{
		++CurrentChunk;

		if (CurrentChunk >= Chunks.Count())
		{
			Chunks.Add(static_cast<uint8*>(memory::NewUnmanaged(ChunkSize)));
			frt_assert(Chunks.Last() != nullptr);
		}

		chunk = Chunks[CurrentChunk];
		aligned = memory::AlignAddress(reinterpret_cast<uint64>(chunk), InAlignment);
		offset = static_cast<uint32>(aligned - reinterpret_cast<uint64>(chunk));
	}

	CurrentChunkUsed = offset + InSize;

	return chunk + offset;
}

void CCommandBuffer::DestroyPayloads ()
{
	for (uint32 i = 0u; i < Commands.Count(); ++i)
	{
		const SCommand& command = Commands[i];
		if (command.DestroyPayload != nullptr && command.Payload != nullptr)
		{
			command.DestroyPayload(command.Payload);
		}
	}
}

void CCommandBuffer::FreeChunks ()
{
	for (uint32 i = 0u; i < Chunks.Count(); ++i)
	{
		memory::DestroyUnmanaged(Chunks[i]);
	}

	Chunks.Clear();
	CurrentChunk = 0u;
	CurrentChunkUsed = 0u;
}
}
