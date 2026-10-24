#pragma once

#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::assettool
{
/**
 * One JSON object, built field by field, for AssetTool's --json output: one object per line, so Lua,
 * Python and hook scripts read a stream line by line without a streaming parser (design §13.1).
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
class CJsonObject
{
public:
	CJsonObject& AddString (FStringView Name, FStringView Value);
	CJsonObject& AddInt (FStringView Name, int64 Value);
	CJsonObject& AddUInt (FStringView Name, uint64 Value);
	CJsonObject& AddBool (FStringView Name, bool Value);
	// Other's fields after these, in their order.
	CJsonObject& AddFields (const CJsonObject& Other);

	// The object's text, "{...}", without a line break.
	[[nodiscard]] FString ToString () const;

private:
	void AddName (FStringView Name);

	FString Fields;   // the members so far, comma-separated, without the braces
};

// Text as a JSON string literal, quotes included.
void AppendJsonString (FString& Out, FStringView Text);
}
