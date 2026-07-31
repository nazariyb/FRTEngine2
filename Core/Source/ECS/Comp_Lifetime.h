#pragma once

#include "CoreTypes.h"
#include "ECS/ComponentRegistry.h"
#include "Precision.h"


namespace frt
{
/**
 * Seconds until the entity is destroyed.
 *
 * Counted down by Sys_Lifetime, which queues the destruction through a command buffer
 * rather than destroying in place - the countdown runs inside a view over this very pool,
 * and destroying would reorder it mid-iteration.
 *
 * Reaching zero destroys the ENTITY, not just this component. An entity that should merely
 * stop doing something on a timer wants its own component, not this one.
 */
struct Comp_Lifetime
{
	Real Remaining = Real(0);
};
}


FRT_DECLARE_COMPONENT_NAMED(frt::Comp_Lifetime, "Comp_Lifetime");
