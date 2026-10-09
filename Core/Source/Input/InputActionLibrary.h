#pragma once

#include <filesystem>

#include "Core.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Input/InputActions.h"


namespace frt::input
{
struct SInputActionDefinitionAsset
{
	SInputActionDefinition Action;
	std::filesystem::path SourcePath;
	std::filesystem::file_time_type LastWriteTime;
};

struct SInputActionMapAsset
{
	FString Name;
	std::filesystem::path SourcePath;
	std::filesystem::file_time_type LastWriteTime;
	CInputActionMap ActionMap;
};


class FRT_CORE_API CInputActionLibrary
{
public:
	SInputActionDefinitionAsset* LoadOrCreateAction (FStringView Name);
	SInputActionMapAsset* LoadOrCreateActionMap (const std::filesystem::path& Path);
	bool ReloadModifiedActions ();
	bool ReloadModifiedActionMaps ();

	// Updates every loaded map's action states from this frame's device input. Once per frame
	// (the engine does it): consumers read states, and whether one may act on them - a focused
	// viewport, a game with input - is decided by whoever routes input, not by re-evaluating.
	void EvaluateActionMaps (const CInputSystem& Input);

	const SInputActionDefinitionAsset* FindAction (FStringView Name) const;
	SInputActionDefinitionAsset* FindAction (FStringView Name);

	SInputActionMapAsset* FindActionMap (const std::filesystem::path& Path);
	const SInputActionMapAsset* FindActionMap (const std::filesystem::path& Path) const;

private:
	SInputActionDefinitionAsset* LoadActionIfExists (FStringView Name);

	static std::filesystem::path MakeActionPath (FStringView Name);
	static FString MakeKey (const std::filesystem::path& Path);
	static FStringView MakeActionKey (FStringView Name);

private:
	TStringMap<SInputActionDefinitionAsset> ActionDefinitions;   // by name, case-sensitive
	TPathMap<SInputActionMapAsset> ActionMaps;                   // by MakeKey: the same file however it is spelled
};
}
