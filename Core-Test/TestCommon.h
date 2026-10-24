#pragma once

#include <span>
#include <vector>

#include "Containers/String.h"
#include "Memory/Memory.h"
#include "Memory/MemoryPool.h"


namespace frt::test
{
/**
 * argv[1..] exactly as main() received it: the CRT's parse of the command line, UTF-8 by the
 * process code page. Copied before ::testing::InitGoogleTest, which removes GoogleTest's own flags
 * from argv in place - and so from __argv, the same array, while __argc keeps the old count.
 */
const std::vector<FString>& GetMainArguments ();

/**
 * Child modes: Core-Test started as "Core-Test --frt-test-child=<Name> <arguments...>" runs the
 * function registered under Name instead of the tests, and exits with what it returns.
 *
 * For tests that need a second process they can kill at any moment (a crash in the middle of a
 * write) - a thread cannot be killed safely, and a separate helper executable would be one more
 * project to build. The child gets the arguments after the switch; it runs before GoogleTest is
 * initialized and without the engine's memory pool. Register from a test file's namespace scope:
 *
 *   const bool bRegistered = test::RegisterChildMode("atomic-writer", &RunAtomicWriter);
 */
using FChildMain = int (*) (std::span<const FString> Arguments);

inline constexpr FStringLiteral ChildModeSwitch = "--frt-test-child=";

bool RegisterChildMode (FStringView Name, FChildMain Main);

/**
 * Text longer than FString's 22 inline bytes, so it owns a heap block. Payloads of tests for
 * containers that relocate their elements: a value relocated as raw bytes, or moved out of after it
 * was destroyed, leaves its block with two owners or none, where inline bytes would survive it and
 * the test would pass by accident.
 */
template <typename T>
FString HeapText (const T& Tag)
{
	return FString::Format("{}: too long for the inline buffer", Tag);
}
}


/**
 * Stands up a fresh TLSF arena for the duration of one test and claims it as the primary
 * instance, which is what TArray and every component pool allocate through.
 *
 * Declare it FIRST in the test body. Everything that allocates - CWorld, pools,
 * TComponentPool - must be destroyed before the arena backing it, and locals are
 * destroyed in reverse declaration order.
 */
#define FRT_TEST_MEMORY_POOL()\
	::frt::memory::CMemoryPool testMemoryPool(64ull * 1024ull * 1024ull);\
	testMemoryPool.MakeThisPrimaryInstance()
