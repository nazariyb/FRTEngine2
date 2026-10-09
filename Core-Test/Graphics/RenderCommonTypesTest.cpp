// The display settings keep the resolution and refresh rate as text (Engine.ini's [Display]): Parse
// must take back what ToString writes and reject anything else whole, never leaving half a
// resolution behind, and refresh-rate text of any kind must give an index.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include "Config/Config.h"
#include "Containers/String.h"
#include "Graphics/Render/RenderCommonTypes.h"
#include "User/UserSettings.h"

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

// The display settings read the config's text whole: a value longer than any the panel writes is still
// that value, and a typo stays the typo (no monitor offers it, so the default mode is used), never cut
// down to a prefix that happens to be another mode.
TEST(DisplaySettings, LoadKeepsTheTextWhole)
{
	const auto load = [] (FStringView Resolution, FStringView RefreshRate)
	{
		config::CConfig config;
		config.SetOverride(names::DisplaySection, "Resolution", Resolution);
		config.SetOverride(names::DisplaySection, "RefreshRate", RefreshRate);
		SDisplaySettings settings;
		LoadDisplaySettings(config, settings);
		return settings;
	};
	// What SDisplaySettings::GetResolution makes of the text (it is Core's own, not exported).
	const auto parse = [] (FStringView Text)
	{
		SResolution resolution;
		resolution.Parse(Text);
		return resolution;
	};

	const SDisplaySettings wide = load("10240x4320", "143.856");
	EXPECT_EQ(wide.ResolutionTxt, "10240x4320");
	EXPECT_EQ(wide.RefreshRateTxt, "143.856");
	EXPECT_EQ(parse(wide.ResolutionTxt), SResolution(10240, 4320));

	// "1920x10800" is no monitor's mode; its first nine bytes would be one.
	const SDisplaySettings typo = load("1920x10800", "60");
	EXPECT_EQ(typo.ResolutionTxt, "1920x10800");
	EXPECT_EQ(parse(typo.ResolutionTxt), SResolution(1920, 10800));
	// And text after a resolution makes it no resolution at all, rather than being cut off.
	EXPECT_FALSE(parse(load("1920x1080@60", "60").ResolutionTxt).IsValid());

	const SDisplaySettings common = load("3840x2160", "60");
	EXPECT_EQ(parse(common.ResolutionTxt), SResolution(3840, 2160));
	EXPECT_EQ(common.RefreshRateTxt, "60");
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
