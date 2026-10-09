# FRTEngine2 — Unicode tables

`GenerateUnicodeTables.py` turns four files of the Unicode Character Database (UCD) into the
character property and case-mapping tables behind `frt::unicode` (`Core/Source/Unicode/Unicode.h`):
general category, White_Space / Alphabetic / Uppercase / Lowercase, and the simple uppercase,
lowercase and case-folding mappings of every code point.

The generated files are committed. Builds need neither Python nor network; only bumping the Unicode
version (or changing the generator) runs it.

| File | Role |
|---|---|
| `Core/Source/Unicode/UnicodeTables.generated.h` | Core-private declarations: version, trie shifts, element types, `extern const` arrays |
| `Core/Source/Unicode/UnicodeTables.generated.inl` | The array definitions; included only by `Unicode.cpp` |
| `Core-Test/Unicode/UnicodeTableChecksum.generated.inl` | FNV-1a-64 of every code point's properties plus per-category counts, which `UnicodeTest.cpp` recomputes through the public API |

Never edit them by hand: `--check` (below) and the checksum test both notice.

## Inputs

One UCD version, all four files from `https://www.unicode.org/Public/<version>/ucd/`:

| File | Used for |
|---|---|
| `UnicodeData.txt` | General category (field 2, `<..., First>`/`<..., Last>` ranges expanded), simple uppercase / lowercase mappings (fields 12, 13) |
| `CaseFolding.txt` | Simple case folding: status `C` and `S` only (`F` and `T` are full and Turkic folding, not ours) |
| `PropList.txt` | `White_Space` |
| `DerivedCoreProperties.txt` | `Alphabetic`, `Uppercase`, `Lowercase` |

They live in `Intermediate/Unicode/<version>/` (untracked). The generator reads the version from the
first line of the last three (`# CaseFolding-17.0.0.txt`) and refuses to run unless they agree.
`UnicodeData.txt` has no version header; keep it with its siblings.

The generator never imports Python's `unicodedata`: its UCD version is the interpreter's (3.12 ships
15.0, 3.13 15.1, 3.14 16.0), so the tables would silently depend on which Python ran. Everything
comes from the input files. Python 3.10+ standard library only.

## Use

From the repository root:

```bat
rem Regenerate the three files from a UCD directory.
python -I Tools/Unicode/GenerateUnicodeTables.py --ucd-dir Intermediate/Unicode/17.0.0

rem Write nothing; exit non-zero if the committed files are not what the generator produces.
python -I Tools/Unicode/GenerateUnicodeTables.py --ucd-dir Intermediate/Unicode/17.0.0 --check

rem Fetch the four files of a version into Intermediate/Unicode/<version>/ first, then generate.
python -I Tools/Unicode/GenerateUnicodeTables.py --download 17.0.0
```

`--check` first compares the SHA-256 of the input files with the ones recorded in the committed
header ("inputs differ", exit 2), then generates in memory and diffs (exit 1 on a difference). It
ignores CRLF (a `core.autocrlf` checkout) and the line recording the Python version (another
interpreter produces the same tables). The output is byte-for-byte deterministic, with LF line
endings on every platform.

## How the tables work

1. Every code point 0..10FFFF gets a record `(category, flags, upper delta, lower delta, fold delta)`;
   unlisted code points are `(Cn, 0, 0, 0, 0)`. Mappings are stored as deltas, so a whole case-paired
   alphabet shares one record.
2. Identical records are merged (first-occurrence order): about 230 for Unicode 17.
3. A three-level trie maps a code point to its record id:
   `Index1[cp >> Shift1]` -> an `Index2` block -> a `Leaf` block -> `Records[id]`. Blocks are
   deduplicated at both levels, element types are the smallest of `uint8`/`uint16` that fit, and the
   generator tries `Shift1` in {8..11} x `Shift2` in {4..6} and keeps the smallest. Over 48 KB aborts
   (Unicode 17: about 24 KB). Code points above U+10FFFF get the `UnassignedRecord`.
4. Self-check: every code point's record is rebuilt from the arrays as emitted (the same arithmetic as
   `LookupRecord` in `Core/Source/Unicode/UnicodeTablesInternal.h`) and compared with step 1.

The lookup is O(1): three dependent loads. Core's own per-code-point loops include
`UnicodeTablesInternal.h` and get it inlined; every other module calls the exported
`unicode::detail::GetCharRecord`. ASCII never reaches the tables at all.

## Bumping the Unicode version

1. Get the four files of the new version into `Intermediate/Unicode/<version>/` (`--download
   <version>`, or copy them there).
2. Regenerate: `python -I Tools/Unicode/GenerateUnicodeTables.py --ucd-dir Intermediate/Unicode/<version>`.
3. `--check` the result, review the diff of the generated files (new records, changed counts), and
   commit all three together with the generator if it changed.
4. Build and run Core-Test (`--gtest_filter=Unicode*:Ascii*`), in particular:
   - `UnicodeTablesTest.PublicApiReproducesGeneratedChecksum`: the C++ lookup reproduces the data;
   - `UnicodeOracleTest`: the parsing agrees with Windows' own ICU (`icu.dll`) on every code point both
     have assigned: case mappings and folding, White_Space, the general category and the Alphabetic,
     Uppercase and Lowercase flags. Case pairs and case folding are stable across versions, so a
     failure there is a parsing bug, not a version difference. The other properties may change on
     purpose: the test lists the few code points Unicode changed since `icu.dll`'s version
     (`KnownChanges`), and a new version that changes more gets them added there, each checked against
     UAX #44's change history first. (It skips where `icu.dll` is missing.)
5. Nothing in the tests hard-codes the version (it comes from the generated files); adjust only a spot
   check in `UnicodeTest.cpp` that the new version deliberately changes (general categories do move
   occasionally, e.g. U+0295 went from Ll to Lo in Unicode 17), and `KnownChanges` as above.

## License

The UCD is © Unicode, Inc., under the Unicode License v3
(<https://www.unicode.org/license.txt>). Each generated file carries the notice, as the license asks
of data derived from it.
