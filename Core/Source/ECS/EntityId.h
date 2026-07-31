#pragma once

#include "Core.h"
#include "CoreTypes.h"


namespace frt
{
/**
 * Opaque entity handle: an index into the world's entity records plus a generation
 * counter, packed into one word.
 *
 * A struct rather than a using-alias so a raw uint32 cannot be passed where a handle is
 * expected. Still exactly 4 bytes, so the record array stays half the size of a
 * two-field handle and comparison is a single-word test.
 *
 * All packing lives behind the accessors, so widening to 64 bits (32 index /
 * 32 generation) is a change to this header alone. That matters: 12 generation bits
 * wrap after 4096 reuses of a slot. At ~5000 destroys/second across ~20000 recycled
 * slots a slot returns every ~4 seconds, so wrap is roughly 4.5 hours of continuous
 * running. Fine for a renderer, marginal for a multiplayer server that ships handles
 * over the wire.
 */
struct EntityId
{
	static constexpr uint32 IndexBits      = 20u;
	static constexpr uint32 GenerationBits = 12u;

	static_assert(IndexBits + GenerationBits == 32u, "EntityId bit budget must be exact");

	static constexpr uint32 IndexMask      = (1u << IndexBits) - 1u;
	static constexpr uint32 GenerationMask = (1u << GenerationBits) - 1u;

	/** Top index is reserved for the invalid handle, hence the -1. */
	static constexpr uint32 MaxIndex      = IndexMask - 1u;
	static constexpr uint32 MaxGeneration = GenerationMask;


	uint32 Value = ~0u;


	constexpr EntityId() = default;

	constexpr explicit EntityId(uint32 InRawValue)
		: Value(InRawValue) {}

	constexpr EntityId(uint32 InIndex, uint32 InGeneration)
		: Value((InIndex & IndexMask) | ((InGeneration & GenerationMask) << IndexBits)) {}


	constexpr uint32 GetIndex() const      { return Value & IndexMask; }
	constexpr uint32 GetGeneration() const { return (Value >> IndexBits) & GenerationMask; }

	constexpr bool IsValid() const { return GetIndex() <= MaxIndex; }

	constexpr bool operator==(const EntityId& Rhs) const { return Value == Rhs.Value; }
	constexpr bool operator!=(const EntityId& Rhs) const { return Value != Rhs.Value; }

	/** Ordering is by raw value - only meaningful for sorting and as a map key. */
	constexpr bool operator<(const EntityId& Rhs) const { return Value < Rhs.Value; }
};


inline constexpr EntityId InvalidEntity = EntityId(~0u);

static_assert(sizeof(EntityId) == 4u, "EntityId must stay one word");
static_assert(!InvalidEntity.IsValid(), "InvalidEntity must not alias a usable index");
}
