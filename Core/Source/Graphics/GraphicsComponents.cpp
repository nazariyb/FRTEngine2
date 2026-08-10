#include "Graphics/GraphicsComponents.h"

#include "ECS/ComponentRegistry.h"


void frt::graphics::RegisterGraphicsComponents ()
{
	// Append only, as in RegisterCoreComponents: this order is the id order, and inserting
	// in the middle shifts every id below it.
	GetComponentId<Comp_Light>();
	GetComponentId<Comp_Portal>();
	GetComponentId<Comp_RenderModel>();
}
