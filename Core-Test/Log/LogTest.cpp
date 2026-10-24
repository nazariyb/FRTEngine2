// frt::log writes "[category] message" lines, filtered by a minimum level per category that can
// change at run time, and stays whole when many threads log at once.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Log/LogCapture.h"

#include <initializer_list>
#include <thread>
#include <vector>

#include "Containers/String.h"
#include "Log/Log.h"

using namespace frt;


namespace
{
// Every test starts and ends with the levels the process starts with.
class LogTest : public ::testing::Test
{
protected:
	void SetUp () override { log::ResetAllMinLevels(); }
	void TearDown () override { log::ResetAllMinLevels(); }
};
}


TEST_F(LogTest, WritesOneLinePerMessageWithItsCategory)
{
	test::CLogCapture capture;
	log::Write(log::Assets, log::ELevel::Warning, "Content/Rock.png: missing meta");
	log::Info(log::Platform, "{} files in {:.2f} ms", 3, 1.25);
	log::Printf(log::Game, log::ELevel::Error, "module '%s' failed (%d)", "Demo", 5);

	const std::vector<test::CLogCapture::SEntry>& entries = capture.GetEntries();
	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0].Line, "[assets] Content/Rock.png: missing meta\n");
	EXPECT_EQ(entries[0].Category, "assets");
	EXPECT_EQ(entries[0].Level, log::ELevel::Warning);
	EXPECT_EQ(entries[0].Text, "Content/Rock.png: missing meta");
	EXPECT_EQ(entries[1].Line, "[platform] 3 files in 1.25 ms\n");
	EXPECT_EQ(entries[2].Line, "[game] module 'Demo' failed (5)\n");
	EXPECT_EQ(entries[2].Level, log::ELevel::Error);
}

TEST_F(LogTest, VerboseIsOffAndTheRestIsOnByDefault)
{
	EXPECT_EQ(log::GetMinLevel(log::Assets), log::ELevel::Info);
	EXPECT_TRUE(log::IsEnabled(log::Assets, log::ELevel::Error));
	EXPECT_TRUE(log::IsEnabled(log::Assets, log::ELevel::Info));
	EXPECT_FALSE(log::IsEnabled(log::Assets, log::ELevel::Verbose));

	test::CLogCapture capture;
	log::Verbose(log::Assets, "hidden {}", 1);
	log::Write(log::Assets, log::ELevel::Verbose, "hidden");
	EXPECT_TRUE(capture.GetEntries().empty());
}

TEST_F(LogTest, EachCategoryHasItsOwnMinimumLevel)
{
	test::CLogCapture capture;
	ASSERT_TRUE(log::SetMinLevel(log::Assets, log::ELevel::Verbose));
	ASSERT_TRUE(log::SetMinLevel(log::Platform, log::ELevel::Error));

	log::Verbose(log::Assets, "verbose assets");
	log::Verbose(log::Memory, "verbose memory");        // the default: Info
	log::Warning(log::Platform, "a platform warning");   // below Error
	log::Error(log::Platform, "a platform error");
	log::Info(log::Memory, "memory info");

	const std::vector<FString> lines = capture.GetLines();
	EXPECT_EQ(lines, (std::vector<FString>{ "[assets] verbose assets\n", "[platform] a platform error\n", "[memory] memory info\n" }));

	// A category named at run time (from a config value) is the same category as the literal.
	EXPECT_EQ(log::GetMinLevel(FString("assets")), log::ELevel::Verbose);

	log::ResetMinLevel(log::Assets);
	EXPECT_EQ(log::GetMinLevel(log::Assets), log::ELevel::Info);
	EXPECT_EQ(log::GetMinLevel(log::Platform), log::ELevel::Error);
}

TEST_F(LogTest, TheDefaultMinimumAppliesToCategoriesWithoutTheirOwn)
{
	log::SetMinLevel(log::Assets, log::ELevel::Info);
	log::SetDefaultMinLevel(log::ELevel::Warning);
	EXPECT_FALSE(log::IsEnabled(log::Memory, log::ELevel::Info));
	EXPECT_TRUE(log::IsEnabled(log::Memory, log::ELevel::Warning));
	EXPECT_TRUE(log::IsEnabled(log::Assets, log::ELevel::Info));

	log::SetDefaultMinLevel(log::ELevel::Verbose);
	EXPECT_TRUE(log::IsEnabled(log::Memory, log::ELevel::Verbose));
	EXPECT_FALSE(log::IsEnabled(log::Assets, log::ELevel::Verbose));

	log::ResetAllMinLevels();
	EXPECT_EQ(log::GetMinLevel(log::Assets), log::ELevel::Info);
	EXPECT_FALSE(log::IsEnabled(log::Memory, log::ELevel::Verbose));
}

TEST_F(LogTest, TheLevelTableIsBoundedAndRefusesOneCategoryTooMany)
{
	for (uint32 i = 0u; i < log::MaxCategoryLevels; ++i)
	{
		EXPECT_TRUE(log::SetMinLevel(FString::Format("category{}", i), log::ELevel::Warning));
	}
	EXPECT_FALSE(log::SetMinLevel("one-too-many", log::ELevel::Warning));
	// Changing a category that has a level still works.
	EXPECT_TRUE(log::SetMinLevel("category7", log::ELevel::Error));
	EXPECT_EQ(log::GetMinLevel("category7"), log::ELevel::Error);
	EXPECT_EQ(log::GetMinLevel("one-too-many"), log::ELevel::Info);
}

TEST_F(LogTest, LongCategoryNamesAreCutForLevelsButPrintedInFull)
{
	const FString name = FString::Repeat("x", log::MaxCategoryLength + 5u);
	ASSERT_TRUE(log::SetMinLevel(name, log::ELevel::Error));
	EXPECT_EQ(log::GetMinLevel(name), log::ELevel::Error);

	test::CLogCapture capture;
	log::Write(name, log::ELevel::Error, "message");
	ASSERT_EQ(capture.GetEntries().size(), 1u);
	EXPECT_EQ(capture.GetEntries()[0].Category, name);
}

TEST_F(LogTest, LevelNamesRoundTrip)
{
	for (const log::ELevel level : { log::ELevel::Error, log::ELevel::Warning, log::ELevel::Info, log::ELevel::Verbose })
	{
		EXPECT_EQ(log::ParseLevel(log::GetLevelName(level)), level);
	}
	EXPECT_EQ(log::ParseLevel("WARNING"), log::ELevel::Warning);
	EXPECT_EQ(log::ParseLevel("Verbose"), log::ELevel::Verbose);
	EXPECT_FALSE(log::ParseLevel("loud").has_value());
}

TEST_F(LogTest, LinesFromManyThreadsNeverInterleave)
{
	constexpr int threads = 8;
	constexpr int perThread = 500;
	test::CLogCapture capture;
	std::vector<std::thread> workers;
	for (int t = 0; t < threads; ++t)
	{
		workers.emplace_back([t] ()
		{
			for (int i = 0; i < perThread; ++i)
			{
				log::Info(log::Assets, "thread {} message {}", t, i);
			}
		});
	}
	for (std::thread& worker : workers)
	{
		worker.join();
	}

	const std::vector<test::CLogCapture::SEntry>& entries = capture.GetEntries();
	ASSERT_EQ(entries.size(), static_cast<size_t>(threads * perThread));
	std::vector<int> next(threads, 0);
	for (const test::CLogCapture::SEntry& entry : entries)
	{
		int thread = -1;
		for (int t = 0; t < threads; ++t)
		{
			if (entry.Text.StartsWith(FString::Format("thread {} ", t)))
			{
				thread = t;
			}
		}
		ASSERT_GE(thread, 0) << entry.Text.CStr();
		// Each thread's lines arrive whole and in its own order.
		const int index = next[thread]++;
		EXPECT_EQ(entry.Text, FString::Format("thread {} message {}", thread, index));
		EXPECT_EQ(entry.Line, FString::Format("[assets] thread {} message {}\n", thread, index));
	}
}
