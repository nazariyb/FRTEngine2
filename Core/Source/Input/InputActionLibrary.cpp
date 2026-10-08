#include "Input/InputActionLibrary.h"

#include "EnginePaths.h"


namespace frt::input
{
namespace
{
	const char* InputActionExtension = ".frtinputaction";

	// Action definitions (.frtinputaction) named by a map: the project's Content/Input, or the
	// engine's without a project.
	std::filesystem::path ResolveInputContentRoot ()
	{
		return (paths::HasProject() ? paths::GetProjectContentDir() : paths::GetEngineContentDir()) / "Input";
	}

	std::filesystem::path NormalizePath (const std::filesystem::path& Path)
	{
		std::error_code ec;
		std::filesystem::path absolute = std::filesystem::absolute(Path, ec);
		if (ec)
		{
			return Path.lexically_normal();
		}
		return absolute.lexically_normal();
	}
}

SInputActionDefinitionAsset* CInputActionLibrary::LoadOrCreateAction (const std::string& Name)
{
	const std::string key = MakeActionKey(Name);
	auto it = ActionDefinitions.find(key);
	if (it != ActionDefinitions.end())
	{
		return &it->second;
	}

	SInputActionDefinitionAsset asset;
	asset.Action.Name = Name;
	asset.SourcePath = MakeActionPath(Name);

	std::error_code ec;
	const bool exists = std::filesystem::exists(asset.SourcePath, ec);
	if (!ec && exists)
	{
		(void)LoadInputActionDefinition(asset.SourcePath, &asset.Action);
	}
	else
	{
		(void)SaveInputActionDefinition(asset.SourcePath, asset.Action);
	}

	asset.LastWriteTime = std::filesystem::last_write_time(asset.SourcePath, ec);

	auto [insertedIt, inserted] = ActionDefinitions.emplace(key, std::move(asset));
	return &insertedIt->second;
}

SInputActionMapAsset* CInputActionLibrary::LoadOrCreateActionMap (const std::filesystem::path& Path)
{
	const std::filesystem::path resolvedPath = NormalizePath(Path);
	const std::string key = MakeKey(resolvedPath);
	auto it = ActionMaps.find(key);
	if (it != ActionMaps.end())
	{
		return &it->second;
	}

	SInputActionMapAsset asset;
	asset.SourcePath = resolvedPath;
	asset.Name = resolvedPath.stem().string();

	std::error_code ec;
	const bool exists = std::filesystem::exists(resolvedPath, ec);
	if (!ec && exists)
	{
		asset.ActionMap.LoadFromFile(resolvedPath);
	}
	else
	{
		asset.ActionMap.SaveToFile(resolvedPath);
	}

	asset.LastWriteTime = std::filesystem::last_write_time(resolvedPath, ec);

	auto [insertedIt, inserted] = ActionMaps.emplace(key, std::move(asset));
	return &insertedIt->second;
}

bool CInputActionLibrary::ReloadModifiedActions ()
{
	bool anyReloaded = false;

	for (auto& entry : ActionDefinitions)
	{
		SInputActionDefinitionAsset& asset = entry.second;
		if (asset.SourcePath.empty())
		{
			continue;
		}

		std::error_code ec;
		const std::filesystem::file_time_type newWriteTime =
			std::filesystem::last_write_time(asset.SourcePath, ec);
		if (ec || newWriteTime == asset.LastWriteTime)
		{
			continue;
		}

		if (LoadInputActionDefinition(asset.SourcePath, &asset.Action))
		{
			asset.LastWriteTime = newWriteTime;
			anyReloaded = true;
		}
	}

	return anyReloaded;
}

void CInputActionLibrary::EvaluateActionMaps (const CInputSystem& Input)
{
	for (auto& entry : ActionMaps)
	{
		entry.second.ActionMap.Evaluate(Input);
	}
}

bool CInputActionLibrary::ReloadModifiedActionMaps ()
{
	bool anyReloaded = false;

	for (auto& entry : ActionMaps)
	{
		SInputActionMapAsset& asset = entry.second;
		if (asset.SourcePath.empty())
		{
			continue;
		}

		std::error_code ec;
		const std::filesystem::file_time_type newWriteTime =
			std::filesystem::last_write_time(asset.SourcePath, ec);
		if (ec || newWriteTime == asset.LastWriteTime)
		{
			continue;
		}

		if (asset.ActionMap.LoadFromFile(asset.SourcePath))
		{
			asset.LastWriteTime = newWriteTime;
			anyReloaded = true;
		}
	}

	return anyReloaded;
}

const SInputActionDefinitionAsset* CInputActionLibrary::FindAction (const std::string& Name) const
{
	const std::string key = MakeActionKey(Name);
	auto it = ActionDefinitions.find(key);
	if (it == ActionDefinitions.end())
	{
		return nullptr;
	}

	return &it->second;
}

SInputActionDefinitionAsset* CInputActionLibrary::FindAction (const std::string& Name)
{
	return const_cast<SInputActionDefinitionAsset*>(
		static_cast<const CInputActionLibrary&>(*this).FindAction(Name));
}

SInputActionMapAsset* CInputActionLibrary::FindActionMap (const std::filesystem::path& Path)
{
	return const_cast<SInputActionMapAsset*>(
		static_cast<const CInputActionLibrary&>(*this).FindActionMap(Path));
}

const SInputActionMapAsset* CInputActionLibrary::FindActionMap (const std::filesystem::path& Path) const
{
	const std::string key = MakeKey(Path);
	auto it = ActionMaps.find(key);
	if (it == ActionMaps.end())
	{
		return nullptr;
	}

	return &it->second;
}

SInputActionDefinitionAsset* CInputActionLibrary::LoadActionIfExists (const std::string& Name)
{
	const std::filesystem::path path = MakeActionPath(Name);
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
	{
		return nullptr;
	}

	SInputActionDefinitionAsset asset;
	asset.SourcePath = path;
	if (!LoadInputActionDefinition(asset.SourcePath, &asset.Action))
	{
		return nullptr;
	}

	asset.LastWriteTime = std::filesystem::last_write_time(asset.SourcePath, ec);
	const std::string key = MakeActionKey(asset.Action.Name.empty() ? Name : asset.Action.Name);

	auto [insertedIt, inserted] = ActionDefinitions.emplace(key, std::move(asset));
	return &insertedIt->second;
}

std::filesystem::path CInputActionLibrary::MakeActionPath (const std::string& Name)
{
	std::filesystem::path path = ResolveInputContentRoot() / Name;
	if (!path.has_extension())
	{
		path.replace_extension(InputActionExtension);
	}
	return path;
}

std::string CInputActionLibrary::MakeKey (const std::filesystem::path& Path)
{
	return NormalizePath(Path).string();
}

std::string CInputActionLibrary::MakeActionKey (const std::string& Name)
{
	return Name;
}
}
