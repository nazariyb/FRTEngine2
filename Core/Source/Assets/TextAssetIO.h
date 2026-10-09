#pragma once

#include <filesystem>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/StringView.h"


namespace frt::assets::text
{
// One `key: value` line of a text asset (`key = value` in a line without ':'): key and value
// trimmed, and one pair of quotes (" or ') around the value removed. False for blank lines,
// comments (# or //) and lines without a separator. The views point into Line.
FRT_CORE_API bool TryParseKeyValue (FStringView Line, FStringView* OutKey, FStringView* OutValue);

// Value (from TryParseKeyValue) without a trailing YAML comment - from a '#' after a space or a tab -
// trimmed, and one pair of quotes around what is left removed: yaml-cpp reads `0.25  # glossy` as
// 0.25. A '#' with no whitespace before it is the value's ("3#x", "#FF8000"). For values that are
// not free text (numbers, flags, keywords, colours): a name or a path may hold " #".
FRT_CORE_API FStringView StripTrailingComment (FStringView Value);

// The value of `version`, when that is the file's first key (StripTrailingComment applied); 0
// otherwise, or when the file cannot be read or the version is not an integer. The file is decoded
// as platform::ReadFileText's Detect decodes it: a reader it is the check for must decode it so too.
FRT_CORE_API int32 GetTextAssetVersion (const std::filesystem::path& Path);
}
