#pragma once

#include <filesystem>
#include <string>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/StringView.h"


namespace frt::assets::text
{
// One `key: value` line of a text asset (`key = value` in a line without ':'): key and value
// trimmed, and one pair of quotes (" or ') around the value removed. False for blank lines,
// comments (# or //) and lines without a separator. The views point into Line.
FRT_CORE_API bool TryParseKeyValue (FStringView Line, FStringView* OutKey, FStringView* OutValue);

// The value of `version`, when that is the file's first key (a trailing YAML comment ignored); 0
// otherwise, or when the file cannot be read or the version is not an integer. The file is decoded
// as platform::ReadFileText's Detect decodes it: a reader it is the check for must decode it so too.
FRT_CORE_API int32 GetTextAssetVersion (const std::filesystem::path& Path);

// std::string forms for callers whose text is not FString yet (the material serializers): thin
// wrappers over the view functions. Case and quotes as before: ASCII lowering, " or '.
FRT_CORE_API std::string TrimCopy (const std::string& Input);
FRT_CORE_API std::string ToLowerCopy (const std::string& Input);
FRT_CORE_API std::string StripQuotes (const std::string& Input);
FRT_CORE_API bool ParseFloat (const std::string& Input, float* OutValue);
FRT_CORE_API bool ParseBool (const std::string& Input, bool* OutValue);
FRT_CORE_API bool TryParseKeyValue (const std::string& Line, std::string* OutKey, std::string* OutValue);
}
