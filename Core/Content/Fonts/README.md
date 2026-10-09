# Fonts

The UI font: one ImGui font (`ui::CFontSet`, `Core/Source/UI/ImGuiFonts.h`) made of Roboto and three
fallbacks merged into it, in this order. ImGui looks a character up in each in turn; each fallback
holds only what no font before it has.

| File | Covers | Size | Source | License |
|------|--------|------|--------|---------|
| `Roboto-Medium.ttf` | Latin (Latin-1, Extended-A, Vietnamese), Greek (modern), Cyrillic (Ukrainian included) | 159 KB | Copied from `ThirdParty/Imgui/misc/fonts/` | Apache License 2.0 |
| `NotoSansCJKsc-Medium-Subset.otf` | All 20,992 CJK Unified Ideographs (U+4E00..9FFF), compatibility ideographs, hiragana, katakana, bopomofo, all 11,172 Hangul syllables and jamo, CJK punctuation and fullwidth forms, and the arrows, box drawing, shapes, math and enclosed symbols of the CJK character sets | 6.1 MB | Noto Sans CJK SC Medium 2.004 | OFL 1.1 (`OFL.txt`) |
| `NotoEmoji-Medium-Subset.ttf` | Emoji, monochrome: 1,399 code points, at weight 500 | 737 KB | Noto Emoji 3.002 | OFL 1.1 (`OFL.txt`) |
| `NotoSansSymbols2-Regular-Subset.ttf` | The symbols left: dingbats, geometric shapes, arrows, braille, chess, cards, technical symbols | 371 KB | Noto Sans Symbols 2 2.008 | OFL 1.1 (`OFL.txt`) |

The Medium weights match Roboto Medium. The fallbacks are drawn at Roboto's em size (an ideograph is
one em wide): ImGui sizes each source by its own line height, which differs per font (`CFontSet`
corrects for it).

Not covered: CJK Extension A (U+3400..4DBF) and the supplementary-plane ideographs (rare: 1.5 MB more
for Extension A alone), colour emoji, and anything that needs shaping - ImGui draws glyph by glyph, so
Arabic, the Indic scripts, right-to-left text and emoji sequences (flags, skin tones, ZWJ families)
show as their parts. A character no font has draws as U+FFFD.

## Cost

ImGui 1.92 rasterises a glyph the first time it is drawn, at the size it is drawn, into an atlas
that grows as needed: what the fonts hold costs memory, not atlas space. Measured on the editor's
smoke test with the three names of its non-ASCII entity check on screen (warm file cache, 3 runs):

| | Roboto only | With the fallbacks |
|---|---|---|
| Reading and adding the fonts at start-up | 0.2-0.9 ms | 3-5 ms |
| Font files held in memory | 0.16 MB | 7.7 MB |
| Font atlas at the end of the run | 512 x 128 | 512 x 128 |

Packaged games get the whole directory (`FRT.bat package` copies it to `Engine/Content/Fonts`). If a
fallback is missing the font goes without it; without Roboto, ImGui's built-in font (ASCII) is used.

## Regenerating the fallbacks

`Tools/Fonts/SubsetFonts.py` cuts them from the upstream releases, which it pins by commit and SHA-256:

| Input | From |
|---|---|
| `NotoSansCJKsc-Medium.otf` | [notofonts/noto-cjk](https://github.com/notofonts/noto-cjk) `Sans/OTF/SimplifiedChinese/`, commit `165c01b`, sha256 `ca094f6b...` |
| `NotoEmoji[wght].ttf` | [google/fonts](https://github.com/google/fonts) `ofl/notoemoji/`, commit `b979dba`, sha256 `de6c1883...` |
| `NotoSansSymbols2-Regular.ttf` | [google/fonts](https://github.com/google/fonts) `ofl/notosanssymbols2/`, commit `7b6724a`, sha256 `7d5fb73b...` |

It needs Python 3.10+ and fontTools (`pip install fonttools==4.60.1`, the version that made the
committed files). From the repository root:

```bat
rem Fetch the inputs into Intermediate/Fonts/ (untracked), then write the three subsets here.
python -I Tools/Fonts/SubsetFonts.py --download

rem Write nothing; exit 1 if the committed fonts differ from what it would write.
python -I Tools/Fonts/SubsetFonts.py --check
```

For each source it keeps the code points of its blocks (the tool lists them) that Roboto and the
fallbacks before it lack, drops the layout tables (GSUB, GPOS: ImGui does not shape) and the hinting
(stb_truetype ignores it), and fixes Noto Emoji's weight axis at 500 (stb_truetype reads no font
variations). The copyright and license records stay in each font's `name` table. The output is
byte-for-byte the same for the same inputs and fontTools version.

Kept here rather than referenced in place so the runtime does not depend on the layout of the Dear
ImGui sources.
