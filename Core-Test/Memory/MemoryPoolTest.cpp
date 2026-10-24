// CMemoryPool's diagnostics (AssetManager-Design I8): the allocated-bytes counter and its peak follow
// every allocation, reallocation and free exactly; the high-water callback fires once per upward
// crossing of its threshold; and a thread that does not own the pool is reported, once per thread,
// while what its allocations do stays unchanged.
#include <gtest/gtest.h>

#include <thread>
#include <utility>

#include "Containers/String.h"
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

void RecordHighWater (CMemoryPool& Pool, uint64 AllocatedBytes, void* UserData)
{
	(void)Pool;
	SHighWaterLog& log = *static_cast<SHighWaterLog*>(UserData);
	++log.Calls;
	log.LastBytes = AllocatedBytes;
}

// A budget that warns at each doubling: the callback moves its own threshold.
void DoubleThreshold (CMemoryPool& Pool, uint64 AllocatedBytes, void* UserData)
{
	RecordHighWater(Pool, AllocatedBytes, UserData);
	Pool.SetHighWaterCallback(AllocatedBytes * 2u, &DoubleThreshold, UserData);
}

// gtest captures stderr at the file-descriptor level, so writes from any thread land in it.
template <typename TBody>
FString CaptureStderr (TBody&& Body)
{
	testing::internal::CaptureStderr();
	Body();
	const auto captured = testing::internal::GetCapturedStderr();
	return FString(FStringView(captured.data(), static_cast<uint32>(captured.size())));
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

	const FString output = CaptureStderr([&pool] ()
	{
		pool.Free(pool.ReAllocate(pool.Allocate(16), 32));
	});
	EXPECT_FALSE(output.Contains("[memory]")) << output.CStr();
}

TEST(MemoryPoolOwnerThread, AnotherThreadIsReportedOnceAndStillAllocates)
{
	CMemoryPool pool(1_Mb);
	uint64 workerId = 0u;
	uint64 bytesSeenByWorker = 0u;
	const FString output = CaptureStderr([&] ()
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

	EXPECT_EQ(output.CountOccurrences("[memory]"), 2u) << output.CStr();
	EXPECT_TRUE(output.Contains(FString::Printf("Allocate on thread %llu", workerId))) << output.CStr();
	EXPECT_TRUE(output.Contains("Free on thread")) << output.CStr();
	const FString owner = FString::Printf("belongs to thread %llu", platform::GetCurrentThreadId());
	EXPECT_TRUE(output.Contains(owner)) << output.CStr();

	// Logging only: the allocations themselves behaved as before.
	EXPECT_EQ(bytesSeenByWorker, BlockBytes(64));
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
}

TEST(MemoryPoolOwnerThread, MakingThePoolPrimaryMakesTheCallingThreadItsOwner)
{
	CMemoryPool pool(1_Mb);
	uint64 workerId = 0u;
	const FString output = CaptureStderr([&] ()
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
	EXPECT_FALSE(output.Contains("[memory]")) << output.CStr();
	EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
}
