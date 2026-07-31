#include "ECS/CoreComponents.h"


void frt::RegisterCoreComponents()
{
	// This order IS the id order. Append only: inserting in the middle shifts every id
	// below it, which silently changes the meaning of any query signature built from
	// them. Removing a component's line is equally load-bearing.
	GetComponentId<Comp_LocalTransform>();
	GetComponentId<Comp_WorldTransform>();
	GetComponentId<Comp_Parent>();
	GetComponentId<Comp_Children>();
	GetComponentId<Comp_Name>();
}
