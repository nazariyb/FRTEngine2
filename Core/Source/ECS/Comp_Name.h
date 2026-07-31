#pragma once

#include <cstring>
#include <string_view>

#include "CoreTypes.h"
#include "ECS/ComponentRegistry.h"


namespace frt
{
/**
 * Display name, inline rather than heap-allocated.
 *
 * Nearly every entity ends up with a name, so this is one of the few components whose
 * per-entity cost is paid across the whole world rather than a subset. A std::string here
 * would mean an allocation per entity, a pointer chase to read it, and a component that
 * cannot be memcpy'd for serialization or replication - for a debug label that is a poor
 * trade. A fixed buffer keeps the component trivially copyable and the pool contiguous.
 *
 * Capacity is the whole struct: 32 bytes, two per cache line, 6.4 MB across 200k entities.
 * Raising it is a one-line change, and multiplies straight through by the entity count.
 *
 * Names longer than the buffer are TRUNCATED, not rejected. This is a label for editors
 * and logs; nothing keys off it, and failing a spawn over a long name would be worse than
 * showing a shortened one. If a stable identity is ever needed, that wants a GUID rather
 * than a wider string.
 */
struct Comp_Name
{
	/** Includes the null terminator, so the longest usable name is Capacity - 1. */
	static constexpr uint32 Capacity = 32u;

	char Value[Capacity] = {};


	Comp_Name () = default;

	explicit Comp_Name (std::string_view InName) { Set(InName); }


	void Set (std::string_view InName)
	{
		const size_t count = InName.size() < (Capacity - 1u) ? InName.size() : (Capacity - 1u);

		if (count > 0u)
		{
			std::memcpy(Value, InName.data(), count);
		}

		Value[count] = '\0';
	}

	/** Always null-terminated, so it is safe to hand to printf and ImGui directly. */
	const char* Get () const { return Value; }

	std::string_view AsView () const { return std::string_view(Value); }

	bool IsEmpty () const { return Value[0] == '\0'; }

	bool operator== (const Comp_Name& Rhs) const { return std::strcmp(Value, Rhs.Value) == 0; }
	bool operator== (std::string_view Rhs) const { return AsView() == Rhs; }
};

static_assert(sizeof(Comp_Name) == Comp_Name::Capacity, "Comp_Name should be exactly its buffer");
static_assert(std::is_trivially_copyable_v<Comp_Name>,
	"Comp_Name exists to stay trivially copyable - that is the point of the inline buffer");
}


FRT_DECLARE_COMPONENT_NAMED(frt::Comp_Name, "Comp_Name");
