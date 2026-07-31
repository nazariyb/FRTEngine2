#pragma once

#include "Memory/Memory.h"
#include "Memory/MemoryPool.h"


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
