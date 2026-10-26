// The Texture importer's Analyze (E3, design §7.11): every image format the importer claims yields Width, Height,
// Channels and HasAlpha from its header alone - read through Platform/ and handed to stb as bytes - including a JPEG
// whose frame header lies past the 64 KiB prefix, and a file stb cannot read is AnalyzeFailed with a code-0 error
// while its record keeps the texture type.
#include <gtest/gtest.h>

#include <vector>

#include "ImporterTestUtils.h"

#include "Platform/Platform.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
void AppendU32BE (std::vector<uint8>& Out, uint32 Value)
{
	for (int shift = 24; shift >= 0; shift -= 8)
	{
		Out.push_back(static_cast<uint8>(Value >> shift));
	}
}

void AppendU16LE (std::vector<uint8>& Out, uint32 Value)
{
	Out.push_back(static_cast<uint8>(Value));
	Out.push_back(static_cast<uint8>(Value >> 8u));
}

void AppendU32LE (std::vector<uint8>& Out, uint32 Value)
{
	AppendU16LE(Out, Value & 0xFFFFu);
	AppendU16LE(Out, Value >> 16u);
}

// A PNG chunk (stb reads but never checks the CRC; a zero one keeps the fixtures short).
void AppendPngChunk (std::vector<uint8>& Out, const char (&Type)[5], std::initializer_list<uint8> Data)
{
	AppendU32BE(Out, static_cast<uint32>(Data.size()));
	Out.insert(Out.end(), Type, Type + 4);
	Out.insert(Out.end(), Data.begin(), Data.end());
	AppendU32BE(Out, 0u);
}

// A paletted PNG's header: signature, IHDR, PLTE, optional tRNS, then the start of the image data.
std::vector<uint8> MakePalettedPngHeader (uint32 Width, uint32 Height, bool bTransparent)
{
	std::vector<uint8> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	std::vector<uint8> ihdr;
	AppendU32BE(ihdr, Width);
	AppendU32BE(ihdr, Height);
	ihdr.insert(ihdr.end(), { 8u, 3u, 0u, 0u, 0u });
	AppendU32BE(png, 13u);
	png.insert(png.end(), { 'I', 'H', 'D', 'R' });
	png.insert(png.end(), ihdr.begin(), ihdr.end());
	AppendU32BE(png, 0u);
	AppendPngChunk(png, "PLTE", { 255u, 0u, 0u });
	if (bTransparent)
	{
		AppendPngChunk(png, "tRNS", { 128u });
	}
	AppendPngChunk(png, "IDAT", {});
	return png;
}

// An uncompressed 24- or 32-bit TGA header followed by its pixels.
std::vector<uint8> MakeTga (uint32 Width, uint32 Height, uint32 Bits)
{
	std::vector<uint8> tga = { 0u, 0u, 2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
	AppendU16LE(tga, Width);
	AppendU16LE(tga, Height);
	tga.push_back(static_cast<uint8>(Bits));
	tga.push_back(Bits == 32u ? 8u : 0u);
	tga.insert(tga.end(), static_cast<size_t>(Width) * Height * (Bits / 8u), uint8(0x7F));
	return tga;
}

// A 24-bit BMP (BITMAPINFOHEADER) with its pixel rows.
std::vector<uint8> MakeBmp (uint32 Width, uint32 Height)
{
	const uint32 rowBytes = (Width * 3u + 3u) & ~3u;
	const uint32 pixels = rowBytes * Height;
	std::vector<uint8> bmp = { 'B', 'M' };
	AppendU32LE(bmp, 54u + pixels);
	AppendU32LE(bmp, 0u);
	AppendU32LE(bmp, 54u);
	AppendU32LE(bmp, 40u);
	AppendU32LE(bmp, Width);
	AppendU32LE(bmp, Height);
	AppendU16LE(bmp, 1u);
	AppendU16LE(bmp, 24u);
	AppendU32LE(bmp, 0u);
	AppendU32LE(bmp, pixels);
	AppendU32LE(bmp, 2835u);
	AppendU32LE(bmp, 2835u);
	AppendU32LE(bmp, 0u);
	AppendU32LE(bmp, 0u);
	bmp.insert(bmp.end(), pixels, uint8(0x40));
	return bmp;
}

void ExpectTexture (const SAnalyzeResult& Result, int64 Width, int64 Height, int64 Channels, bool bAlpha)
{
	ASSERT_EQ(Result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(Result).CStr();
	EXPECT_EQ(Result.Type, asset_types::Texture);
	ASSERT_EQ(Result.Tags.size(), 4u);
	EXPECT_EQ(Result.Tags[0].Key, "Width");
	EXPECT_EQ(GetIntTag(Result.Tags, "Width"), Width);
	EXPECT_EQ(GetIntTag(Result.Tags, "Height"), Height);
	EXPECT_EQ(GetIntTag(Result.Tags, "Channels"), Channels);
	const STagDecl* alpha = FindTag(Result.Tags, "HasAlpha");
	ASSERT_NE(alpha, nullptr);
	EXPECT_EQ(alpha->Value, STagValue::FromBool(bAlpha));
	EXPECT_TRUE(Result.Diagnostics.empty()) << DescribeDiagnostics(Result).CStr();
	EXPECT_TRUE(Result.SubEntries.empty());
	EXPECT_TRUE(Result.Edges.empty());
	EXPECT_FALSE(Result.ContentHash.has_value());   // headers only: nothing hashed
}
}


TEST(TextureImporterTest, DemoImagesGiveTheirHeaderFacts)
{
	const CMountTable mounts = ResolveDemoMounts();
	ExpectTexture(AnalyzeAt(mounts, "/Project/Models/Cube/Cube_BaseColor.png"), 512, 512, 4, true);
	ExpectTexture(AnalyzeAt(mounts, "/Project/Models/Sponza/white.png"), 4, 4, 3, false);
	ExpectTexture(AnalyzeAt(mounts, "/Project/Models/Sponza/8773302468495022225.jpg"), 1024, 1024, 3, false);

	// DuckCM.png is paletted: 3 or 4 channels by its tRNS chunk, as stb counts them.
	const SAnalyzeResult duck = AnalyzeAt(mounts, "/Project/Models/Duck/DuckCM.png");
	ASSERT_EQ(duck.Status, EImportStatus::Analyzed) << DescribeDiagnostics(duck).CStr();
	EXPECT_EQ(GetIntTag(duck.Tags, "Width"), 512);
	EXPECT_EQ(GetIntTag(duck.Tags, "Height"), 512);
}

TEST(TextureImporterTest, PalettedPngCountsTransparencyAsAlpha)
{
	const CImporterTree tree("Paletted");
	tree.WriteBinary("Тексту́ри/opaque.png", MakePalettedPngHeader(7u, 9u, false));
	tree.WriteBinary("Тексту́ри/clear.png", MakePalettedPngHeader(7u, 9u, true));
	ExpectTexture(tree.Analyze("Тексту́ри/opaque.png"), 7, 9, 3, false);
	ExpectTexture(tree.Analyze("Тексту́ри/clear.png"), 7, 9, 4, true);
}

TEST(TextureImporterTest, TgaAndBmpHeadersAreRead)
{
	const CImporterTree tree("TgaBmp");
	tree.WriteBinary("rgba.tga", MakeTga(5u, 3u, 32u));
	tree.WriteBinary("rgb.tga", MakeTga(6u, 2u, 24u));
	tree.WriteBinary("image.bmp", MakeBmp(10u, 4u));
	ExpectTexture(tree.Analyze("rgba.tga"), 5, 3, 4, true);
	ExpectTexture(tree.Analyze("rgb.tga"), 6, 2, 3, false);
	ExpectTexture(tree.Analyze("image.bmp"), 10, 4, 3, false);
}

TEST(TextureImporterTest, JpegWhoseFrameHeaderLiesPastThePrefixIsReadWhole)
{
	const std::optional<std::vector<uint8>> jpeg = platform::ReadFileBytes(
		paths::GetEngineDir() / "Templates" / "Demo" / "Content" / "Models" / "Sponza" / "8773302468495022225.jpg");
	ASSERT_TRUE(jpeg.has_value());
	ASSERT_GE(jpeg->size(), 4u);

	// SOI, then two 60 KiB APP15 segments (stb skips application segments), then the rest of the file: its SOF
	// lands past the 64 KiB prefix.
	std::vector<uint8> padded = { 0xFF, 0xD8 };
	for (int i = 0; i < 2; ++i)
	{
		constexpr uint32 SegmentBytes = 60u * 1024u;
		padded.insert(padded.end(), { 0xFF, 0xEF, static_cast<uint8>((SegmentBytes + 2u) >> 8u), static_cast<uint8>((SegmentBytes + 2u) & 0xFFu) });
		padded.insert(padded.end(), SegmentBytes, uint8(0x20));
	}
	padded.insert(padded.end(), jpeg->begin() + 2, jpeg->end());

	const CImporterTree tree("LateJpegHeader");
	tree.WriteBinary("late.jpeg", padded);
	ExpectTexture(tree.Analyze("late.jpeg"), 1024, 1024, 3, false);
}

TEST(TextureImporterTest, UnreadableImageFailsAndKeepsTheTextureType)
{
	const CImporterTree tree("Unreadable");
	tree.Write("broken.png", "this is not an image");
	tree.Write("empty.jpg", "");

	for (const FStringView file : { FStringView("broken.png"), FStringView("empty.jpg") })
	{
		const SAnalyzeResult result = tree.Analyze(file);
		EXPECT_EQ(result.Status, EImportStatus::AnalyzeFailed);
		EXPECT_EQ(result.Type, asset_types::Texture);
		EXPECT_TRUE(result.Tags.empty());
		ASSERT_EQ(result.Diagnostics.size(), 1u) << DescribeDiagnostics(result).CStr();
		const SDiagnostic& error = result.Diagnostics[0];
		EXPECT_EQ(error.Severity, ESeverity::Error);
		EXPECT_EQ(error.Code, 0u);
		EXPECT_EQ(error.Subject, CImporterTree::MountPath(file));
		EXPECT_EQ(error.File, FString::Format("Content/{}", file));
		EXPECT_FALSE(result.bSourceChanged);
	}
}

TEST(TextureImporterTest, MissingFileFailsWithAReason)
{
	const CImporterTree tree("Missing");
	const SAnalyzeResult result = tree.Analyze("gone.png");
	EXPECT_EQ(result.Status, EImportStatus::AnalyzeFailed);
	ASSERT_EQ(CountDiagnostics(result, ESeverity::Error, 0u), 1u) << DescribeDiagnostics(result).CStr();
	EXPECT_NE(FindDiagnostic(result, "cannot open"), nullptr);
}
