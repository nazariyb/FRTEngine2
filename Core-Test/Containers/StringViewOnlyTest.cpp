// Deliberately the first include and the only engine header: StringView.h must be usable on its own
// (game modules and the Launcher build without a precompiled header). The std::formatter and std::hash
// specializations have to come with it, or a TU that sees only the view would format it as a range of
// chars (['a', 'b']) and an explicit specialization seen later in another TU would be an ODR violation.
#include "Containers/StringView.h"

#include <gtest/gtest.h>

using namespace frt;


TEST(StringViewOnly, FormatterHonoursTheStringSpec)
{
	EXPECT_EQ(std::format("{:>5}", FStringView("ab")), "   ab");
	EXPECT_EQ(std::format("{:<4}|", FStringView("ab")), "ab  |");
	EXPECT_EQ(std::format("{:.1}", FStringView("ab")), "a");

	// Formatted as text, not through the library's range formatter.
	EXPECT_EQ(std::format("{}", FStringView("ab")), "ab");
	EXPECT_EQ(std::format("[{}]", FStringView()), "[]");
}

TEST(StringViewOnly, FormatterForLiterals)
{
	EXPECT_EQ(std::format("{}", FStringLiteral("x")), "x");

	constexpr FStringLiteral Name = "Name";
	EXPECT_EQ(std::format("{:*^8}", Name), "**Name**");
}

TEST(StringViewOnly, StdHashMatchesGetHash)
{
	EXPECT_EQ(std::hash<FStringView>{}("ab"), static_cast<size_t>(FStringView("ab").GetHash()));

	constexpr FStringLiteral Literal = "ab";
	EXPECT_EQ(std::hash<FStringLiteral>{}(Literal), std::hash<FStringView>{}("ab"));
	EXPECT_EQ(std::hash<FStringView>{}(FStringView()), std::hash<FStringView>{}(""));
}
