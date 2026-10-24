#include "TestModule/TestModule.h"

#include <array>

using namespace frt::test_module;


namespace
{
SCounter Counter;
}


extern "C" __declspec(dllexport) void FrtTestModule_MakeSmallLambda (FCounterFunction* Out)
{
	*Out = FCounterFunction([] (int& Total) { Total += 10; });
}

extern "C" __declspec(dllexport) void FrtTestModule_MakeLargeLambda (FCounterFunction* Out)
{
	std::array<int, 32> payload = {};
	payload[31] = 100;
	*Out = FCounterFunction([payload] (int& Total) { Total += payload[31]; });
}

extern "C" __declspec(dllexport) void FrtTestModule_AddOne (int& Total)
{
	Total += 1;
}

extern "C" __declspec(dllexport) SCounter* FrtTestModule_GetCounter ()
{
	return &Counter;
}
