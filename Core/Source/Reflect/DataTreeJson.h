#pragma once

#include "Core.h"
#include "Containers/String.h"
#include "Reflect/DataTree.h"


namespace frt
{
struct SDiagnostic;
}


/**
 * JSON text -> CDataTree: the reader of the JSON lines the engine writes with Text/JsonWriter.h (Rules.cache, and
 * whatever a later file of that kind needs), so what the writer escapes the reader takes back byte for byte.
 *
 * Why not ParseYaml (JSON is nearly a YAML subset): the writer spells an unpaired WTF-8 surrogate - a Windows file or
 * folder name may hold one, and the engine keeps it in its on-disk spelling - as its \u escape, and yaml-cpp refuses
 * every \u escape from D800 to DFFF. This reader decodes an escaped surrogate pair to its code point and an escaped
 * lone surrogate back to its three WTF-8 bytes, the writer's exact inverse.
 *
 * The tree: objects are maps (flow style; a duplicate key is an error, as everywhere in CDataTree), arrays are
 * sequences, strings are double-quoted scalars (AsText), numbers, true and false are plain scalars of their text
 * (AsInt64, AsDouble, AsBool), null is a Null node. Strict RFC 8259 otherwise: one value, no comments, no trailing
 * commas, no leading '+' or zeros, raw control characters inside strings refused, input that is not UTF-8 refused.
 * Positions: Line from 1, Column from 1 in bytes, as ParseYaml's. Nesting deeper than CDataTreeBuilder's limit is an
 * error. Never throws; safe on worker threads.
 */
namespace frt::reflect
{
// Parses Text into Out (replacing it). On failure Out is empty and OutError gets Severity Error, Line, Column and
// Message; the caller fills File, Subject and Code.
FRT_CORE_API bool ParseJson (FStringView Text, CDataTree& Out, SDiagnostic* OutError = nullptr);
}
