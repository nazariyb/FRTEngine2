// The display settings keep the resolution and refresh rate as text (Engine.ini's [Display]): Parse
// must take back what ToString writes and reject anything else whole, never leaving half a
// resolution behind, and refresh-rate text of any kind must give an index.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include "Containers/String.h"
#include "Graphics/Render/RenderCommonTypes.h"

using namespace frt;
using namespace frt::graphics;


TEST(Resolution, ParseReadsWhatToStringWrites)
{
	EXPECT_EQ(SResolution(1920, 1080).ToString(), "1920x1080");
	EXPECT_EQ(SResolution(800, 600).ToString(':'), "800:600");
	EXPECT_TRUE(SResolution().ToString().IsEmpty());

	for (const char delimiter : { 'x', 'X', ':', '|', '-' })
	{
		const FString text = SResolution(2560, 1440).ToString(delimiter);
		SResolution read;
		EXPECT_TRUE(read.Parse(text)) << text;
		EXPECT_TRUE(read.IsValid()) << text;
		EXPECT_EQ(read.GetWidth(), 2560) << text;
		EXPECT_EQ(read.GetHeight(), 1440) << text;
	}
}

TEST(Resolution, ParseRejectsAnythingElseWhole)
{
	constexpr FStringLiteral rejected[] = {
		"", "1920", "x1080", "1920x", "19 20x1080", " 1920x1080", "1920x1080 ", "1920x1080x", "1920x-1080",
		"70000x1080", "1920.5x1080", "١٩٢٠x1080", "1920×1080"
	};
	for (const FStringLiteral text : rejected)
	{
		SResolution resolution(1280, 720);
		EXPECT_FALSE(resolution.Parse(text)) << text.CStr();
		EXPECT_FALSE(resolution.IsValid()) << text.CStr();
		EXPECT_EQ(resolution.GetWidth(), SResolution::InvalidComponent) << text.CStr();
		EXPECT_EQ(resolution.GetHeight(), SResolution::InvalidHeight) << text.CStr();
		EXPECT_TRUE(resolution.ToString().IsEmpty()) << text.CStr();
	}
}

// The rate overloads it ends in are still stubs answering DefaultIndex; the text overload's own job is
// to read whatever Engine.ini holds without failing.
TEST(DisplayOptions, RefreshRateTextOfAnyKindGivesAnIndex)
{
	const SDisplayOptions options {};
	constexpr FStringLiteral texts[] = { "60", "59.94", "", "abc", "60Hz", "-60", "nan", "inf", "1e40" };
	for (const FStringLiteral text : texts)
	{
		EXPECT_EQ(options.GetRefreshRateIndex(0, text), SDisplayOptions::DefaultIndex) << text.CStr();
	}
}
