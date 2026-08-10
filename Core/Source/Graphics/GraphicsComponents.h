#pragma once

#include "Core.h"
#include "Graphics/Comp_Light.h"
#include "Graphics/Comp_Portal.h"
#include "Graphics/Model.h"


namespace frt::graphics
{
/**
 * Pins the graphics components' ids, the same way RegisterCoreComponents does for the
 * engine's own. Call once at startup, right after it.
 *
 * Separate from RegisterCoreComponents so the ECS does not have to know these types
 * exist. A renderer swap replaces this file; ECS/CoreComponents.h is untouched by it.
 */
FRT_CORE_API void RegisterGraphicsComponents ();
}
