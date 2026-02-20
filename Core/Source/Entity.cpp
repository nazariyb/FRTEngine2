#include "Entity.h"

void frt::CEntity::Tick(float DeltaSeconds)
{
	if (RotationSpeed.SizeSquared() > 0.0f)
	{
		Transform.RotateBy(RotationSpeed * DeltaSeconds);
	}
}
