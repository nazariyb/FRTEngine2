#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "Core.h"
#include "Memory/Ref.h"


namespace frt::graphics
{
struct SMaterial;
class CRenderer;


class FRT_CORE_API CMaterialLibrary
{
public:
	void SetRenderer (CRenderer* InRenderer);

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

	bool ParseMaterialFile (const std::filesystem::path& MaterialPath, SMaterial& Material) const;
	bool SaveMaterialFile (const std::filesystem::path& MaterialPath, const SMaterial& Material) const;
	void EnsureBaseColorTexture (SMaterial& Material) const;

	static std::string MakeKey (const std::filesystem::path& MaterialPath);

	std::unordered_map<std::string, SMaterialRecord> Materials;
	CRenderer* Renderer = nullptr;
};
}
