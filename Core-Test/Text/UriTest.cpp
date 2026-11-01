// URI escapes (Text/Uri.h): what glTF's buffer and image URIs need decoded to name a file - %XX sequences, any case of
// hex digit, multibyte UTF-8 - while a '%' without two hex digits stays, a text without escapes is nullopt (the caller
// keeps the name as written), and an escape of NUL is refused (the name would end there).
#include <gtest/gtest.h>

#include "TestCommon.h"

#include "Containers/String.h"

#include "Text/Uri.h"

using namespace frt;
using text::DecodeUriEscapes;


TEST(UriTest, EscapesDecodeToTheirBytesAndEverythingElseStays)
{
	EXPECT_EQ(DecodeUriEscapes("Brick%20Wall.png"), std::optional<FString>("Brick Wall.png"));
	EXPECT_EQ(DecodeUriEscapes("%D0%A2%d0%b5%D0%BA%D1%81%D1%82%D1%83%D1%80%D0%B0.png"), std::optional<FString>("Текстура.png"));
	EXPECT_EQ(DecodeUriEscapes("100%.png%2"), std::nullopt) << "no escape: the name as written";
	EXPECT_EQ(DecodeUriEscapes("a%zz%41"), std::optional<FString>("a%zzA"));
	EXPECT_EQ(DecodeUriEscapes("plain.bin"), std::nullopt);
	EXPECT_EQ(DecodeUriEscapes("x%00.bin"), std::nullopt) << "NUL would end the name";
	EXPECT_EQ(DecodeUriEscapes("%41"), std::optional<FString>("A"));
}
