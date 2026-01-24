#include "Assets/TextAssetIO.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>


namespace frt::assets::text
{
std::string TrimCopy (const std::string& Input)
{
	size_t start = 0;
	while (start < Input.size() && std::isspace(static_cast<unsigned char>(Input[start])))
	{
		++start;
	}

	size_t end = Input.size();
	while (end > start && std::isspace(static_cast<unsigned char>(Input[end - 1u])))
	{
		--end;
	}

	return Input.substr(start, end - start);
}

std::string ToLowerCopy (const std::string& Input)
{
	std::string result = Input;
	for (char& ch : result)
	{
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return result;
}

std::string StripQuotes (const std::string& Input)
{
	if (Input.size() < 2u)
	{
		return Input;
	}

	const char first = Input.front();
	const char last = Input.back();
	if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
	{
		return Input.substr(1u, Input.size() - 2u);
	}

	return Input;
}

bool ParseInt32 (const std::string& Input, int32* OutValue)
{
	const char* cstr = Input.c_str();
	char* end = nullptr;
	const long value = std::strtol(cstr, &end, 10);
	if (end == cstr)
	{
		return false;
	}

	*OutValue = static_cast<int32>(value);
	return true;
}

bool ParseUInt32 (const std::string& Input, uint32* OutValue)
{
	const char* cstr = Input.c_str();
	char* end = nullptr;
	const unsigned long value = std::strtoul(cstr, &end, 10);
	if (end == cstr)
	{
		return false;
	}

	*OutValue = static_cast<uint32>(value);
	return true;
}

bool ParseFloat (const std::string& Input, float* OutValue)
{
	const char* cstr = Input.c_str();
	char* end = nullptr;
	const float value = std::strtof(cstr, &end);
	if (end == cstr)
	{
		return false;
	}

	*OutValue = value;
	return true;
}

bool ParseBool (const std::string& Input, bool* OutValue)
{
	const std::string normalized = ToLowerCopy(Input);
	if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
	{
		*OutValue = true;
		return true;
	}

	if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
	{
		*OutValue = false;
		return true;
	}

	return false;
}

bool TryParseKeyValue (const std::string& Line, std::string* OutKey, std::string* OutValue)
{
	std::string trimmed = TrimCopy(Line);
	if (trimmed.empty())
	{
		return false;
	}

	if (trimmed.rfind("#", 0) == 0u || trimmed.rfind("//", 0) == 0u)
	{
		return false;
	}

	size_t split = trimmed.find(':');
	if (split == std::string::npos)
	{
		split = trimmed.find('=');
	}

	if (split == std::string::npos)
	{
		return false;
	}

	std::string key = TrimCopy(trimmed.substr(0, split));
	std::string value = TrimCopy(trimmed.substr(split + 1));
	value = StripQuotes(value);

	if (OutKey)
	{
		*OutKey = std::move(key);
	}
	if (OutValue)
	{
		*OutValue = std::move(value);
	}

	return true;
}

int32 GetTextAssetVersion (const std::filesystem::path& Path)
{
	std::ifstream stream(Path);
	if (!stream.is_open())
	{
		return 0;
	}

	std::string line;
	while (std::getline(stream, line))
	{
		std::string key;
		std::string value;
		if (!TryParseKeyValue(line, &key, &value))
		{
			continue;
		}

		if (key == "version")
		{
			int32 version = 0;
			if (ParseInt32(value, &version))
			{
				return version;
			}
		}

		break;
	}

	return 0;
}
}
