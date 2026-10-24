// CPU profiling scopes (AssetManager-Design I7): FRT_CPU_SCOPE records nested scopes on any thread
// into that thread's ring; the frame boundary collects every scope exactly once, with its thread and
// depth, and aggregates them per name (total, count, max); a ring that overflows keeps the newest
// scopes and counts the others as dropped; and collecting while a thread records never yields a
// torn scope.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "Containers/String.h"
#include "Platform/Thread.h"
#include "Platform/Timestamp.h"
#include "Profiler/CpuProfiler.h"

using namespace frt;
using namespace frt::profiler;


namespace
{
// Busy for at least Milliseconds, so a scope has a duration worth measuring.
void Spin (double Milliseconds)
{
	const uint64 start = platform::ReadTimestamp();
	while (platform::TimestampToMilliseconds(platform::ReadTimestamp() - start) < Milliseconds)
	{
	}
}

// The last frame's events whose names start with Prefix: whatever else ran may have recorded scopes.
std::vector<SCpuScopeEvent> EventsNamed (FStringView Prefix)
{
	std::vector<SCpuScopeEvent> events;
	for (const SCpuScopeEvent& event : CCpuProfiler::GetLastFrame().Events)
	{
		if (event.Name.StartsWith(Prefix))
		{
			events.push_back(event);
		}
	}
	return events;
}

const SCpuScopeStats* FindStats (FStringView Name)
{
	for (const SCpuScopeStats& stats : CCpuProfiler::GetLastFrame().Scopes)
	{
		if (stats.Name == Name)
		{
			return &stats;
		}
	}
	return nullptr;
}
}


TEST(CpuProfiler, NestedScopesOnTwoThreadsAreCollectedOnceWithThreadAndDepth)
{
	// Whatever ran before goes into a frame of its own.
	CCpuProfiler::BeginFrame();

	uint64 threadIds[2] = {};
	const auto work = [&threadIds] (uint32 Slot)
	{
		threadIds[Slot] = platform::GetCurrentThreadId();
		FRT_CPU_SCOPE("CpuTest.Outer");
		for (uint32 i = 0u; i < 2u; ++i)
		{
			FRT_CPU_SCOPE("CpuTest.Inner");
			Spin(0.2);
		}
	};
	std::thread first(work, 0u);
	std::thread second(work, 1u);
	first.join();
	second.join();
	CCpuProfiler::BeginFrame();

	const std::vector<SCpuScopeEvent> events = EventsNamed("CpuTest.");
	ASSERT_EQ(events.size(), 6u);
	for (uint32 slot = 0u; slot < 2u; ++slot)
	{
		// A thread's scopes are listed together, in the order they closed: inner, inner, outer.
		std::vector<SCpuScopeEvent> own;
		for (const SCpuScopeEvent& event : events)
		{
			if (event.ThreadId == threadIds[slot])
			{
				own.push_back(event);
			}
		}
		ASSERT_EQ(own.size(), 3u);
		EXPECT_EQ(own[0].Name, "CpuTest.Inner");
		EXPECT_EQ(own[1].Name, "CpuTest.Inner");
		EXPECT_EQ(own[2].Name, "CpuTest.Outer");
		EXPECT_EQ(own[0].Depth, 1u);
		EXPECT_EQ(own[1].Depth, 1u);
		EXPECT_EQ(own[2].Depth, 0u);
		EXPECT_EQ(own[0].ThreadIndex, own[2].ThreadIndex);
		EXPECT_LE(own[0].EndTicks, own[1].BeginTicks);
		for (uint32 i = 0u; i < 2u; ++i)
		{
			EXPECT_GE(own[i].BeginTicks, own[2].BeginTicks);
			EXPECT_LE(own[i].EndTicks, own[2].EndTicks);
		}
	}
	EXPECT_NE(threadIds[0], threadIds[1]);

	const SCpuScopeStats* inner = FindStats("CpuTest.Inner");
	const SCpuScopeStats* outer = FindStats("CpuTest.Outer");
	ASSERT_NE(inner, nullptr);
	ASSERT_NE(outer, nullptr);
	EXPECT_EQ(inner->Count, 4u);
	EXPECT_EQ(outer->Count, 2u);
	EXPECT_GE(inner->TotalMs, 0.8);
	EXPECT_GE(inner->MaxMs, 0.2);
	EXPECT_LE(inner->MaxMs, inner->TotalMs);
	// Inclusive: each outer scope holds its two inner ones.
	EXPECT_GE(outer->TotalMs, inner->TotalMs);

	const std::vector<SCpuScopeStats>& scopes = CCpuProfiler::GetLastFrame().Scopes;
	for (size_t i = 1u; i < scopes.size(); ++i)
	{
		EXPECT_GE(scopes[i - 1u].TotalMs, scopes[i].TotalMs);
	}
}

TEST(CpuProfiler, ScopesBelongToTheFrameThatCollectsThem)
{
	CCpuProfiler::BeginFrame();
	{
		FRT_CPU_SCOPE("CpuTest.First");
	}
	CCpuProfiler::BeginFrame();
	const SCpuFrame& frame = CCpuProfiler::GetLastFrame();
	const uint64 index = frame.Index;
	const uint64 end = frame.EndTicks;
	EXPECT_EQ(EventsNamed("CpuTest.First").size(), 1u);
	EXPECT_GT(frame.EndTicks, frame.BeginTicks);
	EXPECT_GT(frame.GetDurationMs(), 0.0);

	// Collected early or at the boundary, each scope lands in exactly one frame.
	{
		FRT_CPU_SCOPE("CpuTest.Second");
	}
	CCpuProfiler::CollectPending();
	{
		FRT_CPU_SCOPE("CpuTest.Third");
	}
	CCpuProfiler::BeginFrame();
	EXPECT_EQ(CCpuProfiler::GetLastFrame().Index, index + 1u);
	EXPECT_EQ(CCpuProfiler::GetLastFrame().BeginTicks, end);
	EXPECT_TRUE(EventsNamed("CpuTest.First").empty());
	EXPECT_EQ(EventsNamed("CpuTest.Second").size(), 1u);
	EXPECT_EQ(EventsNamed("CpuTest.Third").size(), 1u);

	CCpuProfiler::BeginFrame();
	EXPECT_TRUE(EventsNamed("CpuTest.").empty());
	EXPECT_EQ(FindStats("CpuTest.Third"), nullptr);
}

TEST(CpuProfiler, AFullRingKeepsTheNewestScopesAndCountsTheRest)
{
	CCpuProfiler::BeginFrame();
	constexpr uint32 Extra = 100u;
	constexpr uint32 Written = CCpuProfiler::RingCapacity + Extra;
	std::thread writer([] ()
	{
		// Ticks as sequence numbers: which scopes survived is then plain to see.
		for (uint32 i = 0u; i < Written; ++i)
		{
			CCpuProfiler::RecordScope("CpuTest.Wrap", i, i + 1u, 0u);
		}
	});
	writer.join();
	CCpuProfiler::BeginFrame();

	const std::vector<SCpuScopeEvent> events = EventsNamed("CpuTest.Wrap");
	ASSERT_EQ(events.size(), CCpuProfiler::RingCapacity);
	EXPECT_EQ(CCpuProfiler::GetLastFrame().DroppedEvents, Extra);
	EXPECT_EQ(events.front().BeginTicks, Extra);
	EXPECT_EQ(events.back().BeginTicks, Written - 1u);
	for (size_t i = 1u; i < events.size(); ++i)
	{
		EXPECT_EQ(events[i].BeginTicks, events[i - 1u].BeginTicks + 1u);
	}
	const SCpuScopeStats* stats = FindStats("CpuTest.Wrap");
	ASSERT_NE(stats, nullptr);
	EXPECT_EQ(stats->Count, CCpuProfiler::RingCapacity);
}

TEST(CpuProfiler, CollectingWhileAThreadRecordsNeverYieldsATornScope)
{
	CCpuProfiler::BeginFrame();
	constexpr uint64 Written = 200000u;
	std::atomic<bool> bDone = false;
	std::thread writer([&bDone] ()
	{
		// Every field of a scope derives from its sequence number, so a slot mixing two writes shows.
		for (uint64 i = 0u; i < Written; ++i)
		{
			if (i % 2u == 0u)
			{
				CCpuProfiler::RecordScope("CpuTest.Even", i, i * 3u + 1u, static_cast<uint32>(i % 7u));
			}
			else
			{
				CCpuProfiler::RecordScope("CpuTest.OddName", i, i * 3u + 1u, static_cast<uint32>(i % 7u));
			}
		}
		bDone.store(true, std::memory_order_release);
	});

	uint64 collected = 0u;
	uint64 dropped = 0u;
	uint64 next = 0u;
	bool bTorn = false;
	bool bOrdered = true;
	for (bool bLast = false; !bLast;)
	{
		bLast = bDone.load(std::memory_order_acquire);
		CCpuProfiler::BeginFrame();
		dropped += CCpuProfiler::GetLastFrame().DroppedEvents;
		for (const SCpuScopeEvent& event : EventsNamed("CpuTest."))
		{
			const uint64 i = event.BeginTicks;
			const FStringView expectedName = i % 2u == 0u ? FStringView("CpuTest.Even") : FStringView("CpuTest.OddName");
			bTorn |= event.EndTicks != i * 3u + 1u || event.Depth != i % 7u || event.Name != expectedName;
			bOrdered &= i >= next;
			next = i + 1u;
			++collected;
		}
	}
	writer.join();

	EXPECT_FALSE(bTorn);
	EXPECT_TRUE(bOrdered);
	// Every scope is either collected or counted as dropped, exactly once.
	EXPECT_EQ(collected + dropped, Written);
	EXPECT_GT(collected, 0u);
}
