#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"


struct ImFont;
struct ImFontAtlas;


namespace frt::ui
{
/**
 * The UI font: Roboto Medium (Latin, Greek, Cyrillic) with fallbacks merged into the same ImFont
 * for what it lacks - Noto Sans CJK (ideographs, kana, Hangul, and the symbols of the CJK
 * character sets), Noto Emoji (monochrome) and Noto Sans Symbols 2. Text in any mix of scripts
 * draws with the one font. The files are in the engine's Content/Fonts; README.md there lists
 * what each covers and how the fallbacks were cut down to it (Tools/Fonts/SubsetFonts.py).
 *
 * ImGui 1.92 rasterises a glyph the first time it is drawn, at the size it is drawn, so the
 * fallbacks cost what their files hold in memory (about 7.5 MB, kept here), not atlas space.
 *
 * Every fallback is drawn at the em size the primary font has: ImGui sizes each source by its own
 * line height (hhea ascender - descender, stbtt_ScaleForPixelHeight), and Noto Sans CJK's is 1.45
 * em where Roboto's is 1.17, so at the same size its ideographs would come out a fifth too small.
 *
 * Headless builds have no ImGui: these are declared there, but defined only where ImGui is.
 */
class FRT_CORE_API CFontSet
{
public:
	// Adds the font to Atlas at SizePixels and returns it: the atlas's default font if it is the
	// first. A missing fallback is left out; without Roboto it is ImGui's built-in font (ASCII).
	ImFont* AddTo (ImFontAtlas& Atlas, const std::filesystem::path& FontDir, float SizePixels);

	// Frees the font files, which the atlas reads until it is destroyed.
	void Release ();

	// Bytes read: the files the atlas holds on to.
	size_t GetMemoryBytes () const;

private:
	ImFont* AddFile (ImFontAtlas& Atlas, const std::filesystem::path& Path, std::vector<uint8> Bytes, float SizePixels, bool bMerge);

	std::vector<std::vector<uint8>> FontData;
};

// A TrueType / OpenType file's line height in ems, (hhea ascender - descender) / head unitsPerEm:
// what ImGui's rasteriser (stb_truetype) divides a font's size by to scale it. 0 if the file has
// no such tables.
FRT_CORE_API float GetLineHeightInEms (std::span<const uint8> FontFile);
}
