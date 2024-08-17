#include "DemoGame.h"


// DemoGame* frt::Singleton<DemoGame>::_instance(nullptr);

DemoGame::DemoGame()
{
	int s = 23;
	int b = s + 24;
}

DemoGame::~DemoGame()
{
}

int DemoGame::TestFunc()
{
	int s = 23;
	int b = s + 24;
	return b;
}
