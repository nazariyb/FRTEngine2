#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "Enum.h"
#include "Config/Config.h"

using frt::config::CConfig;
using frt::config::EConfigLayer;


// A reflected enum owned by the test, so the layering and round-trip behaviour is checked
// without depending on any engine enum staying the shape it is today.
enum class ETestMode : int32
{
	Off = 0,
	Low,
	High,
};

FRT_DECLARE_ENUM_REFLECTION(
	ETestMode,
	FRT_ENUM_ENTRY(ETestMode, Off),
	FRT_ENUM_ENTRY(ETestMode, Low),
	FRT_ENUM_ENTRY(ETestMode, High));


namespace
{
std::filesystem::path MakeTempPath (const char* Name)
{
	return std::filesystem::temp_directory_path() / "FrtConfigTest" / Name;
}


std::filesystem::path WriteLayerFile (const char* Name, const std::string& Contents)
{
	const std::filesystem::path path = MakeTempPath(Name);
	std::filesystem::create_directories(path.parent_path());

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << Contents;
	out.close();

	return path;
}
}


TEST(ConfigTest, MissingKeyReturnsTheCallersDefault)
{
	CConfig config;

	EXPECT_EQ(config.Get("Display", "MonitorIndex", 3), 3);
	EXPECT_EQ(config.Get("Display", "Scale", 1.5f), 1.5f);
	EXPECT_TRUE(config.Get("Display", "VSync", true));
	EXPECT_EQ(config.Get("Display", "Name", "fallback"), "fallback");
	EXPECT_EQ(config.Get("Display", "Mode", ETestMode::High), ETestMode::High);
	EXPECT_FALSE(config.Has("Display", "MonitorIndex"));
}


TEST(ConfigTest, StrongerLayerWinsAndWeakerStillShowsThrough)
{
	CConfig config;

	const std::filesystem::path engine =
		WriteLayerFile("engine.ini", "[Display]\nMonitorIndex=0\nVSync=true\nOnlyEngine=7\n");
	const std::filesystem::path project =
		WriteLayerFile("project.ini", "[Display]\nMonitorIndex=1\n");
	const std::filesystem::path user =
		WriteLayerFile("user.ini", "[Display]\nVSync=false\n");

	ASSERT_TRUE(config.LoadLayer(EConfigLayer::Engine, engine));
	ASSERT_TRUE(config.LoadLayer(EConfigLayer::Project, project));
	ASSERT_TRUE(config.LoadLayer(EConfigLayer::User, user));

	EXPECT_EQ(config.Get("Display", "MonitorIndex", -1), 1);  // project over engine
	EXPECT_FALSE(config.Get("Display", "VSync", true));       // user over engine
	EXPECT_EQ(config.Get("Display", "OnlyEngine", -1), 7);    // only engine has it

	std::filesystem::remove_all(engine.parent_path());
}


TEST(ConfigTest, MissingLayerFileIsSkippedNotFatal)
{
	CConfig config;

	const std::filesystem::path engine =
		WriteLayerFile("engine-only.ini", "[Display]\nMonitorIndex=2\n");

	ASSERT_TRUE(config.LoadLayer(EConfigLayer::Engine, engine));
	EXPECT_FALSE(config.LoadLayer(EConfigLayer::User, MakeTempPath("absent.ini")));

	EXPECT_EQ(config.Get("Display", "MonitorIndex", -1), 2);

	std::filesystem::remove_all(engine.parent_path());
}


TEST(ConfigTest, ParsesBoolSpellings)
{
	CConfig config;
	config.LoadLayer(EConfigLayer::Engine, WriteLayerFile("bools.ini",
		"[B]\nA=true\nB=TRUE\nC=1\nD=yes\nE=on\nF=false\nG=0\nH=no\nI=off\nJ=maybe\n"));

	EXPECT_TRUE(config.Get("B", "A", false));
	EXPECT_TRUE(config.Get("B", "B", false));
	EXPECT_TRUE(config.Get("B", "C", false));
	EXPECT_TRUE(config.Get("B", "D", false));
	EXPECT_TRUE(config.Get("B", "E", false));

	EXPECT_FALSE(config.Get("B", "F", true));
	EXPECT_FALSE(config.Get("B", "G", true));
	EXPECT_FALSE(config.Get("B", "H", true));
	EXPECT_FALSE(config.Get("B", "I", true));

	// Unrecognised spelling falls back rather than reading as false, so a typo cannot
	// silently turn a feature off.
	EXPECT_TRUE(config.Get("B", "J", true));

	std::filesystem::remove_all(MakeTempPath("bools.ini").parent_path());
}


TEST(ConfigTest, PartiallyNumericValuesFallBackRatherThanTruncate)
{
	CConfig config;
	config.LoadLayer(EConfigLayer::Engine, WriteLayerFile("numbers.ini",
		"[N]\nRes=1920x1080\nCount=12abc\nGood=1920\nNegative=-5\nReal=0.25\nEmpty=\n"));

	EXPECT_EQ(config.Get("N", "Res", -1), -1);
	EXPECT_EQ(config.Get("N", "Count", -1), -1);
	EXPECT_EQ(config.Get("N", "Good", -1), 1920);
	EXPECT_EQ(config.Get("N", "Negative", 0), -5);
	EXPECT_EQ(config.Get("N", "Real", 0.f), 0.25f);
	EXPECT_EQ(config.Get("N", "Empty", -1), -1);

	// A negative in the file must not wrap when read as unsigned.
	EXPECT_EQ(config.Get("N", "Negative", 99u), 99u);

	std::filesystem::remove_all(MakeTempPath("numbers.ini").parent_path());
}


TEST(ConfigTest, EnumsRoundTripByName)
{
	CConfig config;
	config.LoadLayer(EConfigLayer::Engine, WriteLayerFile("enums.ini",
		"[E]\nMode=High\nLower=low\nBogus=Sideways\nNumeric=1\n"));

	EXPECT_EQ(config.Get("E", "Mode", ETestMode::Off), ETestMode::High);

	// Matching is case-insensitive, like the rest of the format.
	EXPECT_EQ(config.Get("E", "Lower", ETestMode::Off), ETestMode::Low);

	// A name that is not in the reflected set falls back.
	EXPECT_EQ(config.Get("E", "Bogus", ETestMode::Off), ETestMode::Off);

	// An integer is NOT accepted: that is the whole reason enums are stored by name.
	EXPECT_EQ(config.Get("E", "Numeric", ETestMode::Off), ETestMode::Off);

	std::filesystem::remove_all(MakeTempPath("enums.ini").parent_path());
}


TEST(ConfigTest, SetWritesToTheUserLayerAndShadowsWeakerOnes)
{
	CConfig config;
	config.LoadLayer(EConfigLayer::Engine, WriteLayerFile("shadow.ini",
		"[Display]\nMonitorIndex=0\n"));

	EXPECT_EQ(config.Get("Display", "MonitorIndex", -1), 0);

	config.Set("Display", "MonitorIndex", 2);
	EXPECT_EQ(config.Get("Display", "MonitorIndex", -1), 2);

	// Dropping the user layer exposes the engine value again, unmodified.
	config.ResetUserLayer();
	EXPECT_EQ(config.Get("Display", "MonitorIndex", -1), 0);

	std::filesystem::remove_all(MakeTempPath("shadow.ini").parent_path());
}


TEST(ConfigTest, SaveThenLoadRoundTripsEveryType)
{
	const std::filesystem::path userPath = MakeTempPath("save-roundtrip.ini");
	std::filesystem::remove(userPath);

	{
		CConfig config;
		config.SetUserLayerPath(userPath);

		config.Set("T", "Flag", false);
		config.Set("T", "Signed", -42);
		config.Set("T", "Unsigned", 4000000000u);
		config.Set("T", "Real", 0.125f);
		config.Set("T", "Text", "some value");
		config.Set("T", "Mode", ETestMode::Low);

		ASSERT_TRUE(config.Save());
	}

	CConfig reloaded;
	ASSERT_TRUE(reloaded.LoadLayer(EConfigLayer::User, userPath));

	EXPECT_FALSE(reloaded.Get("T", "Flag", true));
	EXPECT_EQ(reloaded.Get("T", "Signed", 0), -42);
	EXPECT_EQ(reloaded.Get("T", "Unsigned", 0u), 4000000000u);
	EXPECT_EQ(reloaded.Get("T", "Real", 0.f), 0.125f);
	EXPECT_EQ(reloaded.Get("T", "Text", ""), "some value");
	EXPECT_EQ(reloaded.Get("T", "Mode", ETestMode::Off), ETestMode::Low);

	std::filesystem::remove_all(userPath.parent_path());
}


TEST(ConfigTest, FloatSurvivesRepeatedSaveLoadWithoutDrifting)
{
	const std::filesystem::path userPath = MakeTempPath("float-drift.ini");
	std::filesystem::remove(userPath);

	constexpr float original = 0.1234567f;

	CConfig config;
	config.SetUserLayerPath(userPath);
	config.Set("T", "Value", original);

	for (int32 i = 0; i < 5; ++i)
	{
		ASSERT_TRUE(config.Save());

		CConfig reloaded;
		ASSERT_TRUE(reloaded.LoadLayer(EConfigLayer::User, userPath));
		EXPECT_EQ(reloaded.Get("T", "Value", 0.f), original);

		config = CConfig();
		config.SetUserLayerPath(userPath);
		config.Set("T", "Value", reloaded.Get("T", "Value", 0.f));
	}

	std::filesystem::remove_all(userPath.parent_path());
}


TEST(ConfigTest, SaveOnlyWritesTheUserLayer)
{
	const std::filesystem::path enginePath =
		WriteLayerFile("engine-untouched.ini", "[Display]\nMonitorIndex=0\n");
	const std::filesystem::path userPath = MakeTempPath("user-written.ini");
	std::filesystem::remove(userPath);

	CConfig config;
	ASSERT_TRUE(config.LoadLayer(EConfigLayer::Engine, enginePath));
	config.SetUserLayerPath(userPath);
	config.Set("Display", "MonitorIndex", 1);
	ASSERT_TRUE(config.Save());

	std::string engineContents;
	{
		std::ifstream in(enginePath, std::ios::binary);
		engineContents.assign(
			(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
	EXPECT_EQ(engineContents, "[Display]\nMonitorIndex=0\n");

	std::filesystem::remove_all(enginePath.parent_path());
}


TEST(ConfigTest, LoadingTheUserLayerRecordsTheSaveTarget)
{
	const std::filesystem::path userPath = MakeTempPath("target.ini");
	std::filesystem::remove(userPath);

	CConfig config;
	// Absent on a first run, but the path still has to be remembered so Save has somewhere
	// to go.
	EXPECT_FALSE(config.LoadLayer(EConfigLayer::User, userPath));
	EXPECT_EQ(config.GetUserLayerPath(), userPath);

	config.Set("Display", "MonitorIndex", 1);
	EXPECT_TRUE(config.Save());
	EXPECT_TRUE(std::filesystem::exists(userPath));

	std::filesystem::remove_all(userPath.parent_path());
}


TEST(ConfigTest, SaveWithoutAPathFails)
{
	CConfig config;
	config.Set("Display", "MonitorIndex", 1);

	EXPECT_FALSE(config.Save());
}


TEST(ConfigTest, StringLiteralDefaultsDoNotResolveToTheBoolOverload)
{
	// Guards the const char* overloads: without them `Get(S, K, "text")` picks bool,
	// because const char* to bool is a standard conversion and beats string_view.
	CConfig config;
	config.Set("T", "Text", "written");

	EXPECT_EQ(config.Get("T", "Text", "fallback"), "written");
	EXPECT_EQ(config.Get("T", "Absent", "fallback"), "fallback");
}
