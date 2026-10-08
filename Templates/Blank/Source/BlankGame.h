#pragma once

#include "GameInstance.h"


// The game: builds its world - a floor, two shapes and a light under the sky. BeginPlay, Tick and
// EndPlay (GameInstance.h) are where it would run.
class BlankGame final : public frt::GameInstance
{
public:
	void BuildWorld (const frt::SGameContext& Context) override;
};
