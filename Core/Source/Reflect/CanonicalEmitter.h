#pragma once

#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Reflect/DataTree.h"


namespace frt
{
struct SDiagnostic;
}


/**
 * CDataTree -> canonical YAML bytes (design §3.7, §15 I17): how every file FRT writes itself looks (`.frt` documents,
 * `.meta` files, the lock and migration files). Determinism and small diffs are format properties: two people who save
 * the same data get the same bytes, so a three-way merge works and a re-save without a change touches nothing.
 *
 * Why FRT writes the bytes itself rather than ordering what yaml-cpp emits (I17 first said "yaml-cpp emits", revised at
 * M4): yaml-cpp's emitter decides quoting by the value, formats floats its own way, indents sequences differently and
 * rewrites some text (YamlString.h), and none of that is ours to fix. yaml-cpp stays the parser: every result is read back
 * through ParseYaml and refused if it would not say the same thing, which is what makes "the emitter writes the bytes"
 * safe. This file includes no yaml-cpp and needs no IsYamlStorable.
 *
 * What the bytes are, for any tree:
 * - **Keys of a map:** `$type`, `$version`, `$rowType`, `$rowVersion`, `$note`; then the rule's KeyOrder (a type's declared
 *   order, §3.2); then the rest in the order the tree has them, or all of the rest by key bytes with bSortKeys (table rows
 *   and keyed arrays: parallel additions merge). A reference (a map holding `$asset`) instead leads with `$asset`, `sub`,
 *   `row`, `id`, `path`.
 * - **Style:** block, two spaces per level, sequence items indented under their key, one key per line. Flow, `{ a: 1 }`
 *   and `[ a, b ]`, for references and what a rule's bFlow names (reflection's `meta::Flow` arrives with M5), and always
 *   for what is below a flow node and for an empty map or sequence (`{}`, `[]`). The style the tree came in is ignored.
 * - **Scalars:** a plain integer in decimal, a plain float in std::to_chars' shortest form that reads back the same value
 *   (so `1.50` is `1.5` and `1.0` is `1`), `.inf`, `-.inf`, `.nan`, a null as `null`, booleans `true`/`false`. The text of
 *   `$type`, `$rowType`, `$note` and of every scalar of a reference is never read as a number: an id stays its digits.
 * - **Strings** (an enum's reflected name is one by the time it reaches a tree) are plain unless YAML (1.2 or 1.1) would
 *   type them (`yes`, `null`, `0x10`, `1e5`), they start with an indicator or a space, or end with a space, or hold `:`
 *   or `#` (so `"mesh:Body"`, always), a control character, a line break, or in flow a `,` `?` `[` `]` `{` `}`. Quoted
 *   means double-quoted on one line: no block scalars, no folding. Text that was quoted and needs none loses its quotes;
 *   a plain `yes`, which the strict typed reads take as text anyway, gains them. Keys follow the same rule.
 * - LF, a final newline, no BOM, no comments (authors who annotate use `$note`). An empty tree is no bytes at all.
 *
 * Refusal: invalid UTF-8 in a key or a scalar, a Raw node (nothing says its text fits where it would land), or any text
 * that would not read back through ParseYaml as the same data, for instance a key past yaml-cpp's limit of 1024 bytes. A
 * refused tree writes nothing, and the reason names the node's position in its source tree.
 *
 * Plain data on the caller's stack, std containers and FString only: safe on a worker.
 */
namespace frt::reflect
{
/**
 * What the shared rules cannot know about one map or sequence of a document. Path is its place from the root, map keys
 * separated by '.', with "*" for any key or any sequence item: "" is the root, "FireModes" the value of that key,
 * "FireModes.*" each of its values. A key that itself holds a '.' is reached with "*". The first rule that matches wins.
 */
struct SEmitRule
{
	FStringView Path;
	std::span<const FStringLiteral> KeyOrder;   // a map: these keys come first, in this order
	bool bFlow = false;                         // written in flow style, and everything below it
	bool bSortKeys = false;                     // a map: the keys KeyOrder leaves are sorted by bytes (ordinal)
};

struct SEmitPolicy
{
	std::span<const SEmitRule> Rules;
	// Read the output back and refuse it unless it says the same. Off only for a caller about to compare the output with
	// bytes that have just been parsed into this tree (is this file canonical?), where equal bytes already prove it.
	bool bVerifyRoundTrip = true;
};

/**
 * Tree's canonical bytes into Out (replacing it). False with Out empty when the tree is refused; OutError then has
 * Severity Error, the Line and Column of the node in Tree's source (0 when it has none) and the Message. The caller fills
 * Code, File and Subject, as with ParseYaml.
 */
[[nodiscard]] FRT_CORE_API bool EmitCanonical (const CDataTree& Tree, const SEmitPolicy& Policy, FString& Out, SDiagnostic* OutError = nullptr);

// A number's canonical text: the shortest form std::to_chars finds that reads back as the same value at this precision
// (0.1f is "0.1", not the float's exact 0.100000001...), and ".inf", "-.inf", ".nan". For a builder of a tree from typed
// data: a float field's text must come from its own precision, since widening it to double first would print the noise.
FRT_CORE_API void AppendCanonicalNumber (FString& Out, double Value) noexcept;
FRT_CORE_API void AppendCanonicalNumber (FString& Out, float Value) noexcept;
}
