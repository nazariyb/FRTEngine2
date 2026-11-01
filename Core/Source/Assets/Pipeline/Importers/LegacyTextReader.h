#pragma once

#include "CoreTypes.h"
#include "Containers/String.h"
#include "Reflect/DataTree.h"


/**
 * The legacy line formats as CDataTrees (design §5.5: the legacy readers feed the same tree the schema-free scan
 * and, later, the tree cache walk). A v1 `.frtmat.yml`, a `.frtinputmap` and a `.frtinputaction` are not YAML -
 * the input files repeat `action:` and `bind:` - so they are read line by line with text::TryParseKeyValue, the
 * reader the runtime uses (CMaterialLegacyTextSerializer::Read, CInputActionMap::LoadFromFile,
 * LoadInputActionDefinition), and built with CDataTreeBuilder. Ports, not calls: the runtime's readers fill
 * TArray-based structs, which workers must not touch. Every node keeps its line and its byte column. Internal to
 * the importers.
 */
namespace frt::assets::importers
{
/**
 * A flat map of the text's `key: value` (or `key = value`) lines, as the runtime reads a v1 material or an input
 * action: a key that repeats keeps its last value (the runtime overwrites), at that line; lines without a
 * separator, comments and empty keys are skipped. Values are TryParseKeyValue's (trimmed, one pair of quotes
 * removed - the style says which), not comment-stripped: names and paths are taken as written.
 */
[[nodiscard]] reflect::CDataTree ReadKeyValueTree (FStringView Text);

/**
 * An input map as the runtime reads it: `version` (its last value), then `actions`, a sequence with one map per
 * `action:` line in file order - `action` (the name), `type` (the last `type:` after it, when there is one) and
 * `bind`, a sequence of its `bind:` values. Lines before the first action other than `version` are ignored, as the
 * runtime ignores them.
 */
[[nodiscard]] reflect::CDataTree ReadInputMapTree (FStringView Text);

// "Button" or "Axis" for an action's `type:` value as the runtime parses it (`_`, `-` and spaces dropped, ASCII
// case ignored; no comment stripping); empty when the runtime would ignore it.
[[nodiscard]] FStringView ParseInputActionKind (FStringView Value) noexcept;
}
