#pragma once

#include "Core.h"
#include "Containers/String.h"
#include "Reflect/DataTree.h"


namespace frt
{
struct SDiagnostic;
}


/**
 * YAML text -> CDataTree (design §5.5, §15 I15). Built on yaml-cpp's event parser (YAML::Parser with an event
 * handler of our own that feeds CDataTreeBuilder), never YAML::Load / YAML::Node: those drop scalar styles, merge
 * duplicate keys silently and resolve aliases, and CYamlReadArchive sits on the TLSF stack. yaml-cpp stays inside
 * DataTreeYaml.cpp: no Core header includes it.
 *
 * FRT's YAML is a strict subset, and anything outside it is an error with a position, never a silent guess:
 * - one document (an empty text, or one of comments only, gives an empty tree; a second document is an error);
 * - no anchors, aliases or tags (`&a`, `*a`, `!!str`, `!x`): FRT files never need them, and an alias would make one
 *   value appear in two places of a file the canonical emitter rewrites (§3.7);
 * - map keys are scalars (no complex keys) and unique within their map, byte for byte;
 * - nesting at most CDataTreeBuilder::DefaultMaxDepth maps and sequences deep;
 * - every quote closes (yaml-cpp 0.8.0 accepts "a: 'open" followed by a line break at the end of the text).
 * A plain key spelled like YAML's null (`null: 1`, `~: 1`) is the text it spells; an empty key is an error. A null
 * value (empty, `~` or `null`, as HDataNode::IsNull reads a tree from any builder) is a Null node; yaml-cpp's other
 * YAML 1.2 core spellings `Null` and `NULL` are plain scalars of that text.
 * A plain value cannot start with * & ! % @ or ` (an alias, anchor, tag, directive or reserved indicator): the error
 * at such a value says to quote it (an Assets.yml glob that starts with `**` is the usual case).
 *
 * Scalar styles come from yaml-cpp's non-specific tags ('?' plain, '!' quoted or block) refined by the source byte
 * at the scalar's mark (' " | >). Positions are yaml-cpp's marks plus one: Line from 1, Column from 1 in bytes of
 * the UTF-8 text. A leading UTF-8 byte order mark is skipped and not counted.
 *
 * Safe on worker threads (std containers and FString only). Callers read the file with platform::ReadFileText
 * (Detect), as CYamlReadArchive does, so UTF-16 files and BOMs arrive as UTF-8.
 */
namespace frt::reflect
{
// Why ParseYaml failed, for a caller that treats the two differently (E3 warns about a file the runtime's YAML::Load
// still reads, and keeps its record).
enum class EYamlFailure : uint8
{
	None,     // parsed
	Syntax,   // not YAML that yaml-cpp reads (YAML::Load refuses it too); the error is yaml-cpp's
	Subset,   // YAML that yaml-cpp reads to the end, outside FRT's subset: a duplicate key, an anchor, alias or tag, a
	          // complex or empty key, a second document, nesting past the depth limit, an unclosed quote at the end
};

/**
 * Parses Text into Out (replacing it). On failure Out is empty and OutError gets Severity Error, Line, Column (from 1;
 * 0 when yaml-cpp gives no position) and Message; the caller fills File, Subject and Code. OutFailure gets the kind
 * (None on success). After a subset violation the parse reads on to the end of the document without building, so
 * Subset means the rest is YAML too; when it is not, the result is Syntax with yaml-cpp's error. The one exception is
 * nesting deeper than CDataTreeBuilder::DefaultMaxDepth: the parse stops there (yaml-cpp's recursive descent would
 * overflow the stack in Debug first) and the result is Subset without the rest being read. Never throws.
 */
FRT_CORE_API bool ParseYaml (FStringView Text, CDataTree& Out, SDiagnostic* OutError = nullptr, EYamlFailure* OutFailure = nullptr);
}
