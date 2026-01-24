#pragma once

#include <filesystem>
#include <string>

#include "Core.h"
#include "CoreTypes.h"


namespace frt::assets::text
{
FRT_CORE_API std::string TrimCopy (const std::string& Input);
FRT_CORE_API std::string ToLowerCopy (const std::string& Input);
FRT_CORE_API std::string StripQuotes (const std::string& Input);

FRT_CORE_API bool ParseInt32 (const std::string& Input, int32* OutValue);
FRT_CORE_API bool ParseUInt32 (const std::string& Input, uint32* OutValue);
FRT_CORE_API bool ParseFloat (const std::string& Input, float* OutValue);
FRT_CORE_API bool ParseBool (const std::string& Input, bool* OutValue);

FRT_CORE_API bool TryParseKeyValue (const std::string& Line, std::string* OutKey, std::string* OutValue);
FRT_CORE_API int32 GetTextAssetVersion (const std::filesystem::path& Path);
}
