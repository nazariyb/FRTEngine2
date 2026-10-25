#include "Input/InputActionLibrary.h"

#include "EnginePaths.h"
#include "Assets/FileWatcherService.h"


namespace frt::input
{
namespace
{
	constexpr FStringLiteral InputActionExtension = ".frtinputaction";
	constexpr FStringLiteral InputActionMapExtension = ".frtinputmap";

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

CInputActionLibrary::~CInputActionLibrary ()
{
	Unwatch();
}

SInputActionDefinitionAsset* CInputActionLibrary::LoadOrCreateAction (FStringView Name)
{
	const FStringView key = MakeActionKey(Name);
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

	auto [insertedIt, inserted] = ActionDefinitions.emplace(FString(key), std::move(asset));
	Watch(insertedIt->second.SourcePath);
	return &insertedIt->second;
}

SInputActionMapAsset* CInputActionLibrary::LoadOrCreateActionMap (const std::filesystem::path& Path)
{
	const std::filesystem::path resolvedPath = NormalizePath(Path);
	const FString key = MakeKey(resolvedPath);
	auto it = ActionMaps.find(key);
	if (it != ActionMaps.end())
	{
		return &it->second;
	}

	SInputActionMapAsset asset;
	asset.SourcePath = resolvedPath;
	asset.Name = FString::FromPath(resolvedPath.stem());

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
	Watch(resolvedPath);
	return &insertedIt->second;
}

void CInputActionLibrary::EvaluateActionMaps (const CInputSystem& Input)
{
	for (auto& entry : ActionMaps)
	{
		entry.second.ActionMap.Evaluate(Input);
	}
}

void CInputActionLibrary::SetFileWatcher (assets::CFileWatcherService* Watcher)
{
	if (Watcher == FileWatcher)
	{
		return;
	}
	Unwatch();
	FileWatcher = Watcher;
	for (const auto& entry : ActionDefinitions)
	{
		Watch(entry.second.SourcePath);
	}
	for (const auto& entry : ActionMaps)
	{
		Watch(entry.second.SourcePath);
	}
}

void CInputActionLibrary::Watch (const std::filesystem::path& Path)
{
	if (!FileWatcher || Path.empty())
	{
		return;
	}
	const std::filesystem::path directory = Path.parent_path();
	const FString key = MakeKey(directory);
	if (WatchedDirectories.contains(key))
	{
		return;
	}
	assets::SWatchFilter filter;
	filter.Extensions.push_back(InputActionExtension);
	filter.Extensions.push_back(InputActionMapExtension);
	const SDelegateHandle handle = FileWatcher->Subscribe(directory, filter,
		[this] (std::span<const assets::SFileChange> Changes) { OnFilesChanged(Changes); });
	WatchedDirectories.try_emplace(key, handle);
}

void CInputActionLibrary::Unwatch ()
{
	if (FileWatcher)
	{
		for (const auto& entry : WatchedDirectories)
		{
			FileWatcher->Unsubscribe(entry.second);
		}
	}
	WatchedDirectories.clear();
	FileWatcher = nullptr;
}

void CInputActionLibrary::OnFilesChanged (std::span<const assets::SFileChange> Changes)
{
	// Level-triggered: a change only says where to look. A file is read again when its write time
	// differs from the one it was read with - the time taken before the content, so a write that
	// lands while it is read leaves a newer time behind, and its own report reads it again. A read
	// that fails keeps the old time: the next save tries again.
	const auto changedSince = [Changes] (const std::filesystem::path& Path, std::filesystem::file_time_type LastWriteTime,
		std::filesystem::file_time_type& OutWriteTime)
	{
		if (Path.empty())
		{
			return false;
		}
		for (const assets::SFileChange& change : Changes)
		{
			if (assets::IsFileAffected(change, Path))
			{
				std::error_code ec;
				OutWriteTime = std::filesystem::last_write_time(Path, ec);
				return !ec && OutWriteTime != LastWriteTime;
			}
		}
		return false;
	};

	for (auto& entry : ActionDefinitions)
	{
		SInputActionDefinitionAsset& asset = entry.second;
		std::filesystem::file_time_type writeTime;
		if (changedSince(asset.SourcePath, asset.LastWriteTime, writeTime) && LoadInputActionDefinition(asset.SourcePath, &asset.Action))
		{
			asset.LastWriteTime = writeTime;
		}
	}
	for (auto& entry : ActionMaps)
	{
		SInputActionMapAsset& asset = entry.second;
		std::filesystem::file_time_type writeTime;
		if (changedSince(asset.SourcePath, asset.LastWriteTime, writeTime) && asset.ActionMap.LoadFromFile(asset.SourcePath))
		{
			asset.LastWriteTime = writeTime;
		}
	}
}

const SInputActionDefinitionAsset* CInputActionLibrary::FindAction (FStringView Name) const
{
	auto it = ActionDefinitions.find(MakeActionKey(Name));
	if (it == ActionDefinitions.end())
	{
		return nullptr;
	}

	return &it->second;
}

SInputActionDefinitionAsset* CInputActionLibrary::FindAction (FStringView Name)
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
	auto it = ActionMaps.find(MakeKey(Path));
	if (it == ActionMaps.end())
	{
		return nullptr;
	}

	return &it->second;
}

SInputActionDefinitionAsset* CInputActionLibrary::LoadActionIfExists (FStringView Name)
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
	FString key(MakeActionKey(asset.Action.Name.IsEmpty() ? Name : asset.Action.Name.View()));

	auto [insertedIt, inserted] = ActionDefinitions.emplace(std::move(key), std::move(asset));
	Watch(insertedIt->second.SourcePath);
	return &insertedIt->second;
}

std::filesystem::path CInputActionLibrary::MakeActionPath (FStringView Name)
{
	std::filesystem::path path = ResolveInputContentRoot() / Name.ToPath();
	if (!path.has_extension())
	{
		path.replace_extension(InputActionExtension.ToPath());
	}
	return path;
}

FString CInputActionLibrary::MakeKey (const std::filesystem::path& Path)
{
	// A TPathMap key: case-insensitive as NTFS is, so "IAM_Editor" and "iam_editor" load once.
	return FString::FromPath(NormalizePath(Path), EPathStyle::Generic);
}

FStringView CInputActionLibrary::MakeActionKey (FStringView Name)
{
	return Name;
}
}
