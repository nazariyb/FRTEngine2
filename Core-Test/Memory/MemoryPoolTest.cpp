// CMemoryPool's diagnostics (AssetManager-Design I8): the allocated-bytes counter and its peak follow
// every allocation, reallocation and free exactly; the high-water callback fires once per upward
// crossing of its threshold; and a thread that does not own the pool stops a Debug build at its first
// allocation, while Development reports it, once per thread, and leaves what its allocations do
// unchanged.
#include <gtest/gtest.h>

#include <thread>
#include <utility>
#include <vector>

#include "Containers/String.h"
#include "Log/Log.h"
#include "Log/LogCapture.h"
#include "Memory/Memory.h"
#include "Memory/MemoryPool.h"
#include "Memory/TLSF.h"
#include "Platform/Thread.h"

using namespace frt;
using namespace frt::memory;
using namespace frt::memory::literals;


namespace
{
// What one request occupies: TLSF rounds it up to its alignment, and to its smallest block.
constexpr uint64 BlockBytes (uint64 Requested)
{
	const uint64 aligned = (Requested + TLSF::AlignSize - 1u) & ~static_cast<uint64>(TLSF::AlignSize - 1u);
	return aligned < TLSF::BlockMinSize ? TLSF::BlockMinSize : aligned;
}

struct SHighWaterLog
{
	uint32 Calls = 0u;
	uint64 LastBytes = 0u;
};

void RecordHighWater (CMemoryPool& Pool, uint64 AllocatedBytes, void* Context)
{
	(void)Pool;
	SHighWaterLog& log = *static_cast<SHighWaterLog*>(Context);
	++log.Calls;
	log.LastBytes = AllocatedBytes;
}

// A budget that warns at each doubling: the callback moves its own threshold.
void DoubleThreshold (CMemoryPool& Pool, uint64 AllocatedBytes, void* Context)
{
	RecordHighWater(Pool, AllocatedBytes, Context);
	Pool.SetHighWaterCallback(AllocatedBytes * 2u, &DoubleThreshold, Context);
}

// What the pool reported through frt::log while Body ran, from any thread: the "memory" messages,
// in the order they were written. Nothing reaches stderr meanwhile.
template <typename TBody>
std::vector<test::CLogCapture::SEntry> CaptureMemoryLog (TBody&& Body)
{
	test::CLogCapture capture;
	Body();
	std::vector<test::CLogCapture::SEntry> entries;
	for (const test::CLogCapture::SEntry& entry : capture.GetEntries())
	{
		if (entry.Category == log::Memory)
		{
			entries.push_back(entry);
		}
	}
	return entries;
}
}


TEST(MemoryPoolCounters, AllocatedBytesFollowEveryAllocationAndFree)
{
	CMemoryPool pool(1_Mb);
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
	EXPECT_EQ(pool.GetPeakAllocatedBytes(), 0u);

	void* a = pool.Allocate(100);
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(100));
	void* b = pool.Allocate(1);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(100) + BlockBytes(1));

	pool.Free(a);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(1));
	EXPECT_EQ(pool.GetPeakAllocatedBytes(), BlockBytes(100) + BlockBytes(1));

	// Growing in place and moving both count the new block only.
	b = pool.ReAllocate(b, 1000);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(1000));
	void* c = pool.Allocate(48);
	b = pool.ReAllocate(b, 5000);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(5000) + BlockBytes(48));
	EXPECT_EQ(pool.GetPeakAllocatedBytes(), BlockBytes(5000) + BlockBytes(48));

	// Reallocating to zero frees; from nullptr allocates.
	EXPECT_EQ(pool.ReAllocate(b, 0u), nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(48));
	pool.ResetPeakAllocatedBytes();
	EXPECT_EQ(pool.GetPeakAllocatedBytes(), BlockBytes(48));
	void* d = pool.ReAllocate(nullptr, 64);
	ASSERT_NE(d, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(48) + BlockBytes(64));

	pool.Free(nullptr);
	pool.Free(c);
	pool.Free(d);
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
	EXPECT_EQ(pool.GetPeakAllocatedBytes(), BlockBytes(48) + BlockBytes(64));
}

TEST(MemoryPoolCounters, FailedAllocationsCountNothing)
{
	CMemoryPool pool(4_Kb);
	void* a = pool.Allocate(256);
	ASSERT_NE(a, nullptr);
	const uint64 before = pool.GetAllocatedBytes();

	EXPECT_EQ(pool.Allocate(1_Mb), nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), before);
	// A failed reallocation keeps the old block, and its bytes.
	EXPECT_EQ(pool.ReAllocate(a, 1_Mb), nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), before);

	pool.Free(a);
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
}

TEST(MemoryPoolCounters, CountersMoveWithTheArena)
{
	CMemoryPool pool(1_Mb);
	void* a = pool.Allocate(200);
	const uint64 bytes = pool.GetAllocatedBytes();

	CMemoryPool moved(std::move(pool));
	EXPECT_EQ(moved.GetAllocatedBytes(), bytes);
	EXPECT_EQ(moved.GetPeakAllocatedBytes(), bytes);
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
	EXPECT_EQ(moved.GetOwnerThreadId(), platform::GetCurrentThreadId());

	moved.Free(a);
	EXPECT_EQ(moved.GetAllocatedBytes(), 0u);
}

TEST(MemoryPoolCounters, HighWaterCallbackFiresOncePerUpwardCrossing)
{
	CMemoryPool pool(1_Mb);
	SHighWaterLog log;
	pool.SetHighWaterCallback(1000u, &RecordHighWater, &log);

	void* a = pool.Allocate(512);
	EXPECT_EQ(log.Calls, 0u);
	void* b = pool.Allocate(512);
	EXPECT_EQ(log.Calls, 1u);
	EXPECT_EQ(log.LastBytes, 1024u);
	// Above the threshold already: no call.
	void* c = pool.Allocate(512);
	EXPECT_EQ(log.Calls, 1u);

	// Below it again, then a reallocation climbs back over it.
	pool.Free(b);
	pool.Free(c);
	EXPECT_LT(pool.GetAllocatedBytes(), 1000u);
	a = pool.ReAllocate(a, 2000);
	EXPECT_EQ(log.Calls, 2u);
	EXPECT_EQ(log.LastBytes, 2000u);

	// Setting a callback never calls it, even above its threshold; a null callback removes it.
	SHighWaterLog second;
	pool.SetHighWaterCallback(100u, &RecordHighWater, &second);
	EXPECT_EQ(second.Calls, 0u);
	pool.SetHighWaterCallback(0u, nullptr);
	pool.Free(a);
	pool.Free(pool.Allocate(64));
	EXPECT_EQ(second.Calls, 0u);
	EXPECT_EQ(log.Calls, 2u);
}

TEST(MemoryPoolCounters, ReallocatingOverTheThresholdNeverCallsTheCallback)
{
	// A TArray that holds most of a pool's bytes reallocates on every growth. Only a reallocation
	// that takes the count from below the threshold to the threshold or more is a crossing.
	CMemoryPool pool(1_Mb);
	SHighWaterLog log;
	pool.SetHighWaterCallback(1000u, &RecordHighWater, &log);

	void* a = pool.Allocate(600);
	void* b = pool.Allocate(600);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(log.Calls, 1u);
	EXPECT_EQ(log.LastBytes, 1200u);

	// The same size, a shrink that stays over the threshold, and a growth: no call.
	b = pool.ReAllocate(b, 600);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), 1200u);
	EXPECT_EQ(log.Calls, 1u);
	b = pool.ReAllocate(b, 500);
	ASSERT_NE(b, nullptr);
	EXPECT_GE(pool.GetAllocatedBytes(), 1000u);
	EXPECT_EQ(log.Calls, 1u);
	b = pool.ReAllocate(b, 4000);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(pool.GetAllocatedBytes(), BlockBytes(600) + BlockBytes(4000));
	EXPECT_EQ(log.Calls, 1u);

	// A shrink below the threshold is no call either; growing back over it is the next crossing.
	b = pool.ReAllocate(b, 64);
	ASSERT_NE(b, nullptr);
	ASSERT_LT(pool.GetAllocatedBytes(), 1000u);
	EXPECT_EQ(log.Calls, 1u);
	b = pool.ReAllocate(b, 600);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(log.Calls, 2u);
	EXPECT_EQ(log.LastBytes, pool.GetAllocatedBytes());
	EXPECT_EQ(log.LastBytes, 1200u);

	pool.Free(b);
	pool.Free(a);
	EXPECT_EQ(log.Calls, 2u);
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
}

TEST(MemoryPoolCounters, HighWaterCallbackMayMoveItsThreshold)
{
	CMemoryPool pool(1_Mb);
	SHighWaterLog log;
	pool.SetHighWaterCallback(1000u, &DoubleThreshold, &log);

	void* a = pool.Allocate(1024);
	EXPECT_EQ(log.Calls, 1u);
	void* b = pool.Allocate(512);       // 1536, under the new 2048
	EXPECT_EQ(log.Calls, 1u);
	void* c = pool.Allocate(600);       // 2136
	EXPECT_EQ(log.Calls, 2u);
	EXPECT_EQ(log.LastBytes, 1024u + 512u + 600u);

	pool.Free(a);
	pool.Free(b);
	pool.Free(c);
}

TEST(MemoryPoolOwnerThread, TheConstructingThreadOwnsThePool)
{
	CMemoryPool pool(1_Mb);
	EXPECT_EQ(pool.GetOwnerThreadId(), platform::GetCurrentThreadId());
	EXPECT_NE(platform::GetCurrentThreadId(), 0u);

	const auto entries = CaptureMemoryLog([&pool] ()
	{
		pool.Free(pool.ReAllocate(pool.Allocate(16), 32));
	});
	EXPECT_TRUE(entries.empty()) << entries.front().Line.CStr();
}

#if FRT_BUILD_DEBUG
// Debug: the first allocation or free from another thread asserts, after the report. Development and
// Release report and keep running (the #else branch).
TEST(MemoryPoolOwnerThreadDeathTest, AnotherThreadAllocatingStopsADebugBuild)
{
	CMemoryPool pool(1_Mb);
	EXPECT_DEATH(
		{
			std::thread worker([&pool] () { pool.Free(pool.Allocate(16)); });
			worker.join();
		},
		"ASSERT FAILED");
}

TEST(MemoryPoolOwnerThreadDeathTest, AnotherThreadFreeingStopsADebugBuild)
{
	CMemoryPool pool(1_Mb);
	void* block = pool.Allocate(16);
	EXPECT_DEATH(
		{
			std::thread worker([&pool, block] () { pool.Free(block); });
			worker.join();
		},
		"Free on thread");
	pool.Free(block);
}
#else
TEST(MemoryPoolOwnerThread, AnotherThreadIsReportedOnceAndStillAllocates)
{
	CMemoryPool pool(1_Mb);
	uint64 workerId = 0u;
	uint64 bytesSeenByWorker = 0u;
	const auto entries = CaptureMemoryLog([&] ()
	{
		std::thread worker([&] ()
		{
			workerId = platform::GetCurrentThreadId();
			void* a = pool.Allocate(16);
			a = pool.ReAllocate(a, 64);
			bytesSeenByWorker = pool.GetAllocatedBytes();
			pool.Free(a);
			pool.Free(pool.Allocate(16));
		});
		worker.join();

		// Every thread is reported on its own, whichever operation it starts with.
		void* b = pool.Allocate(8);
		std::thread second([&pool, b] () { pool.Free(b); });
		second.join();
	});

	ASSERT_EQ(entries.size(), 2u);
	const FString owner = FString::Format("belongs to thread {}", platform::GetCurrentThreadId());
	for (const test::CLogCapture::SEntry& entry : entries)
	{
		EXPECT_EQ(entry.Level, log::ELevel::Warning) << entry.Line.CStr();
		EXPECT_TRUE(entry.Text.Contains(owner)) << entry.Line.CStr();
	}
	EXPECT_TRUE(entries[0].Text.Contains(FString::Format("Allocate on thread {}", workerId))) << entries[0].Line.CStr();
	EXPECT_TRUE(entries[1].Text.Contains("Free on thread")) << entries[1].Line.CStr();

	// Logging only: the allocations themselves behaved as before.
	EXPECT_EQ(bytesSeenByWorker, BlockBytes(64));
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
}
#endif

TEST(MemoryPoolOwnerThread, MakingThePoolPrimaryMakesTheCallingThreadItsOwner)
{
	CMemoryPool pool(1_Mb);
	uint64 workerId = 0u;
	const auto entries = CaptureMemoryLog([&] ()
	{
		std::thread worker([&] ()
		{
			pool.MakeThisPrimaryInstance();
			workerId = platform::GetCurrentThreadId();
			pool.Free(NewUnmanaged(32u));
		});
		worker.join();
	});

	EXPECT_EQ(pool.GetOwnerThreadId(), workerId);
	EXPECT_EQ(CMemoryPool::GetPrimaryInstance(), &pool);
	EXPECT_TRUE(entries.empty()) << entries.front().Line.CStr();
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
}
