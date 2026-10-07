#include <gtest/gtest.h>

#include "WorldScene.h"

using frt::SWorldClock;

TEST(WorldClock, RunningFollowsRealTimeScaledAndClamped)
{
	SWorldClock clock;
	EXPECT_FLOAT_EQ(clock.Advance(0.016f), 0.016f);
	clock.TimeScale = 0.5f;
	EXPECT_FLOAT_EQ(clock.Advance(0.016f), 0.008f);
	clock.TimeScale = 1.0f;
	// A debugger break must not become one huge simulation step.
	EXPECT_FLOAT_EQ(clock.Advance(5.0f), clock.MaxDeltaSeconds);
	EXPECT_EQ(clock.SimulatedFrames, 3u);
}

TEST(WorldClock, PausedDoesNotAdvance)
{
	SWorldClock clock;
	clock.bPaused = true;
	EXPECT_EQ(clock.Advance(0.016f), 0.0f);
	EXPECT_EQ(clock.TotalSeconds, 0.0);
	EXPECT_EQ(clock.SimulatedFrames, 0u);
}

TEST(WorldClock, StepAdvancesExactlyOneFrame)
{
	SWorldClock clock;
	clock.Step();
	EXPECT_TRUE(clock.bPaused);
	EXPECT_FLOAT_EQ(clock.Advance(0.25f), clock.StepDeltaSeconds);
	EXPECT_EQ(clock.Advance(0.25f), 0.0f);
	EXPECT_EQ(clock.SimulatedFrames, 1u);

	clock.Step(2u);
	EXPECT_GT(clock.Advance(0.0f), 0.0f);
	EXPECT_GT(clock.Advance(0.0f), 0.0f);
	EXPECT_EQ(clock.Advance(0.0f), 0.0f);
}

TEST(WorldClock, FixedStepIgnoresRealTime)
{
	SWorldClock clock;
	clock.FixedDeltaSeconds = 1.0f / 60.0f;
	EXPECT_FLOAT_EQ(clock.Advance(0.5f), 1.0f / 60.0f);
	EXPECT_FLOAT_EQ(clock.Advance(0.001f), 1.0f / 60.0f);
}
