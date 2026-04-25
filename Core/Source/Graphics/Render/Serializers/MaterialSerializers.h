#pragma once

#include "Core.h"
#include "Assets/Serializer.h"
#include "Graphics/Render/Material.h"


namespace frt::graphics
{
// YAML backend: uses the visitor archive + free Serialize() overloads.
// Reads/writes v2 .frtmat.yml files.
class FRT_CORE_API CMaterialYamlSerializer : public assets::ISerializer<SMaterial>
{
public:
	assets::EAssetFormat GetFormat () const override { return assets::EAssetFormat::Yaml; }
	bool CanRead (const std::filesystem::path& Path) const override;
	bool Read (const std::filesystem::path& Path, SMaterial& Out) const override;
	bool Write (const std::filesystem::path& Path, const SMaterial& In) const override;
};

// Legacy custom-text backend (v1). Read-only — kept for migration.
// CanRead matches files where `version: 1`. Write returns false.
class FRT_CORE_API CMaterialLegacyTextSerializer : public assets::ISerializer<SMaterial>
{
public:
	assets::EAssetFormat GetFormat () const override { return assets::EAssetFormat::LegacyText; }
	bool CanRead (const std::filesystem::path& Path) const override;
	bool Read (const std::filesystem::path& Path, SMaterial& Out) const override;
	bool Write (const std::filesystem::path& Path, const SMaterial& In) const override;
};
}
