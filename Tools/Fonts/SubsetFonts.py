"""Subset the UI's fallback fonts (Core/Content/Fonts) from their upstream releases.

    python -I Tools/Fonts/SubsetFonts.py --download
    python -I Tools/Fonts/SubsetFonts.py
    python -I Tools/Fonts/SubsetFonts.py --check

Roboto, the UI font, covers Latin, Greek and Cyrillic. The fallbacks merged after it (UI/ImGuiFonts.cpp)
cover what it lacks: CJK (Noto Sans CJK), then emoji (Noto Emoji, monochrome), then the remaining
symbols (Noto Sans Symbols 2). Each keeps only the code points no font before it in that order
has, and no layout tables (ImGui does not shape text), so the three together are 7.5 MB where the
sources are 20 MB. ImGui rasterises a glyph the first time it is drawn: what a font holds
costs file size and memory, not atlas space.

Needs fontTools (pip install fonttools); the committed fonts were made with FONTTOOLS_VERSION.
Inputs are pinned by commit and SHA-256 and live in Intermediate/Fonts/ (untracked); only
--download touches the network. Output is deterministic for the same inputs and fontTools version.
See Core/Content/Fonts/README.md.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FONTS_DIR = REPO_ROOT / "Core" / "Content" / "Fonts"
DEFAULT_SOURCE_DIR = REPO_ROOT / "Intermediate" / "Fonts"
PRIMARY_FONT = "Roboto-Medium.ttf"
FONTTOOLS_VERSION = "4.60.1"


def blocks(*ranges: tuple[int, int]) -> set[int]:
    return {cp for first, last in ranges for cp in range(first, last + 1)}


# Symbol blocks, for the CJK font (whose national character sets carry many of them) and Symbols 2.
SYMBOLS = blocks(
    (0x2000, 0x206F),    # General Punctuation
    (0x2070, 0x209F),    # Superscripts and Subscripts
    (0x20A0, 0x20CF),    # Currency Symbols
    (0x2100, 0x214F),    # Letterlike Symbols
    (0x2150, 0x218F),    # Number Forms
    (0x2190, 0x21FF),    # Arrows
    (0x2200, 0x22FF),    # Mathematical Operators
    (0x2300, 0x23FF),    # Miscellaneous Technical
    (0x2400, 0x245F),    # Control Pictures, OCR
    (0x2460, 0x24FF),    # Enclosed Alphanumerics
    (0x2500, 0x25FF),    # Box Drawing, Block Elements, Geometric Shapes
    (0x2600, 0x27BF),    # Miscellaneous Symbols, Dingbats
    (0x27C0, 0x27FF),    # Miscellaneous Mathematical Symbols-A, Supplemental Arrows-A
    (0x2800, 0x28FF),    # Braille Patterns
    (0x2900, 0x2BFF),    # Supplemental Arrows-B ... Miscellaneous Symbols and Arrows
    (0x1F000, 0x1F0FF),  # Mahjong, Domino, Playing Cards
    (0x1F100, 0x1F1FF),  # Enclosed Alphanumeric Supplement
    (0x1F300, 0x1FAFF),  # Pictographs, Emoticons, Transport, ... Symbols and Pictographs Extended-A
)

CJK = blocks(
    (0x1100, 0x11FF),    # Hangul Jamo
    (0x2E80, 0x2FDF),    # CJK Radicals Supplement, Kangxi Radicals
    (0x2FF0, 0x2FFF),    # Ideographic Description Characters
    (0x3000, 0x303F),    # CJK Symbols and Punctuation
    (0x3040, 0x30FF),    # Hiragana, Katakana
    (0x3100, 0x312F),    # Bopomofo
    (0x3130, 0x318F),    # Hangul Compatibility Jamo
    (0x3190, 0x31FF),    # Kanbun, Bopomofo Extended, CJK Strokes, Katakana Phonetic Extensions
    (0x3200, 0x33FF),    # Enclosed CJK Letters and Months, CJK Compatibility
    (0x4E00, 0x9FFF),    # CJK Unified Ideographs: all of them, not a national set's subset
    (0xA960, 0xA97F),    # Hangul Jamo Extended-A
    (0xAC00, 0xD7AF),    # Hangul Syllables: all 11,172 - an IME composes any of them
    (0xD7B0, 0xD7FF),    # Hangul Jamo Extended-B
    (0xF900, 0xFAFF),    # CJK Compatibility Ideographs
    (0xFE10, 0xFE1F),    # Vertical Forms
    (0xFE30, 0xFE6F),    # CJK Compatibility Forms, Small Form Variants
    (0xFF00, 0xFFEF),    # Halfwidth and Fullwidth Forms
    (0x1F200, 0x1F2FF),  # Enclosed Ideographic Supplement
) | SYMBOLS

# In merge order (UI/ImGuiFonts.cpp loads them in this order, after the primary font).
# Extension A (U+3400..4DBF, 6,582 rare ideographs) and the supplementary-plane extensions are left
# out: 1.5 MB more for characters next to no name uses.
SOURCES = (
    {
        "output": "NotoSansCJKsc-Medium-Subset.otf",
        "file": "NotoSansCJKsc-Medium.otf",
        "url": "https://raw.githubusercontent.com/notofonts/noto-cjk/165c01b46ea533872e002e0785ff17e44f6d97d8/"
               "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Medium.otf",
        "sha256": "ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7",
        "code_points": CJK,
    },
    {
        "output": "NotoEmoji-Medium-Subset.ttf",
        "file": "NotoEmoji[wght].ttf",
        "url": "https://raw.githubusercontent.com/google/fonts/b979dba422e445492b0eb9951ac52ee0b4d648c3/"
               "ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf",
        "sha256": "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551",
        "code_points": None,     # everything it has
        "instance": {"wght": 500},   # Medium, as Roboto-Medium; stb_truetype reads no variations
    },
    {
        "output": "NotoSansSymbols2-Regular-Subset.ttf",
        "file": "NotoSansSymbols2-Regular.ttf",
        "url": "https://raw.githubusercontent.com/google/fonts/7b6724ac7ececc713e9ba93af309f7520c9a80a3/"
               "ofl/notosanssymbols2/NotoSansSymbols2-Regular.ttf",
        "sha256": "7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21",
        "code_points": SYMBOLS,
    },
)


class SubsetError(Exception):
    """A problem with the inputs or the environment: reported, and the run fails."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def download(source_dir: Path) -> None:
    """Fetch the inputs. Only when asked (--download)."""
    import urllib.request

    source_dir.mkdir(parents=True, exist_ok=True)
    for source in SOURCES:
        target = source_dir / source["file"]
        if target.is_file() and sha256(target.read_bytes()) == source["sha256"]:
            print(f"have {source['file']}")
            continue
        print(f"downloading {source['url']}")
        with urllib.request.urlopen(source["url"], timeout=120) as response:
            payload = response.read()
        if sha256(payload) != source["sha256"]:
            raise SubsetError(f"{source['url']}: SHA-256 {sha256(payload)}, expected {source['sha256']}")
        partial = target.with_name(target.name + ".partial")
        partial.write_bytes(payload)
        partial.replace(target)


def read_input(source_dir: Path, source: dict) -> bytes:
    path = source_dir / source["file"]
    if not path.is_file():
        raise SubsetError(f"{path} is missing (run with --download)")
    data = path.read_bytes()
    if sha256(data) != source["sha256"]:
        raise SubsetError(f"{path}: SHA-256 {sha256(data)}, expected {source['sha256']}")
    return data


def generate(source_dir: Path) -> dict[str, bytes]:
    try:
        import fontTools
        from fontTools import subset
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
    except ImportError as error:
        raise SubsetError(f"fontTools is required (pip install fonttools=={FONTTOOLS_VERSION}): {error}")
    if fontTools.version != FONTTOOLS_VERSION:
        print(f"note: fontTools {fontTools.version}; the committed fonts were made with {FONTTOOLS_VERSION}, "
              "so the bytes may differ (not the code points)")

    covered = set(TTFont(FONTS_DIR / PRIMARY_FONT).getBestCmap())
    outputs: dict[str, bytes] = {}
    for source in SOURCES:
        # The source's own timestamps: a save stamps 'head' with the current time otherwise.
        font = TTFont(io.BytesIO(read_input(source_dir, source)), recalcTimestamp=False)
        if "instance" in source:
            font = instancer.instantiateVariableFont(font, source["instance"])
        available = set(font.getBestCmap())
        wanted = available if source["code_points"] is None else available & source["code_points"]
        keep = wanted - covered

        options = subset.Options()
        options.layout_features = []      # no GSUB/GPOS: ImGui places glyph by glyph
        options.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "JSTF", "MATH", "VORG", "vhea", "vmtx", "DSIG"]
        options.hinting = False           # stb_truetype ignores hinting
        options.name_IDs = ["*"]          # keep the copyright and license records
        options.name_languages = ["*"]
        options.notdef_outline = True
        options.glyph_names = False
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=sorted(keep))
        subsetter.subset(font)

        buffer = io.BytesIO()
        font.save(buffer)
        outputs[source["output"]] = buffer.getvalue()
        covered |= keep
        print(f"{source['output']}: {len(keep)} code points, {len(buffer.getvalue()):,} bytes")
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR,
                        help="directory holding the source fonts (default: Intermediate/Fonts)")
    parser.add_argument("--download", action="store_true", help="fetch the pinned source fonts first")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit non-zero if the committed fonts differ from a fresh run")
    args = parser.parse_args()

    if sys.version_info < (3, 10):
        print("Python 3.10 or newer is required", file=sys.stderr)
        return 2
    source_dir = args.source_dir if args.source_dir.is_absolute() else Path.cwd() / args.source_dir

    try:
        if args.download:
            download(source_dir)
        outputs = generate(source_dir)
    except SubsetError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    status = 0
    for name, content in outputs.items():
        path = FONTS_DIR / name
        if args.check:
            if not path.is_file() or path.read_bytes() != content:
                print(f"differs: Core/Content/Fonts/{name}")
                status = 1
        else:
            path.write_bytes(content)
            print(f"wrote Core/Content/Fonts/{name}  sha256 {sha256(content)}")
    if args.check and status == 0:
        print("committed fonts are up to date")
    return status


if __name__ == "__main__":
    sys.exit(main())
