#pragma once

#include "GameInstance.h"

class DemoGame final : public frt::GameInstance
{
public:
	DemoGame();
	virtual ~DemoGame();

	DemoGame(const DemoGame&) = delete;
	DemoGame(DemoGame&&) = delete;
	DemoGame& operator=(const DemoGame&) = delete;
	DemoGame& operator=(DemoGame&&) = delete;

	FRT_SINGLETON_GETTERS(DemoGame)

};
