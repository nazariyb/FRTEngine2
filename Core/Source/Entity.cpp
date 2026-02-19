#include "Entity.h"

#include "GameInstance.h"
#include "Timer.h"

void frt::CEntity::Tick(float DeltaSeconds)
{
	const float timeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	Transform.SetScale(1.f);
	Transform.SetRotation(RotationSpeed * timeElapsed);
}
