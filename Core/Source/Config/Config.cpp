#include "Config/Config.h"

#include "EnginePaths.h"


namespace frt::config
{
namespace
{
// Parses the whole token or nothing. A partial parse is rejected on purpose: "1920x1080"
// must not quietly read back as 1920, and "12abc" must not read back as 12. Both would
// otherwise look like a working setting while meaning something else.
template <concepts::StringNumber T>
T ParseNumber (const FString* Raw, T Default)
{
	if (Raw == nullptr)
	{
		return Default;
	}

	return Raw->TryParse<T>().value_or(Default);
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


bool CConfig::Has (FStringView Section, FStringView Key) const
{
	return FindValue(Section, Key) != nullptr;
}


void CConfig::ResetUserLayer ()
{
	Layers[static_cast<size_t>(EConfigLayer::User)].Clear();
}


void CConfig::SetOverride (FStringView Section, FStringView Key, FStringView Value)
{
	Layers[static_cast<size_t>(EConfigLayer::CommandLine)].Set(Section, Key, Value);
}


uint32 CConfig::ApplyCommandLineArguments (std::span<const FString> Arguments)
{
	uint32 applied = 0u;
	for (const FString& argument : Arguments)
	{
		// The first '=' ends the name, and the first ':' in the name ends the section: a value
		// may hold both (-Automation:CapturePath=C:/Captures).
		const SStringSplit nameAndValue = argument.View().TrimStart("-").SplitFirst('=');
		const SStringSplit sectionAndKey = nameAndValue.Left.SplitFirst(':');
		if (!nameAndValue || !sectionAndKey || sectionAndKey.Left.IsEmpty())
		{
			continue;
		}
		SetOverride(sectionAndKey.Left, sectionAndKey.Right, nameAndValue.Right);
		++applied;
	}
	return applied;
}


const FString* CConfig::FindValue (FStringView Section, FStringView Key) const
{
	// Strongest layer first; the first hit wins.
	for (int32 i = static_cast<int32>(EConfigLayer::Count) - 1; i >= 0; --i)
	{
		if (const FString* found = Layers[i].Find(Section, Key))
		{
			return found;
		}
	}
	return nullptr;
}


void CConfig::SetValue (FStringView Section, FStringView Key, FStringView Value)
{
	Layers[static_cast<size_t>(EConfigLayer::User)].Set(Section, Key, Value);
}


int32 CConfig::Get (FStringView Section, FStringView Key, int32 Default) const
{
	return ParseNumber(FindValue(Section, Key), Default);
}


uint32 CConfig::Get (FStringView Section, FStringView Key, uint32 Default) const
{
	// An unsigned parse rejects a leading '-', so a negative in the file falls back to the
	// default rather than wrapping to a huge value.
	return ParseNumber(FindValue(Section, Key), Default);
}


float CConfig::Get (FStringView Section, FStringView Key, float Default) const
{
	return ParseNumber(FindValue(Section, Key), Default);
}


FString CConfig::Get (FStringView Section, FStringView Key, FStringView Default) const
{
	const FString* raw = FindValue(Section, Key);
	return raw != nullptr ? *raw : FString(Default);
}


void CConfig::Set (FStringView Section, FStringView Key, int32 Value)
{
	SetValue(Section, Key, FString::FromNumber(Value));
}


void CConfig::Set (FStringView Section, FStringView Key, uint32 Value)
{
	SetValue(Section, Key, FString::FromNumber(Value));
}


void CConfig::Set (FStringView Section, FStringView Key, float Value)
{
	// The shortest representation that reads back as the same float, which is what makes
	// Set -> Save -> Load -> Get an identity rather than a slow drift.
	SetValue(Section, Key, FString::FromNumber(Value));
}


void CConfig::Set (FStringView Section, FStringView Key, FStringView Value)
{
	SetValue(Section, Key, Value);
}
}
