// Platform/Thread.h: ConfigureThread names the calling thread (any text, read back as the OS keeps
// it), sets its priority and its preferred processors, and reports what it could not apply. Checked
// against the OS's own view (GetThreadPriority, GetThreadSelectedCpuSets), each on a thread of the
// test's own, so the test runner's thread keeps its settings.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <thread>
#include <utility>

#include "Platform/Thread.h"

#include <Windows.h>

using namespace frt;
using platform::EThreadPriority;


namespace
{
// Body's result, run on a fresh thread.
template <typename TBody>
auto OnNewThread (TBody&& Body)
{
	decltype(Body()) result{};
	std::thread thread([&] () { result = Body(); });
	thread.join();
	return result;
}

// How many CPU sets the calling thread has selected: 0 when it may run on every processor.
ULONG SelectedCpuSetCount ()
{
	ULONG count = 0u;
	(void)GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0u, &count);
	return count;
}
}


TEST(ThreadTest, ThreadIdsAreNonZeroAndDifferPerThread)
{
	const uint64 mine = platform::GetCurrentThreadId();
	const uint64 other = OnNewThread([] () { return platform::GetCurrentThreadId(); });
	EXPECT_NE(mine, 0u);
	EXPECT_NE(other, 0u);
	EXPECT_NE(mine, other);
}

TEST(ThreadTest, ANewThreadHasNoName)
{
	EXPECT_TRUE(OnNewThread([] () { return platform::GetCurrentThreadName(); }).IsEmpty());
}

TEST(ThreadTest, ConfigureThreadNamesTheCallingThreadInAnyLanguage)
{
	const auto [bApplied, name] = OnNewThread([] ()
	{
		const bool bOk = platform::ConfigureThread("FRT Робітник ґ 3 — 工作", EThreadPriority::Normal);
		return std::pair<bool, FString>(bOk, platform::GetCurrentThreadName());
	});
	EXPECT_TRUE(bApplied);
	EXPECT_EQ(name, "FRT Робітник ґ 3 — 工作");
}

TEST(ThreadTest, ConfigureThreadSetsEachPriority)
{
	const std::pair<EThreadPriority, int> cases[] = {
		{ EThreadPriority::Lowest, THREAD_PRIORITY_LOWEST },
		{ EThreadPriority::BelowNormal, THREAD_PRIORITY_BELOW_NORMAL },
		{ EThreadPriority::Normal, THREAD_PRIORITY_NORMAL },
		{ EThreadPriority::AboveNormal, THREAD_PRIORITY_ABOVE_NORMAL },
		{ EThreadPriority::Highest, THREAD_PRIORITY_HIGHEST },
	};
	for (const std::pair<EThreadPriority, int>& testCase : cases)
	{
		const EThreadPriority priority = testCase.first;
		const auto [bApplied, actual] = OnNewThread([priority] ()
		{
			const bool bOk = platform::ConfigureThread("FRT priority test", priority);
			return std::pair<bool, int>(bOk, GetThreadPriority(GetCurrentThread()));
		});
		EXPECT_TRUE(bApplied) << static_cast<int>(priority);
		EXPECT_EQ(actual, testCase.second) << static_cast<int>(priority);
	}
}

TEST(ThreadTest, AnAffinityMaskSelectsThoseProcessors)
{
	EXPECT_EQ(OnNewThread([] ()
	{
		return platform::ConfigureThread("FRT affinity test", EThreadPriority::Normal, 0b1u) ? SelectedCpuSetCount() : 0u;
	}), 1u);
	if (std::thread::hardware_concurrency() >= 2u)
	{
		EXPECT_EQ(OnNewThread([] ()
		{
			return platform::ConfigureThread("FRT affinity test", EThreadPriority::Normal, 0b11u) ? SelectedCpuSetCount() : 0u;
		}), 2u);
	}
}

TEST(ThreadTest, NoAffinityMaskLeavesTheThreadOnEveryProcessor)
{
	EXPECT_EQ(OnNewThread([] ()
	{
		return platform::ConfigureThread("FRT affinity test", EThreadPriority::Normal) ? SelectedCpuSetCount() : 99u;
	}), 0u);
}

TEST(ThreadTest, AMaskOfMissingProcessorsFailsAndTheRestStillApplies)
{
	if (std::thread::hardware_concurrency() >= 64u)
	{
		GTEST_SKIP() << "every bit of the mask names a processor on this machine";
	}
	const auto [bApplied, name] = OnNewThread([] ()
	{
		const bool bOk = platform::ConfigureThread("FRT no such processor", EThreadPriority::Normal, 1ull << 63u);
		return std::pair<bool, FString>(bOk, platform::GetCurrentThreadName());
	});
	EXPECT_FALSE(bApplied);
	EXPECT_EQ(name, "FRT no such processor");
}
