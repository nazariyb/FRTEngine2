#pragma once
#include "CoreTypes.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/EntityId.h"


namespace frt
{
/**
 * "I have children." Present only on entities that actually do, so leaves cost nothing
 * and a root with children gets this without needing a Comp_Parent.
 *
 * Head of the intrusive child list; the links themselves live in each child's
 * Comp_Parent. Walking children is a handle chase, but one parent has few of them and
 * this is a cold path - editor tree views, recursive destroy, and depth propagation on
 * reparent. Sys_Transform never touches this component.
 *
 * Count is maintained alongside the list so callers can size buffers and assert without
 * walking it.
 */
struct Comp_Children
{
	EntityId FirstChild = InvalidEntity;
	uint32   Count      = 0u;
};
}

FRT_DECLARE_COMPONENT_NAMED(frt::Comp_Children, "Comp_Children");
