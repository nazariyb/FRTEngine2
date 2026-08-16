#include "Config/Config.h"

#include <charconv>
#include <format>
#include <initializer_list>

#include "EnginePaths.h"


namespace frt::config
{
namespace
{
// Parses the whole token or nothing. A partial parse is rejected on purpose: "1920x1080"
// must not quietly read back as 1920, and "12abc" must not read back as 12. Both would
// otherwise look like a working setting while meaning something else.
template <typename T>
T ParseNumber (const std::string* Raw, T Default)
{
	if (Raw == nullptr)
	{
		return Default;
	}

	const char* begin = Raw->data();
	const char* end = begin + Raw->size();

	T value{};
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if (result.ec != std::errc{} || result.ptr != end)
	{
		return Default;
	}

	return value;
}
}


void CConfig::LoadDefaultLayers ()
{
	LoadLayer(EConfigLayer::Engine, paths::GetEngineConfigPath());
	LoadLayer(EConfigLayer::Project, paths::GetProjectConfigPath());
	LoadLayer(EConfigLayer::User, paths::GetUserConfigPath());
}


bool CConfig::LoadLayer (EConfigLayer Layer, const std::filesystem::path& Path)
{
	frt_assert(Layer >= EConfigLayer::Engine && Layer < EConfigLayer::Count)

	if (Layer == EConfigLayer::User)
	{
		// Recorded even when the file does not exist yet: a first run has nothing to load
		// but still needs somewhere to save to.
		UserLayerPath = Path;
	}

	return Layers[static_cast<size_t>(Layer)].LoadFromFile(Path);
}


bool CConfig::Save () const
{
	if (UserLayerPath.empty())
	{
		return false;
	}

	return Layers[static_cast<size_t>(EConfigLayer::User)].SaveToFile(UserLayerPath);
}


bool CConfig::Has (std::string_view Section, std::string_view Key) const
{
	return FindValue(Section, Key) != nullptr;
}


void CConfig::ResetUserLayer ()
{
	Layers[static_cast<size_t>(EConfigLayer::User)].Clear();
}


const std::string* CConfig::FindValue (std::string_view Section, std::string_view Key) const
{
	// Strongest layer first; the first hit wins.
	for (int32 i = static_cast<int32>(EConfigLayer::Count) - 1; i >= 0; --i)
	{
		if (const std::string* found = Layers[i].Find(Section, Key))
		{
			return found;
		}
	}
	return nullptr;
}


void CConfig::SetValue (std::string_view Section, std::string_view Key, std::string_view Value)
{
	Layers[static_cast<size_t>(EConfigLayer::User)].Set(Section, Key, Value);
}


bool CConfig::Get (std::string_view Section, std::string_view Key, bool Default) const
{
	const std::string* raw = FindValue(Section, Key);
	if (raw == nullptr)
	{
		return Default;
	}

	// The spellings people actually type. Anything else is treated as absent rather than
	// as false, so a typo keeps the default instead of silently disabling a feature.
	for (const std::string_view text : { "true", "1", "yes", "on" })
	{
		if (enum_::EqualsIgnoreCase(*raw, text))
		{
			return true;
		}
	}

	for (const std::string_view text : { "false", "0", "no", "off" })
	{
		if (enum_::EqualsIgnoreCase(*raw, text))
		{
			return false;
		}
	}

	return Default;
}


int32 CConfig::Get (std::string_view Section, std::string_view Key, int32 Default) const
{
	return ParseNumber(FindValue(Section, Key), Default);
}


uint32 CConfig::Get (std::string_view Section, std::string_view Key, uint32 Default) const
{
	// from_chars rejects a leading '-' for an unsigned type, so a negative in the file
	// falls back to the default rather than wrapping to a huge value.
	return ParseNumber(FindValue(Section, Key), Default);
}


float CConfig::Get (std::string_view Section, std::string_view Key, float Default) const
{
	return ParseNumber(FindValue(Section, Key), Default);
}


std::string CConfig::Get (std::string_view Section, std::string_view Key, std::string_view Default) const
{
	const std::string* raw = FindValue(Section, Key);
	return raw != nullptr ? *raw : std::string(Default);
}


std::string CConfig::Get (std::string_view Section, std::string_view Key, const char* Default) const
{
	return Get(Section, Key, std::string_view(Default != nullptr ? Default : ""));
}

void CConfig::Get (
	char* OutBuffer,
	uint8 BufferSize,
	std::string_view Section,
	std::string_view Key,
	const char* Default) const
{
	frt_assert(BufferSize > 0);

	const std::string* raw = FindValue(Section, Key);
	std::string_view value = raw != nullptr ? *raw : std::string_view(Default != nullptr ? Default : "");

	if (value.size() >= BufferSize)
	{
		// TODO: warning
	}

	const size_t copySize = std::min(static_cast<size_t>(BufferSize), value.size());
	memcpy(OutBuffer, value.data(), copySize);
	OutBuffer[copySize] = '\0';
}


void CConfig::Set (std::string_view Section, std::string_view Key, bool Value)
{
	SetValue(Section, Key, Value ? "true" : "false");
}


void CConfig::Set (std::string_view Section, std::string_view Key, int32 Value)
{
	SetValue(Section, Key, std::to_string(Value));
}


void CConfig::Set (std::string_view Section, std::string_view Key, uint32 Value)
{
	SetValue(Section, Key, std::to_string(Value));
}


void CConfig::Set (std::string_view Section, std::string_view Key, float Value)
{
	// std::format gives the shortest representation that reads back as the same float,
	// which is what makes Set -> Save -> Load -> Get an identity rather than a slow drift.
	SetValue(Section, Key, std::format("{}", Value));
}


void CConfig::Set (std::string_view Section, std::string_view Key, std::string_view Value)
{
	SetValue(Section, Key, Value);
}


void CConfig::Set (std::string_view Section, std::string_view Key, const char* Value)
{
	SetValue(Section, Key, std::string_view(Value != nullptr ? Value : ""));
}
}
