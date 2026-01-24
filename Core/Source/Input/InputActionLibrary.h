#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "Core.h"
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
	std::string Name;
	std::filesystem::path SourcePath;
	std::filesystem::file_time_type LastWriteTime;
	CInputActionMap ActionMap;
};


class FRT_CORE_API CInputActionLibrary
{
public:
	SInputActionDefinitionAsset* LoadOrCreateAction (const std::string& Name);
	SInputActionMapAsset* LoadOrCreateActionMap (const std::filesystem::path& Path);
	bool ReloadModifiedActions ();
	bool ReloadModifiedActionMaps ();

	const SInputActionDefinitionAsset* FindAction (const std::string& Name) const;
	SInputActionDefinitionAsset* FindAction (const std::string& Name);

	SInputActionMapAsset* FindActionMap (const std::filesystem::path& Path);
	const SInputActionMapAsset* FindActionMap (const std::filesystem::path& Path) const;

private:
	SInputActionDefinitionAsset* LoadActionIfExists (const std::string& Name);

	static std::filesystem::path MakeActionPath (const std::string& Name);
	static std::string MakeKey (const std::filesystem::path& Path);
	static std::string MakeActionKey (const std::string& Name);

private:
	std::unordered_map<std::string, SInputActionDefinitionAsset> ActionDefinitions;
	std::unordered_map<std::string, SInputActionMapAsset> ActionMaps;
};
}
