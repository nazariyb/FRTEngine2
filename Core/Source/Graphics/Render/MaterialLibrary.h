#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "Core.h"
#include "Memory/Ref.h"
#include "Assets/Serializer.h"


namespace frt::graphics
{
struct SMaterial;
class CRenderer;


class FRT_CORE_API CMaterialLibrary
{
public:
	CMaterialLibrary ();

	void SetRenderer (CRenderer* InRenderer);

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
	void EnsureBaseColorTexture (SMaterial& Material) const;

	static std::string MakeKey (const std::filesystem::path& MaterialPath);

	assets::CSerializerRegistry<SMaterial> Serializers;
	std::unordered_map<std::string, SMaterialRecord> Materials;
	CRenderer* Renderer = nullptr;
};
}
