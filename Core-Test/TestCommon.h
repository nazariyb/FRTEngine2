#pragma once

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
