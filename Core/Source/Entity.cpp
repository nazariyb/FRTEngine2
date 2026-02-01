#include "Entity.h"

#include "GameInstance.h"
#include "Timer.h"

void frt::CEntity::Tick(float DeltaSeconds)
{
	const float timeElapsed = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	Transform.SetScale(1.f);
	Transform.SetRotation(0.f, math::PI_OVER_FOUR * 0.5f * timeElapsed, 0.f);
}
