#include <gtest/gtest.h>

#include "ECS/CoreComponents.h"


namespace
{
/**
 * Pins the engine's component ids before any test body runs.
 *
 * This mirrors what the engine itself has to do: RegisterCoreComponents() assigns ids in
 * a declared order, but only if nothing has already registered a type lazily. The first
 * GetComponentId<T>() for any type wins, so calling this late is silently useless -
 * ids just keep whatever order first touch gave them.
 *
 * Without it, whichever test happened to run first decided the ids for the whole process.
 */
class CComponentRegistrationEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		frt::RegisterCoreComponents();
	}
};

// Registered during static init; gtest runs SetUp before the first test.
const ::testing::Environment* gComponentRegistration =
	::testing::AddGlobalTestEnvironment(new CComponentRegistrationEnvironment());
}
