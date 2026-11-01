#pragma once

#include <filesystem>
#include <span>

#include "Core.h"
#include "Memory/Ref.h"
#include "Assets/Serializer.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Functional/FunctionRef.h"
#include "Functional/MulticastDelegate.h"
// Complete type: the exported library's implicit members destroy TRefShared<SMaterial>.
#include "Graphics/Render/Material.h"


namespace frt::assets
{
class CFileWatcherService;
struct SFileChange;
}


namespace frt::graphics
{
class CRenderer;


/**
 * Materials by file: each .frtmat.yml is loaded once, however its path is spelled, and shared.
 *
 * Hot reload (dev builds). With a file watcher (SetFileWatcher; the renderer passes the engine's),
 * the library subscribes to the directory of every material it loads, for .frtmat.yml files, instead
 * of looking at every file's time each frame. The watcher's callback only notes which loaded
 * materials it touched (assets::IsFileAffected) and whose file's write time now differs from the one
 * they were read with - the library's own writes (create, upgrade on load) leave that time behind, so
 * they never read back - and ReloadModifiedMaterials, which the renderer calls once per frame before
 * the GPU scene reads materials, reads exactly those again. A material whose file was removed keeps
 * what it has; a Rescan (events were missed) compares every loaded material below it the same way.
 */
class FRT_CORE_API CMaterialLibrary
{
public:
	CMaterialLibrary ();
	// Ends its watcher subscriptions.
	~CMaterialLibrary ();
	// Pinned: its watcher subscriptions call back into it.
	CMaterialLibrary (const CMaterialLibrary&) = delete;
	CMaterialLibrary& operator= (const CMaterialLibrary&) = delete;

	// Access the serializer registry to register/replace backends, change default write format,
	// or query available formats. Per-project setup happens here.
	assets::CSerializerRegistry<SMaterial>& GetSerializers () { return Serializers; }
	const assets::CSerializerRegistry<SMaterial>& GetSerializers () const { return Serializers; }

	memory::TRefShared<SMaterial> LoadOrCreateMaterial (
		const std::filesystem::path& MaterialPath,
		const SMaterial& DefaultMaterial);

	// The file watcher materials reload through (Assets/FileWatcherService.h), or null for none: the
	// directories of the materials loaded so far are subscribed at once, those of later ones as they
	// load; null (or another watcher) ends the subscriptions. The watcher must outlive them. Without
	// one nothing reloads (Release, headless builds).
	void SetFileWatcher (assets::CFileWatcherService* Watcher);

	// Reads again every loaded material whose file the watcher reported changed: true when one was.
	// Its Version goes up, so the GPU scene uploads it again. Once per frame, before the scene reads
	// materials.
	bool ReloadModifiedMaterials ();

	// Every loaded material, in no particular order: for tools and tests (the editor's smoke test
	// edits one on a scratch copy).
	void ForEachMaterial (TFunctionRef<void (const memory::TRefShared<SMaterial>&)> Visit) const;

private:
	struct SMaterialRecord
	{
		memory::TRefShared<SMaterial> Material;
		// The watcher reported its file changed (with another write time): ReloadModifiedMaterials
		// reads it.
		bool bChanged = false;
	};


	memory::TRefShared<SMaterial> LoadMaterialFromFile (
		const std::filesystem::path& MaterialPath,
		const SMaterial& DefaultMaterial,
		bool bCreateIfMissing);

	bool ReadMaterial (const std::filesystem::path& Path, SMaterial& Out) const;
	bool WriteMaterial (const std::filesystem::path& Path, const SMaterial& In) const;
	void UpdateDerivedState (SMaterial& Material) const;

	// Subscribes MaterialPath's directory, unless it is already.
	void Watch (const std::filesystem::path& MaterialPath);
	void Unwatch ();
	void OnFilesChanged (std::span<const assets::SFileChange> Changes);

	static FString MakeKey (const std::filesystem::path& MaterialPath);

	assets::CSerializerRegistry<SMaterial> Serializers;
	TPathMap<SMaterialRecord> Materials;   // by MakeKey: the same file however it is spelled
	assets::CFileWatcherService* FileWatcher = nullptr;
	TPathMap<SDelegateHandle> WatchedDirectories;   // by MakeKey of the directory
	bool bAnyChanged = false;
};
}
