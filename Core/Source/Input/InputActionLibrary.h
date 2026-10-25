#pragma once

#include <filesystem>
#include <span>

#include "Core.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Functional/MulticastDelegate.h"
#include "Input/InputActions.h"


namespace frt::assets
{
class CFileWatcherService;
struct SFileChange;
}


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


/**
 * Input actions (.frtinputaction, by name) and action maps (.frtinputmap, by file), each loaded once.
 *
 * Hot reload (dev builds). With a file watcher (SetFileWatcher; the engine passes its own), the
 * library subscribes to the directory of every file it loads, for those two extensions, instead of
 * looking at every file's time each frame. The watcher's callback reads again, right there, each
 * loaded file it touched (assets::IsFileAffected) whose write time differs from the one it was read
 * with: the engine pumps the watcher in BeginFrame before it evaluates the maps, so a saved binding
 * applies in the same frame. A removed file keeps what was loaded; a Rescan (events were missed)
 * compares every loaded file below it the same way.
 */
class FRT_CORE_API CInputActionLibrary
{
public:
	// Pinned: the maps it holds own their action events' subscriptions (TMulticastDelegate), and its
	// watcher subscriptions call back into it. Said explicitly because an exported class
	// instantiates every implicit member, and the map container's copy would not compile.
	CInputActionLibrary () = default;
	// Ends its watcher subscriptions.
	~CInputActionLibrary ();
	CInputActionLibrary (const CInputActionLibrary&) = delete;
	CInputActionLibrary& operator= (const CInputActionLibrary&) = delete;

	SInputActionDefinitionAsset* LoadOrCreateAction (FStringView Name);
	SInputActionMapAsset* LoadOrCreateActionMap (const std::filesystem::path& Path);

	// The file watcher actions and maps reload through (Assets/FileWatcherService.h), or null for
	// none: the directories of the files loaded so far are subscribed at once, those of later ones as
	// they load; null (or another watcher) ends the subscriptions. The watcher must outlive them, so
	// the engine passes null before it destroys its own. Without one nothing reloads (Release).
	void SetFileWatcher (assets::CFileWatcherService* Watcher);

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

	// Subscribes Path's directory, unless it is already.
	void Watch (const std::filesystem::path& Path);
	void Unwatch ();
	void OnFilesChanged (std::span<const assets::SFileChange> Changes);

	static std::filesystem::path MakeActionPath (FStringView Name);
	static FString MakeKey (const std::filesystem::path& Path);
	static FStringView MakeActionKey (FStringView Name);

private:
	TStringMap<SInputActionDefinitionAsset> ActionDefinitions;   // by name, case-sensitive
	TPathMap<SInputActionMapAsset> ActionMaps;                   // by MakeKey: the same file however it is spelled
	assets::CFileWatcherService* FileWatcher = nullptr;
	TPathMap<SDelegateHandle> WatchedDirectories;                // by MakeKey of the directory
};
}
