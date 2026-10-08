#pragma once

#include "GameInstance.h"


namespace frt::input
{
struct SInputActionMapAsset;
}

// The demo: a static showcase scene (Sponza, models, a skylight portal) with a fly-through
// camera on RMB + WASD.
class DemoGame final : public frt::GameInstance
{
public:
	void BuildWorld (const frt::SGameContext& Context) override;
	void BeginPlay (const frt::SGameContext& Context) override;
	void Tick (const frt::SGameContext& Context, const frt::SGameFrame& Frame) override;

private:
	frt::input::SInputActionMapAsset* FlyActions = nullptr;   // IAM_Editor: RMB + WASD fly camera
};
