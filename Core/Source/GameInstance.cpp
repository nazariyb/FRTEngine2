#include "GameInstance.h"

NAMESPACE_FRT_START

GameInstance* Singleton<GameInstance>::_instance(nullptr);

GameInstance::GameInstance()
{
}

GameInstance::~GameInstance()
{
}

NAMESPACE_FRT_END
