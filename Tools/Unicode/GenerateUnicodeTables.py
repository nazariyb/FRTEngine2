"""Generate frt::unicode's property and case-mapping tables from the Unicode Character Database.

    python -I Tools/Unicode/GenerateUnicodeTables.py --ucd-dir Intermediate/Unicode/17.0.0
    python -I Tools/Unicode/GenerateUnicodeTables.py --ucd-dir Intermediate/Unicode/17.0.0 --check
    python -I Tools/Unicode/GenerateUnicodeTables.py --download 17.0.0

Writes (LF line endings, byte-for-byte deterministic for the same inputs and generator):
    Core/Source/Unicode/UnicodeTables.generated.h       Core-private declarations
    Core/Source/Unicode/UnicodeTables.generated.inl     definitions, included only by Unicode.cpp
    Core-Test/Unicode/UnicodeTableChecksum.generated.inl

Standard library only, and deliberately never unicodedata: its UCD version is whatever the
interpreter shipped with (3.12: 15.0, 3.13: 15.1, 3.14: 16.0). Every property comes from the four
input files, so the tables depend on the files and on nothing else. See README.md next to this file.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import re
import sys
from array import array
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR_NAME = "Tools/Unicode/GenerateUnicodeTables.py"

TABLES_H = Path("Core/Source/Unicode/UnicodeTables.generated.h")
TABLES_INL = Path("Core/Source/Unicode/UnicodeTables.generated.inl")
CHECKSUM_INL = Path("Core-Test/Unicode/UnicodeTableChecksum.generated.inl")

INPUT_FILES = ("UnicodeData.txt", "CaseFolding.txt", "PropList.txt", "DerivedCoreProperties.txt")
# UnicodeData.txt has no header; these three start with "# <Name>-X.Y.Z.txt".
VERSIONED_INPUTS = ("CaseFolding.txt", "PropList.txt", "DerivedCoreProperties.txt")
DOWNLOAD_URL = "https://www.unicode.org/Public/{version}/ucd/{file}"

CODE_POINT_COUNT = 0x110000

# (UCD abbreviation, frt::unicode::EGeneralCategory enumerator) in the enum's order
# (Core/Source/Unicode/Unicode.h). The order is part of the table contract.
CATEGORIES = (
    ("Cn", "Unassigned"),
    ("Lu", "UppercaseLetter"), ("Ll", "LowercaseLetter"), ("Lt", "TitlecaseLetter"),
    ("Lm", "ModifierLetter"), ("Lo", "OtherLetter"),
    ("Mn", "NonspacingMark"), ("Mc", "SpacingMark"), ("Me", "EnclosingMark"),
    ("Nd", "DecimalNumber"), ("Nl", "LetterNumber"), ("No", "OtherNumber"),
    ("Pc", "ConnectorPunctuation"), ("Pd", "DashPunctuation"), ("Ps", "OpenPunctuation"),
    ("Pe", "ClosePunctuation"), ("Pi", "InitialPunctuation"), ("Pf", "FinalPunctuation"),
    ("Po", "OtherPunctuation"),
    ("Sm", "MathSymbol"), ("Sc", "CurrencySymbol"), ("Sk", "ModifierSymbol"), ("So", "OtherSymbol"),
    ("Zs", "SpaceSeparator"), ("Zl", "LineSeparator"), ("Zp", "ParagraphSeparator"),
    ("Cc", "Control"), ("Cf", "Format"), ("Cs", "Surrogate"), ("Co", "PrivateUse"),
)
CATEGORY_INDEX = {abbreviation: index for index, (abbreviation, _) in enumerate(CATEGORIES)}

# frt::unicode::detail::ECharFlags, and the binary property each bit is read from.
FLAG_PROPERTIES = (
    ("PropList.txt", "White_Space", 1 << 0),
    ("DerivedCoreProperties.txt", "Alphabetic", 1 << 1),
    ("DerivedCoreProperties.txt", "Uppercase", 1 << 2),
    ("DerivedCoreProperties.txt", "Lowercase", 1 << 3),
)

SHIFT1_CHOICES = (8, 9, 10, 11)
SHIFT2_CHOICES = (4, 5, 6)
RECORD_BYTES = 16                 # sizeof(frt::unicode::detail::SCharRecord)
SIZE_BUDGET = 48 * 1024

FNV_OFFSET_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
UINT64_MASK = (1 << 64) - 1

# The line of the generated headers recording the interpreter. It is excluded from --check:
# another Python version produces the same tables, and that must not count as a difference.
PYTHON_LINE_PREFIX = "// Python: "


class GeneratorError(Exception):
    """A problem with the inputs or the generated data: reported, and the run fails."""


# ---- Inputs ----

def read_inputs(ucd_dir: Path) -> dict[str, bytes]:
    inputs = {}
    for name in INPUT_FILES:
        path = ucd_dir / name
        if not path.is_file():
            raise GeneratorError(f"missing input {path}")
        inputs[name] = path.read_bytes()
    return inputs


def read_version(inputs: dict[str, bytes], expected: str | None) -> str:
    """The UCD version all three versioned headers agree on (and --download asked for)."""
    versions = {}
    for name in VERSIONED_INPUTS:
        first_line = inputs[name].split(b"\n", 1)[0].decode("utf-8").strip()
        match = re.fullmatch(r"#\s*(\w+)-(\d+\.\d+\.\d+)\.txt", first_line)
        if match is None or f"{match.group(1)}.txt" != name:
            raise GeneratorError(f"{name}: unexpected first line {first_line!r}")
        versions[name] = match.group(2)

    distinct = sorted(set(versions.values()))
    if len(distinct) != 1:
        raise GeneratorError(f"the inputs are from different UCD versions: {versions}")
    if expected is not None and distinct[0] != expected:
        raise GeneratorError(f"the inputs are UCD {distinct[0]}, --download asked for {expected}")
    return distinct[0]


def read_copyright_year(inputs: dict[str, bytes]) -> str:
    """The year of the Unicode copyright line in the versioned headers, for the license notice."""
    years = set()
    for name in VERSIONED_INPUTS:
        for line in inputs[name].decode("utf-8").splitlines()[:8]:
            match = re.match(r"#\s*(?:©|\(c\)|Copyright)\s*(?:\d{4}-)?(\d{4})", line, re.IGNORECASE)
            if match:
                years.add(match.group(1))
    if not years:
        raise GeneratorError("no copyright line in the input headers")
    return max(years)


def data_lines(text: bytes):
    """(line number, fields) of every data line: comments stripped, fields split on ';' and trimmed."""
    for number, line in enumerate(text.decode("utf-8").splitlines(), start=1):
        line = line.split("#", 1)[0].strip()
        if line:
            yield number, [field.strip() for field in line.split(";")]


def parse_range(field: str) -> range:
    first, _, last = field.partition("..")
    return range(int(first, 16), int(last or first, 16) + 1)


def parse_unicode_data(text: bytes, categories: bytearray, upper: dict, lower: dict) -> None:
    """General category and simple upper/lower mappings (fields 2, 12, 13); expands First/Last ranges."""
    range_start = None
    for number, fields in data_lines(text):
        if len(fields) != 15:
            raise GeneratorError(f"UnicodeData.txt:{number}: expected 15 fields, got {len(fields)}")
        code_point = int(fields[0], 16)
        name, category = fields[1], fields[2]
        if category not in CATEGORY_INDEX or category == "Cn":
            raise GeneratorError(f"UnicodeData.txt:{number}: unexpected general category {category!r}")

        if name.endswith(", First>"):
            range_start = (code_point, category)
            continue
        if name.endswith(", Last>"):
            if range_start is None or range_start[1] != category:
                raise GeneratorError(f"UnicodeData.txt:{number}: range end without a matching start")
            if fields[12] or fields[13]:
                raise GeneratorError(f"UnicodeData.txt:{number}: case mapping on a range")
            for value in range(range_start[0], code_point + 1):
                categories[value] = CATEGORY_INDEX[category]
            range_start = None
            continue
        if range_start is not None:
            raise GeneratorError(f"UnicodeData.txt:{number}: range start without an end")

        categories[code_point] = CATEGORY_INDEX[category]
        if fields[12]:
            upper[code_point] = int(fields[12], 16)
        if fields[13]:
            lower[code_point] = int(fields[13], 16)


def parse_case_folding(text: bytes, fold: dict) -> None:
    """Simple case folding: status C (common) and S (simple). F (full) and T (Turkic) are not ours."""
    for number, fields in data_lines(text):
        if len(fields) < 3:
            raise GeneratorError(f"CaseFolding.txt:{number}: expected code; status; mapping")
        status = fields[1]
        if status not in ("C", "S"):
            continue
        code_point = int(fields[0], 16)
        mapping = fields[2].split()
        if len(mapping) != 1:
            raise GeneratorError(f"CaseFolding.txt:{number}: status {status} with a multi-code-point mapping")
        if code_point in fold:
            raise GeneratorError(f"CaseFolding.txt:{number}: U+{code_point:04X} folded twice (C and S)")
        fold[code_point] = int(mapping[0], 16)


def parse_binary_property(name: str, text: bytes, prop: str, flags: bytearray, bit: int) -> None:
    found = False
    for _, fields in data_lines(text):
        # Two fields: a binary property. Longer lines are enumerated properties (InCB; Linker).
        if len(fields) == 2 and fields[1] == prop:
            for code_point in parse_range(fields[0]):
                flags[code_point] |= bit
            found = True
    if not found:
        raise GeneratorError(f"{name}: no {prop} entries")


class UnicodeData:
    """Per-code-point properties, step 1 of the algorithm (README.md)."""

    def __init__(self, inputs: dict[str, bytes]):
        self.categories = bytearray(CODE_POINT_COUNT)     # 0 = Cn, also for unlisted code points
        self.flags = bytearray(CODE_POINT_COUNT)
        self.upper: dict[int, int] = {}
        self.lower: dict[int, int] = {}
        self.fold: dict[int, int] = {}

        parse_unicode_data(inputs["UnicodeData.txt"], self.categories, self.upper, self.lower)
        parse_case_folding(inputs["CaseFolding.txt"], self.fold)
        for file_name, prop, bit in FLAG_PROPERTIES:
            parse_binary_property(file_name, inputs[file_name], prop, self.flags, bit)

    def record(self, code_point: int) -> tuple[int, int, int, int, int]:
        """(category, flags, upper delta, lower delta, fold delta): the SCharRecord fields."""
        return (self.categories[code_point], self.flags[code_point],
                self.upper.get(code_point, code_point) - code_point,
                self.lower.get(code_point, code_point) - code_point,
                self.fold.get(code_point, code_point) - code_point)

    def mapped(self, code_point: int) -> tuple[int, int, int]:
        return (self.upper.get(code_point, code_point), self.lower.get(code_point, code_point),
                self.fold.get(code_point, code_point))


# ---- Tables ----

def element_size(max_value: int) -> int:
    """Bytes of the smallest of uint8 / uint16 holding max_value."""
    if max_value <= 0xFF:
        return 1
    if max_value <= 0xFFFF:
        return 2
    raise GeneratorError(f"{max_value} ids do not fit uint16")


class Trie:
    """Three-level trie: Index1[cp >> Shift1] -> Index2 block, Index2 -> Leaf block, Leaf -> record id."""

    def __init__(self, record_ids: array, shift1: int, shift2: int):
        self.shift1 = shift1
        self.shift2 = shift2

        leaf_size = 1 << shift2
        leaf_blocks: dict[bytes, int] = {}
        self.leaf = array("H")
        leaf_block_ids = []
        for start in range(0, CODE_POINT_COUNT, leaf_size):
            block = record_ids[start:start + leaf_size]
            key = block.tobytes()
            if key not in leaf_blocks:
                leaf_blocks[key] = len(leaf_blocks)
                self.leaf.extend(block)
            leaf_block_ids.append(leaf_blocks[key])

        index2_size = 1 << (shift1 - shift2)
        index2_blocks: dict[tuple, int] = {}
        self.index2 = []
        self.index1 = []
        for start in range(0, len(leaf_block_ids), index2_size):
            block = tuple(leaf_block_ids[start:start + index2_size])
            if block not in index2_blocks:
                index2_blocks[block] = len(index2_blocks)
                self.index2.extend(block)
            self.index1.append(index2_blocks[block])

        self.index1_size = element_size(max(self.index1))
        self.index2_size = element_size(max(self.index2))
        self.leaf_size = element_size(max(self.leaf))

    def total_bytes(self, record_count: int) -> int:
        return (len(self.index1) * self.index1_size + len(self.index2) * self.index2_size
                + len(self.leaf) * self.leaf_size + record_count * RECORD_BYTES)

    def lookup(self, code_point: int) -> int:
        """The record id, computed exactly the way UnicodeTablesInternal.h's LookupRecord does."""
        level_bits = self.shift1 - self.shift2
        block1 = self.index1[code_point >> self.shift1]
        block2 = self.index2[(block1 << level_bits) | ((code_point >> self.shift2) & ((1 << level_bits) - 1))]
        return self.leaf[(block2 << self.shift2) | (code_point & ((1 << self.shift2) - 1))]


class Tables:
    """Steps 2-4: unique records, the smallest trie over the shift choices, the self-check."""

    def __init__(self, data: UnicodeData):
        record_index: dict[tuple, int] = {}
        self.records: list[tuple] = []
        self.first_user: list[int] = []      # first code point of each record, for the comments
        record_ids = array("H")
        for code_point in range(CODE_POINT_COUNT):
            record = data.record(code_point)
            index = record_index.get(record)
            if index is None:
                index = record_index[record] = len(self.records)
                self.records.append(record)
                self.first_user.append(code_point)
            record_ids.append(index)

        # Above U+10FFFF the lookup answers with this record: Cn, no properties, no mappings.
        self.unassigned_record = record_index[(0, 0, 0, 0, 0)]

        best = None
        for shift1 in SHIFT1_CHOICES:
            for shift2 in SHIFT2_CHOICES:
                trie = Trie(record_ids, shift1, shift2)
                if best is None or trie.total_bytes(len(self.records)) < best.total_bytes(len(self.records)):
                    best = trie
        self.trie = best
        self.total_bytes = best.total_bytes(len(self.records))
        if self.total_bytes > SIZE_BUDGET:
            raise GeneratorError(f"tables take {self.total_bytes} bytes, over the {SIZE_BUDGET}-byte budget")

        # Self-check: every code point's record rebuilt from the arrays as emitted.
        for code_point in range(CODE_POINT_COUNT):
            if self.records[self.trie.lookup(code_point)] != data.record(code_point):
                raise GeneratorError(f"self-check failed at U+{code_point:04X}")


def table_checksum(data: UnicodeData) -> tuple[int, list[int]]:
    """FNV-1a-64 over (uint32 cp, uint8 category, uint8 flags, uint32 upper, lower, fold) little endian
    for every code point in order, and the number of code points per category. Core-Test recomputes
    both through the public API."""
    checksum = FNV_OFFSET_BASIS
    counts = [0] * len(CATEGORIES)
    for code_point in range(CODE_POINT_COUNT):
        category = data.categories[code_point]
        counts[category] += 1
        upper, lower, fold = data.mapped(code_point)
        chunk = (code_point.to_bytes(4, "little") + bytes((category, data.flags[code_point]))
                 + upper.to_bytes(4, "little") + lower.to_bytes(4, "little") + fold.to_bytes(4, "little"))
        for byte in chunk:
            checksum = ((checksum ^ byte) * FNV_PRIME) & UINT64_MASK
    return checksum, counts


# ---- Output ----

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def generator_sha256() -> str:
    # LF-normalized: a checkout with core.autocrlf has CRLF in the working tree, and that must not
    # change the recorded hash of the same committed generator.
    return sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n"))


def license_notice(year: str) -> list[str]:
    return [
        "Unicode Character Database: Copyright (c) 1991-" + year + " Unicode, Inc.",
        "For terms of use and license, see https://www.unicode.org/terms_of_use.html",
        "",
        "UNICODE LICENSE V3",
        "",
        "COPYRIGHT AND PERMISSION NOTICE",
        "",
        "Copyright (c) 1991-" + year + " Unicode, Inc.",
        "",
        "NOTICE TO USER: Carefully read the following legal agreement. BY",
        "DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR",
        "SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE",
        "TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT",
        "DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.",
        "",
        "Permission is hereby granted, free of charge, to any person obtaining a",
        "copy of data files and any associated documentation (the \"Data Files\") or",
        "software and any associated documentation (the \"Software\") to deal in the",
        "Data Files or Software without restriction, including without limitation",
        "the rights to use, copy, modify, merge, publish, distribute, and/or sell",
        "copies of the Data Files or Software, and to permit persons to whom the",
        "Data Files or Software are furnished to do so, provided that either (a)",
        "this copyright and permission notice appear with all copies of the Data",
        "Files or Software, or (b) this copyright and permission notice appear in",
        "associated Documentation.",
        "",
        "THE DATA FILES AND SOFTWARE ARE PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY",
        "KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF",
        "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF",
        "THIRD PARTY RIGHTS.",
        "",
        "IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE",
        "BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,",
        "OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,",
        "WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,",
        "ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA",
        "FILES OR SOFTWARE.",
        "",
        "Except as contained in this notice, the name of a copyright holder shall",
        "not be used in advertising or otherwise to promote the sale, use or other",
        "dealings in these Data Files or Software without prior written",
        "authorization of the copyright holder.",
    ]


def provenance_header(version: str, inputs: dict[str, bytes], tables: Tables, year: str) -> list[str]:
    lines = [
        f"// Generated by {GENERATOR_NAME} -- DO NOT EDIT. Regenerate instead (Tools/Unicode/README.md).",
        "//",
        f"// Unicode Character Database {version}:",
    ]
    width = max(len(name) for name in INPUT_FILES)
    for name in INPUT_FILES:
        lines.append(f"//   {name.ljust(width)}  sha256 {sha256(inputs[name])}")
    lines += [
        f"// Generator sha256 (LF line endings): {generator_sha256()}",
        PYTHON_LINE_PREFIX + " ".join(sys.version.split()),
        f"// Trie: Shift1 = {tables.trie.shift1}, Shift2 = {tables.trie.shift2}; "
        f"{len(tables.records)} records; {tables.total_bytes} bytes in total.",
        "//",
    ]
    lines += [("// " + line).rstrip() for line in license_notice(year)]
    return lines


CPP_TYPES = {1: "uint8", 2: "uint16"}


def format_array(values, per_line: int = 32) -> list[str]:
    lines = []
    for start in range(0, len(values), per_line):
        lines.append("\t" + ", ".join(str(value) for value in values[start:start + per_line]) + ",")
    return lines


def emit_tables_h(header: list[str], version: str, tables: Tables) -> str:
    trie = tables.trie
    lines = header + [
        "",
        "#pragma once",
        "",
        "// Core-private: reached through Unicode/UnicodeTablesInternal.h (other modules call",
        "// unicode::detail::GetCharRecord). The arrays are defined in UnicodeTables.generated.inl.",
        "",
        "#include \"CoreTypes.h\"",
        "#include \"Unicode/Unicode.h\"",
        "",
        "",
        "namespace frt::unicode::tables",
        "{",
        f"inline constexpr char Version[] = \"{version}\";",
        "",
        "// Index1[cp >> Shift1] -> Index2 block; Index2[(block << (Shift1 - Shift2)) | ((cp >> Shift2) & mask)]",
        "// -> Leaf block; Leaf[(block << Shift2) | (cp & mask)] -> Records index.",
        f"inline constexpr uint32 Shift1 = {trie.shift1}u;",
        f"inline constexpr uint32 Shift2 = {trie.shift2}u;",
        f"inline constexpr uint32 RecordCount = {len(tables.records)}u;",
        f"inline constexpr uint32 TotalBytes = {tables.total_bytes}u;   // all four arrays",
        "// Records index of (Cn, no flags, no mappings): the answer for code points above U+10FFFF.",
        f"inline constexpr uint32 UnassignedRecord = {tables.unassigned_record}u;",
        "",
        f"using TIndex1 = {CPP_TYPES[trie.index1_size]};",
        f"using TIndex2 = {CPP_TYPES[trie.index2_size]};",
        f"using TLeaf = {CPP_TYPES[trie.leaf_size]};",
        "",
        f"extern const TIndex1 Index1[{len(trie.index1)}];",
        f"extern const TIndex2 Index2[{len(trie.index2)}];",
        f"extern const TLeaf Leaf[{len(trie.leaf)}];",
        "extern const detail::SCharRecord Records[RecordCount];",
        "}",
    ]
    return "\n".join(lines) + "\n"


def flag_names(flags: int) -> str:
    names = [prop for _, prop, bit in FLAG_PROPERTIES if flags & bit]
    return " ".join(names)


def emit_tables_inl(header: list[str], tables: Tables) -> str:
    trie = tables.trie
    lines = header + [
        "",
        "// Included only by Unicode.cpp: one definition of each array in Core.",
        "",
        "#include \"Unicode/UnicodeTables.generated.h\"",
        "",
        "",
        "namespace frt::unicode::tables",
        "{",
        f"const TIndex1 Index1[{len(trie.index1)}] =",
        "{",
        *format_array(trie.index1),
        "};",
        "",
        f"const TIndex2 Index2[{len(trie.index2)}] =",
        "{",
        *format_array(trie.index2),
        "};",
        "",
        f"const TLeaf Leaf[{len(trie.leaf)}] =",
        "{",
        *format_array(trie.leaf),
        "};",
        "",
        "// { UpperDelta, LowerDelta, FoldDelta, Category, Flags, Padding }   // id: first code point using it",
        "const detail::SCharRecord Records[RecordCount] =",
        "{",
    ]
    for index, (category, flags, upper, lower, fold) in enumerate(tables.records):
        flag_text = flag_names(flags)
        comment = f"// {index}: U+{tables.first_user[index]:04X} {CATEGORIES[category][0]}"
        if flag_text:
            comment += " " + flag_text
        lines.append(f"\t{{ {upper}, {lower}, {fold}, EGeneralCategory::{CATEGORIES[category][1]}, "
                     f"0x{flags:02X}, {{ 0, 0 }} }},   {comment}")
    lines += ["};", "}"]
    return "\n".join(lines) + "\n"


def emit_checksum_inl(header: list[str], version: str, checksum: int, counts: list[int]) -> str:
    lines = header + [
        "",
        "#pragma once",
        "",
        "// What Unicode/UnicodeTest.cpp recomputes through the public frt::unicode API. It proves the C++",
        "// lookup reproduces the generated data; UnicodeOracleTest.cpp checks the UCD parsing itself.",
        "",
        "#include \"CoreTypes.h\"",
        "",
        "",
        "namespace frt::unicode::checksum",
        "{",
        f"inline constexpr char Version[] = \"{version}\";",
        "",
        "// FNV-1a-64 over every code point 0..10FFFF in order, 18 bytes each, little endian:",
        "// uint32 code point, uint8 category, uint8 flags (ECharFlags), uint32 upper, uint32 lower,",
        "// uint32 fold (mapped code points, not deltas).",
        f"inline constexpr uint64 Fnv1a64 = 0x{checksum:016X}ull;",
        "",
        "// Code points per EGeneralCategory, in the enum's order.",
        f"inline constexpr uint32 CategoryCounts[{len(CATEGORIES)}] =",
        "{",
    ]
    for (abbreviation, name), count in zip(CATEGORIES, counts):
        lines.append(f"\t{count}u,   // {abbreviation} {name}")
    lines += ["};", "}"]
    return "\n".join(lines) + "\n"


def generate(ucd_dir: Path, expected_version: str | None) -> tuple[dict[Path, bytes], dict]:
    inputs = read_inputs(ucd_dir)
    version = read_version(inputs, expected_version)
    year = read_copyright_year(inputs)

    data = UnicodeData(inputs)
    tables = Tables(data)
    checksum, counts = table_checksum(data)

    if "unicodedata" in sys.modules:
        raise GeneratorError("unicodedata was imported: the tables must come from the input files only")

    header = provenance_header(version, inputs, tables, year)
    outputs = {
        TABLES_H: emit_tables_h(header, version, tables).encode("ascii"),
        TABLES_INL: emit_tables_inl(header, tables).encode("ascii"),
        CHECKSUM_INL: emit_checksum_inl(header, version, checksum, counts).encode("ascii"),
    }
    stats = {
        "version": version,
        "shift1": tables.trie.shift1,
        "shift2": tables.trie.shift2,
        "records": len(tables.records),
        "total_bytes": tables.total_bytes,
        "index1": f"{len(tables.trie.index1)} x {CPP_TYPES[tables.trie.index1_size]}",
        "index2": f"{len(tables.trie.index2)} x {CPP_TYPES[tables.trie.index2_size]}",
        "leaf": f"{len(tables.trie.leaf)} x {CPP_TYPES[tables.trie.leaf_size]}",
        "checksum": f"0x{checksum:016X}",
    }
    return outputs, stats


# ---- Commands ----

def download(version: str, ucd_dir: Path) -> None:
    """Fetch the four inputs. Only when asked (--download): the generator never touches the network
    otherwise, and builds never run it at all."""
    import urllib.request

    already_imported = "unicodedata" in sys.modules
    ucd_dir.mkdir(parents=True, exist_ok=True)
    for name in INPUT_FILES:
        url = DOWNLOAD_URL.format(version=version, file=name)
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=60) as response:
            payload = response.read()
        partial = ucd_dir / (name + ".partial")
        partial.write_bytes(payload)
        partial.replace(ucd_dir / name)

    # urlopen resolves the host name through the 'idna' codec, which imports unicodedata for its own
    # Unicode 3.2 tables: nothing the generator reads. Forget that import again, so that generate()'s
    # check still catches any use of unicodedata by the generator itself.
    if not already_imported:
        sys.modules.pop("unicodedata", None)


def normalized_for_check(text: bytes) -> list[str]:
    # CRLF: a core.autocrlf checkout. The Python line: see PYTHON_LINE_PREFIX.
    lines = text.replace(b"\r\n", b"\n").decode("ascii", errors="replace").split("\n")
    return [line for line in lines if not line.startswith(PYTHON_LINE_PREFIX)]


def committed_input_hashes() -> dict[str, str]:
    path = REPO_ROOT / TABLES_H
    if not path.is_file():
        raise GeneratorError(f"{TABLES_H} does not exist: nothing to check against")
    hashes = {}
    for line in path.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"//\s+(\S+\.txt)\s+sha256 ([0-9a-f]{64})", line)
        if match:
            hashes[match.group(1)] = match.group(2)
    return hashes


def check(ucd_dir: Path, expected_version: str | None) -> int:
    committed = committed_input_hashes()
    inputs = read_inputs(ucd_dir)
    differing = [name for name in INPUT_FILES if committed.get(name) != sha256(inputs[name])]
    if differing:
        print(f"inputs differ from the ones {TABLES_H} was generated from: {', '.join(differing)}")
        return 2

    outputs, _ = generate(ucd_dir, expected_version)
    status = 0
    for relative, generated in outputs.items():
        path = REPO_ROOT / relative
        if not path.is_file():
            print(f"{relative}: missing")
            status = 1
            continue
        expected_lines = normalized_for_check(path.read_bytes())
        generated_lines = normalized_for_check(generated)
        if expected_lines != generated_lines:
            status = 1
            print(f"{relative}: differs from what the generator produces")
            diff = difflib.unified_diff(expected_lines, generated_lines, "committed", "generated", lineterm="", n=1)
            for count, line in enumerate(diff):
                if count == 40:
                    print("   ...")
                    break
                print("   " + line[:160])
    if status == 0:
        print("generated files are up to date")
    return status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ucd-dir", type=Path,
                        help="directory holding the four UCD files (default with --download: "
                             "Intermediate/Unicode/<version>)")
    parser.add_argument("--download", metavar="VERSION",
                        help="fetch the four files of this UCD version from unicode.org first")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit non-zero if the committed files differ from a fresh run")
    args = parser.parse_args()

    if sys.version_info < (3, 10):
        print("Python 3.10 or newer is required", file=sys.stderr)
        return 2

    ucd_dir = args.ucd_dir
    if ucd_dir is None:
        if args.download is None:
            parser.error("--ucd-dir is required (or --download VERSION)")
        ucd_dir = REPO_ROOT / "Intermediate" / "Unicode" / args.download
    elif not ucd_dir.is_absolute():
        ucd_dir = Path.cwd() / ucd_dir

    try:
        if args.download is not None:
            download(args.download, ucd_dir)
        if args.check:
            return check(ucd_dir, args.download)

        outputs, stats = generate(ucd_dir, args.download)
        for relative, content in outputs.items():
            path = REPO_ROOT / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)        # bytes: LF on every platform
            print(f"wrote {relative}")
        print("self-check passed: every code point's record rebuilt from the emitted arrays")
        for key, value in stats.items():
            print(f"  {key}: {value}")
        return 0
    except GeneratorError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
