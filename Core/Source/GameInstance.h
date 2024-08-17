#pragma once

#include "Core.h"
#include "Singleton.h"

NAMESPACE_FRT_START

class FRT_CORE_API GameInstance : public Singleton<GameInstance>
{
public:
	GameInstance();
	virtual ~GameInstance();

	FRT_SINGLETON_GETTERS(GameInstance)
};

NAMESPACE_FRT_END
