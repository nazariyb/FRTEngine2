// platform::FillRandomBytes (Platform/Random.h) fills exactly the span it is given, with bytes that
// differ between calls: the property SGuid::NewRandom builds unique identities on.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "Platform/Random.h"

using namespace frt;


TEST(RandomTest, FillsTheWholeSpanAndOnlyIt)
{
	constexpr size_t count = 16384u;
	std::vector<uint8> buffer(count + 2u, 0xCCu);
	platform::FillRandomBytes(std::span(buffer).subspan(1u, count));
	EXPECT_EQ(buffer.front(), 0xCCu);
	EXPECT_EQ(buffer.back(), 0xCCu);

	// Every byte value turns up in 16 KiB of random bytes (one is missing with p about 4e-26).
	std::array<bool, 256> seen = {};
	for (size_t i = 1u; i <= count; ++i)
	{
		seen[buffer[i]] = true;
	}
	EXPECT_EQ(std::count(seen.begin(), seen.end(), true), 256);
}

TEST(RandomTest, CallsDrawDifferentBytes)
{
	std::array<uint8, 32> first = {};
	std::array<uint8, 32> second = {};
	platform::FillRandomBytes(first);
	platform::FillRandomBytes(second);
	EXPECT_NE(first, second);
}

TEST(RandomTest, EmptySpanIsANoOp)
{
	platform::FillRandomBytes({});
	SUCCEED();
}
