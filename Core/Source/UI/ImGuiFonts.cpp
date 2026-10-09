#include "UI/ImGuiFonts.h"

#if !defined(FRT_HEADLESS)

#include <optional>

#include "imgui.h"

#include "Containers/String.h"
#include "Platform/Platform.h"


namespace frt::ui
{
namespace
{
constexpr FStringLiteral PrimaryFont = "Roboto-Medium.ttf";
// In merge order. Each holds only code points no font before it has (Tools/Fonts/SubsetFonts.py),
// so the order decides nothing but where ImGui looks first.
constexpr FStringLiteral FallbackFonts[] =
{
	"NotoSansCJKsc-Medium-Subset.otf",
	"NotoEmoji-Medium-Subset.ttf",
	"NotoSansSymbols2-Regular-Subset.ttf",
};

uint32 ReadBigEndian (std::span<const uint8> Bytes, size_t Offset, uint32 Size)
{
	uint32 value = 0u;
	for (uint32 i = 0u; i < Size; ++i)
	{
		value = (value << 8) | Bytes[Offset + i];
	}
	return value;
}

constexpr uint32 MakeTag (const char (&Tag)[5])
{
	uint32 value = 0u;
	for (uint32 i = 0u; i < 4u; ++i)
	{
		value = (value << 8) | static_cast<uint8>(Tag[i]);
	}
	return value;
}
}

float GetLineHeightInEms (std::span<const uint8> FontFile)
{
	// The sfnt header (OpenType "Organization of an OpenType Font"): version, numTables at 4, three
	// search fields; then a 16-byte record per table - tag, checksum, offset at 8, length at 12.
	if (FontFile.size() < 12u)
	{
		return 0.0f;
	}
	const uint32 tableCount = ReadBigEndian(FontFile, 4u, 2u);
	int32 unitsPerEm = 0;
	int32 ascender = 0;
	int32 descender = 0;
	for (uint32 i = 0u; i < tableCount; ++i)
	{
		const size_t record = 12u + 16u * static_cast<size_t>(i);
		if (record + 16u > FontFile.size())
		{
			return 0.0f;
		}
		const uint32 tag = ReadBigEndian(FontFile, record, 4u);
		const size_t offset = ReadBigEndian(FontFile, record + 8u, 4u);
		if (tag == MakeTag("head") && offset + 20u <= FontFile.size())
		{
			unitsPerEm = static_cast<int32>(ReadBigEndian(FontFile, offset + 18u, 2u));
		}
		else if (tag == MakeTag("hhea") && offset + 8u <= FontFile.size())
		{
			ascender = static_cast<int16>(ReadBigEndian(FontFile, offset + 4u, 2u));
			descender = static_cast<int16>(ReadBigEndian(FontFile, offset + 6u, 2u));
		}
	}
	return unitsPerEm > 0 && ascender > descender ? static_cast<float>(ascender - descender) / static_cast<float>(unitsPerEm) : 0.0f;
}

ImFont* CFontSet::AddTo (ImFontAtlas& Atlas, const std::filesystem::path& FontDir, float SizePixels)
{
	// Read through the UTF-16 path. A stripped content dir degrades to the built-in font.
	const std::filesystem::path primaryPath = FontDir / PrimaryFont.ToPath();
	std::optional<std::vector<uint8>> primary = platform::ReadFileBytes(primaryPath);
	if (!primary)
	{
		return Atlas.AddFontDefault();
	}
	FontData.reserve(FontData.size() + 1u + std::size(FallbackFonts));
	const float primaryEms = GetLineHeightInEms(*primary);
	ImFont* font = AddFile(Atlas, primaryPath, std::move(*primary), SizePixels, false);
	if (font == nullptr)
	{
		return Atlas.AddFontDefault();
	}

	for (const FStringLiteral name : FallbackFonts)
	{
		const std::filesystem::path path = FontDir / name.ToPath();
		if (std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(path))
		{
			// ImGui scales a merged source by SizePixels over the primary's, on top of its own
			// line-height scale: this size gives it the primary's em, as a browser's fallback
			// fonts get the size of the font they stand in for.
			const float ems = GetLineHeightInEms(*bytes);
			const float size = ems > 0.0f && primaryEms > 0.0f ? SizePixels * ems / primaryEms : SizePixels;
			AddFile(Atlas, path, std::move(*bytes), size, true);
		}
	}
	return font;
}

ImFont* CFontSet::AddFile (ImFontAtlas& Atlas, const std::filesystem::path& Path, std::vector<uint8> Bytes, float SizePixels, bool bMerge)
{
	// Moving the vector keeps its buffer where it is: the atlas holds a pointer into it.
	FontData.push_back(std::move(Bytes));
	std::vector<uint8>& data = FontData.back();
	ImFontConfig config;
	config.MergeMode = bMerge;
	config.FontDataOwnedByAtlas = false;
	// What AddFontFromFileTTF would name it (debug windows show it).
	FString::FromPath(Path.filename()).CopyTo(config.Name, static_cast<uint32>(sizeof(config.Name)));
	return Atlas.AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()), SizePixels, &config);
}

void CFontSet::Release ()
{
	FontData = std::vector<std::vector<uint8>>();
}

size_t CFontSet::GetMemoryBytes () const
{
	size_t bytes = 0u;
	for (const std::vector<uint8>& data : FontData)
	{
		bytes += data.size();
	}
	return bytes;
}
}

#endif // !FRT_HEADLESS
