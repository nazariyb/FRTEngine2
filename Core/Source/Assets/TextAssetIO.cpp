#include "Assets/TextAssetIO.h"

#include <optional>

#include "Containers/String.h"
#include "Platform/Platform.h"


namespace frt::assets::text
{
namespace
{
constexpr FStringLiteral Quotes = "\"'";
}

bool TryParseKeyValue (FStringView Line, FStringView* OutKey, FStringView* OutValue)
{
	const FStringView trimmed = Line.TrimStartAndEnd();
	if (trimmed.IsEmpty() || trimmed.StartsWith('#') || trimmed.StartsWith("//"))
	{
		return false;
	}

	// The first ':' separates; '=' only where there is none.
	SStringSplit split = trimmed.SplitFirst(':');
	if (!split)
	{
		split = trimmed.SplitFirst('=');
		if (!split)
		{
			return false;
		}
	}

	if (OutKey)
	{
		*OutKey = split.Left.TrimStartAndEnd();
	}
	if (OutValue)
	{
		*OutValue = split.Right.TrimStartAndEnd().TrimQuotes(Quotes);
	}
	return true;
}

FStringView StripTrailingComment (FStringView Value)
{
	return Value.Left(Value.FindAny({ " #", "\t#" })).TrimEnd().TrimQuotes(Quotes);
}

int32 GetTextAssetVersion (const std::filesystem::path& Path)
{
	// Detect: a UTF-8 BOM would otherwise be part of the first key.
	const std::optional<FString> text = platform::ReadFileText(Path);
	if (!text)
	{
		return 0;
	}

	for (const FStringView line : text->Lines())
	{
		FStringView key;
		FStringView value;
		if (TryParseKeyValue(line, &key, &value))
		{
			// The first key decides: a file that does not open with its version has none.
			if (key != "version")
			{
				return 0;
			}

			// yaml-cpp reads `version: 3  # note` as 3, and a probe that did not would leave the file
			// to no reader. "3x" and "3#x" stay malformed.
			return StripTrailingComment(value).ToInt32().value_or(0);
		}
	}
	return 0;
}
}
