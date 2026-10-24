#include "TestModule/TestModule.h"

#include <array>

using namespace frt::test_module;


namespace
{
SCounter Counter;
}


extern "C" __declspec(dllexport) void FrtTestModule_MakeSmallLambda (CounterFunction* Out)
{
	*Out = CounterFunction([] (int& Total) { Total += 10; });
}

extern "C" __declspec(dllexport) void FrtTestModule_MakeLargeLambda (CounterFunction* Out)
{
	std::array<int, 32> payload = {};
	payload[31] = 100;
	*Out = CounterFunction([payload] (int& Total) { Total += payload[31]; });
}

extern "C" __declspec(dllexport) void FrtTestModule_AddOne (int& Total)
{
	Total += 1;
}

extern "C" __declspec(dllexport) SCounter* FrtTestModule_GetCounter ()
{
	return &Counter;
}

extern "C" __declspec(dllexport) void FrtTestModule_GetScopeName (frt::FStringLiteral* Out)
{
	*Out = "TestModule.UnloadedScope";
}
