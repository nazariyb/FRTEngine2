#include <gtest/gtest.h>

#include "TestCommon.h"

#if !defined(FRT_HEADLESS)

#include <cstdint>
#include <optional>
#include <vector>

#include "imgui.h"

#include "Core.h"
#include "EnginePaths.h"
#include "Platform/Platform.h"
#include "UI/ImGuiFonts.h"

using namespace frt;


namespace
{
std::filesystem::path FontDir ()
{
	return paths::GetEngineContentDir() / "Fonts";
}

// The UI font in an atlas of its own, as CImGuiLayer::StartUp adds it to ImGui's.
class ImGuiFontsTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Atlas = IM_NEW(ImFontAtlas)();
		Font = Fonts.AddTo(*Atlas, FontDir(), Size);
		ASSERT_NE(Font, nullptr);
	}

	void TearDown () override
	{
		IM_DELETE(Atlas);
		Fonts.Release();
	}

	static constexpr float Size = 16.0f;
	ImFontAtlas* Atlas = nullptr;
	ImFont* Font = nullptr;
	ui::CFontSet Fonts;
};
}

TEST_F(ImGuiFontsTest, LoadsTheFallbacks)
{
	// Roboto and three fallbacks, all in the one font.
	EXPECT_EQ(Font->Sources.Size, 4);
	EXPECT_GT(Fonts.GetMemoryBytes(), size_t(5u) << 20);
}

TEST_F(ImGuiFontsTest, HasAGlyphForEveryScriptTheEditorShows)
{
	struct SCase
	{
		char32_t CodePoint;
		const char* What;
	};
	static constexpr SCase cases[] =
	{
		{ U'A', "Latin" }, { U'é', "Latin-1" }, { U'ő', "Latin Extended-A" },
		{ U'ґ', "Ukrainian ghe with upturn" }, { U'Є', "Ukrainian ie" }, { U'ї', "Ukrainian yi" },
		{ U'ж', "Cyrillic zhe" }, { U'Ω', "Greek capital omega" }, { U'λ', "Greek lambda" },
		{ U'水', "CJK ideograph (water)" }, { U'镕', "CJK ideograph outside GB 2312" },
		{ U'あ', "Hiragana" }, { U'カ', "Katakana" }, { U'。', "ideographic full stop" },
		{ U'한', "Hangul syllable" }, { U'똠', "Hangul syllable outside KS X 1001" },
		{ U'！', "fullwidth exclamation mark" },
		{ U'→', "arrow" }, { U'✓', "check mark" }, { U'★', "star" }, { U'▶', "play triangle" },
		{ U'─', "box drawing" }, { U'⏻', "power symbol" }, { U'❤', "heart" },
		{ U'\U0001F600', "emoji (grinning face)" }, { U'\U0001F3AE', "emoji (video game)" }, { U'\U0001F680', "emoji (rocket)" },
	};
	for (const SCase& c : cases)
	{
		EXPECT_TRUE(Font->IsGlyphInFont(static_cast<ImWchar>(c.CodePoint)))
			<< c.What << ": U+" << std::hex << static_cast<uint32_t>(c.CodePoint);
	}
}

TEST_F(ImGuiFontsTest, FallbacksDrawAtThePrimaryFontsEmSize)
{
	// An ideograph is one em wide. Unscaled, ImGui would size Noto Sans CJK by its own line
	// height (1.448 em) and draw it at 0.81 of Roboto's em.
	const std::optional<std::vector<uint8>> roboto = platform::ReadFileBytes(FontDir() / "Roboto-Medium.ttf");
	ASSERT_TRUE(roboto);
	const float em = Size / ui::GetLineHeightInEms(*roboto);
	ImFontBaked* baked = Font->GetFontBaked(Size);
	ASSERT_NE(baked, nullptr);
	EXPECT_NEAR(baked->GetCharAdvance(static_cast<ImWchar>(U'水')), em, 0.05f);
	EXPECT_NEAR(baked->GetCharAdvance(static_cast<ImWchar>(U'あ')), em, 0.05f);
}

TEST(ImGuiFonts, LineHeightInEmsReadsHeadAndHhea)
{
	const std::optional<std::vector<uint8>> roboto = platform::ReadFileBytes(FontDir() / "Roboto-Medium.ttf");
	const std::optional<std::vector<uint8>> cjk = platform::ReadFileBytes(FontDir() / "NotoSansCJKsc-Medium-Subset.otf");
	ASSERT_TRUE(roboto);
	ASSERT_TRUE(cjk);
	EXPECT_FLOAT_EQ(ui::GetLineHeightInEms(*roboto), (1900.0f + 500.0f) / 2048.0f);   // TrueType outlines
	EXPECT_FLOAT_EQ(ui::GetLineHeightInEms(*cjk), (1160.0f + 288.0f) / 1000.0f);      // CFF ('OTTO')

	// Anything else is 0: no scale rather than a wrong one.
	EXPECT_EQ(ui::GetLineHeightInEms({}), 0.0f);
	std::vector<uint8> truncated(roboto->begin(), roboto->begin() + 40);
	EXPECT_EQ(ui::GetLineHeightInEms(truncated), 0.0f);
	std::vector<uint8> noTables(12u, uint8(0u));
	EXPECT_EQ(ui::GetLineHeightInEms(noTables), 0.0f);
}

#endif // !FRT_HEADLESS
