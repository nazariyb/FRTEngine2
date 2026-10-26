#pragma once

#include <span>

#include "Core.h"
#include "Containers/String.h"


namespace frt::json
{
/**
 * One JSON object, built field by field: AssetTool's --json output and the JSON-lines files the engine
 * writes (Local/Assets/Rules.cache). One object per line, so Lua, Python and hook scripts read a stream
 * line by line without a streaming parser (design §13.1). Arrays and nested objects exist for the few
 * outputs that carry lists per line (AssetTool's dump of a registry record). Reflect/DataTreeJson.h reads such a line
 * back, surrogate escapes included: the one writer and its one reader.
 *
 * Field names are the stable interface: once a verb prints a field, scripts may rely on it, so a field
 * is renamed or removed only with the verb's documented output. Fields keep the order they are added
 * in, which keeps the output diffable; nothing here checks that a name is used once.
 *
 * Text is escaped as RFC 8259 requires and stays UTF-8. Engine text is canonical WTF-8 (paths with
 * unpaired surrogates, Containers/String.h): a surrogate is written as its \uXXXX escape, which JSON
 * readers accept, and a byte that is not UTF-8 at all as �. The adders have distinct names on
 * purpose: one overloaded Add would take a string literal as bool.
 */
class FRT_CORE_API CJsonObject
{
public:
	CJsonObject& AddString (FStringView Name, FStringView Value);
	CJsonObject& AddInt (FStringView Name, int64 Value);
	CJsonObject& AddUInt (FStringView Name, uint64 Value);
	CJsonObject& AddBool (FStringView Name, bool Value);
	// An array of strings, and an array of objects (the registry dump's tags, edges and sub-entries).
	CJsonObject& AddStringArray (FStringView Name, std::span<const FString> Values);
	CJsonObject& AddObjectArray (FStringView Name, std::span<const CJsonObject> Values);
	// Another object as this field's value.
	CJsonObject& AddObject (FStringView Name, const CJsonObject& Value);
	// Other's fields after these, in their order.
	CJsonObject& AddFields (const CJsonObject& Other);

	// The object's text, "{...}", without a line break.
	[[nodiscard]] FString ToString () const;

private:
	void AddName (FStringView Name);

	FString Fields;   // the members so far, comma-separated, without the braces
};

// Text as a JSON string literal, quotes included.
FRT_CORE_API void AppendJsonString (FString& Out, FStringView Text);
}
