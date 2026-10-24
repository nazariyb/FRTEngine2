#pragma once

#include "Containers/StringView.h"
#include "Functional/UniqueFunction.h"


/**
 * Core-TestModule.dll: a stand-in for a game module in the module-release tests
 * (Core-Test/Functional/MulticastDelegateTest.cpp, Core-Test/Profiler/CpuProfilerTest.cpp). It hands
 * out what a game module leaves behind in the engine - callables whose code is in its image, a
 * function, an object in its static data, a CPU scope name in its constant data - so a test can
 * release them and unload the DLL while the engine still holds them.
 *
 * It includes Core's headers and links nothing of Core: the callables are header-only templates,
 * instantiated here. Core-Test loads it by name (GetProcAddress); it is a DLL in every
 * configuration, Release included.
 */
namespace frt::test_module
{
// Each call adds to Total, so a test can tell which subscribers ran.
using CounterFunction = TUniqueFunction<void (int&)>;

struct SCounter
{
	int Calls = 0;

	void Bump (int& Total)
	{
		++Calls;
		Total += 1000;
	}
};

// Builds a lambda that adds 10: small, stored inline; its invoker and manager are in the DLL.
using MakeSmallLambdaFunction = void (*) (CounterFunction* Out);
inline constexpr char MakeSmallLambdaName[] = "FrtTestModule_MakeSmallLambda";
// Builds a lambda that adds 100: large, in a heap block the DLL allocates (and, through its manager,
// frees).
using MakeLargeLambdaFunction = void (*) (CounterFunction* Out);
inline constexpr char MakeLargeLambdaName[] = "FrtTestModule_MakeLargeLambda";
// A function of the DLL that adds 1, for subscriptions built outside it: only the target is in the
// image.
using AddOneFunction = void (*) (int& Total);
inline constexpr char AddOneName[] = "FrtTestModule_AddOne";
// An object in the DLL's static data, for Add(Object, &SCounter::Bump) made outside it.
using GetCounterFunction = SCounter* (*) ();
inline constexpr char GetCounterName[] = "FrtTestModule_GetCounter";
// A string literal of the DLL, as a CPU scope (FRT_CPU_SCOPE) in its code names itself.
using GetScopeNameFunction = void (*) (FStringLiteral* Out);
inline constexpr char GetScopeNameName[] = "FrtTestModule_GetScopeName";
}
