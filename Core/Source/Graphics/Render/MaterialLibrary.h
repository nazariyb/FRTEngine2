#pragma once

#include <filesystem>

#include "Core.h"
#include "Memory/Ref.h"
#include "Assets/Serializer.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
// Complete type: the exported library's implicit members destroy TRefShared<SMaterial>.
#include "Graphics/Render/Material.h"


namespace frt::graphics
{
class CRenderer;


class FRT_CORE_API CMaterialLibrary
{
public:
	CMaterialLibrary ();

	// Access the serializer registry to register/replace backends, change default write format,
	// or query available formats. Per-project setup happens here.
	assets::CSerializerRegistry<SMaterial>& GetSerializers () { return Serializers; }
	const assets::CSerializerRegistry<SMaterial>& GetSerializers () const { return Serializers; }

	memory::TRefShared<SMaterial> LoadOrCreateMaterial (
		const std::filesystem::path& MaterialPath,
		const SMaterial& DefaultMaterial);

	bool ReloadModifiedMaterials ();

private:
	struct SMaterialRecord
	{
		memory::TRefShared<SMaterial> Material;
	};


	memory::TRefShared<SMaterial> LoadMaterialFromFile (
		const std::filesystem::path& MaterialPath,
		const SMaterial& DefaultMaterial,
		bool bCreateIfMissing);

	bool ReadMaterial (const std::filesystem::path& Path, SMaterial& Out) const;
	bool WriteMaterial (const std::filesystem::path& Path, const SMaterial& In) const;
	void UpdateDerivedState (SMaterial& Material) const;

	static FString MakeKey (const std::filesystem::path& MaterialPath);

	assets::CSerializerRegistry<SMaterial> Serializers;
	TPathMap<SMaterialRecord> Materials;   // by MakeKey: the same file however it is spelled
};
}
