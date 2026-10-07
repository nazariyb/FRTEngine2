#include "MaterialLibrary.h"

#include "Material.h"

#include "Graphics/Render/Serializers/MaterialSerialize.h"
#include "Graphics/Render/Serializers/MaterialSerializers.h"
#include "Memory/Memory.h"


namespace frt::graphics
{
CMaterialLibrary::CMaterialLibrary ()
{
	// Default per-project setup. Override by calling GetSerializers() after construction.
	// Order matters for read dispatch: YAML (v2) is tried first, legacy text (v1) is the fallback.
	Serializers.Register(std::make_unique<CMaterialYamlSerializer>());
	Serializers.Register(std::make_unique<CMaterialLegacyTextSerializer>());
	Serializers.SetDefaultWriteFormat(assets::EAssetFormat::Yaml);
}

memory::TRefShared<SMaterial> CMaterialLibrary::LoadOrCreateMaterial (
	const std::filesystem::path& MaterialPath,
	const SMaterial& DefaultMaterial)
{
	const std::string key = MakeKey(MaterialPath);
	auto it = Materials.find(key);
	if (it != Materials.end())
	{
		return it->second.Material;
	}

	SMaterialRecord record;
	record.Material = LoadMaterialFromFile(MaterialPath, DefaultMaterial, true);
	Materials.emplace(key, record);
	return record.Material;
}

bool CMaterialLibrary::ReloadModifiedMaterials ()
{
	bool anyReloaded = false;
	for (auto& entry : Materials)
	{
		if (!entry.second.Material)
		{
			continue;
		}

		SMaterial& material = *entry.second.Material;
		if (material.SourcePath.empty())
		{
			continue;
		}

		std::error_code ec;
		const std::filesystem::file_time_type newWriteTime =
			std::filesystem::last_write_time(material.SourcePath, ec);
		if (ec || newWriteTime == material.LastWriteTime)
		{
			continue;
		}

		if (ReadMaterial(material.SourcePath, material))
		{
			material.LastWriteTime = newWriteTime;
			UpdateDerivedState(material);
			anyReloaded = true;
		}
	}

	return anyReloaded;
}

memory::TRefShared<SMaterial> CMaterialLibrary::LoadMaterialFromFile (
	const std::filesystem::path& MaterialPath,
	const SMaterial& DefaultMaterial,
	bool bCreateIfMissing)
{
	SMaterial material = DefaultMaterial;
	material.SourcePath = MaterialPath;

	std::error_code ec;
	const bool exists = std::filesystem::exists(MaterialPath, ec);
	if (!ec && exists)
	{
		const assets::ISerializer<SMaterial>* reader = Serializers.PickReader(MaterialPath);
		if (reader)
		{
			(void)reader->Read(MaterialPath, material);

			// If the file was loaded via a non-default format (e.g. legacy v1),
			// upgrade by re-saving in the default write format.
			if (reader->GetFormat() != Serializers.GetDefaultWriteFormat())
			{
				WriteMaterial(MaterialPath, material);
			}
		}
	}
	else if (bCreateIfMissing)
	{
		WriteMaterial(MaterialPath, material);
	}

	if (material.Name.empty())
	{
		material.Name = MaterialPath.stem().string();
	}

	material.LastWriteTime = std::filesystem::last_write_time(MaterialPath, ec);
	UpdateDerivedState(material);
	return memory::NewShared<SMaterial>(std::move(material));
}

bool CMaterialLibrary::ReadMaterial (const std::filesystem::path& Path, SMaterial& Out) const
{
	const assets::ISerializer<SMaterial>* reader = Serializers.PickReader(Path);
	if (!reader)
	{
		return false;
	}
	return reader->Read(Path, Out);
}

bool CMaterialLibrary::WriteMaterial (const std::filesystem::path& Path, const SMaterial& In) const
{
	const assets::ISerializer<SMaterial>* writer = Serializers.PickWriter();
	if (!writer)
	{
		return false;
	}
	return writer->Write(Path, In);
}

void CMaterialLibrary::UpdateDerivedState (SMaterial& Material) const
{
	const bool bWantsBaseColorTexture = !Material.BaseColorTexturePath.empty();
	Material.Flags << (EMaterialFlags::UseBaseColorTexture + bWantsBaseColorTexture);
	// Raster preview blends what the path tracer treats stochastically.
	Material.bAlphaBlend = Material.bAlphaBlend || Material.AlphaMode == EAlphaMode::Blend;
	++Material.Version;
}

std::string CMaterialLibrary::MakeKey (const std::filesystem::path& MaterialPath)
{
	return MaterialPath.lexically_normal().string();
}
}


namespace frt::graphics
{
std::filesystem::path SMaterial::ResolveTexturePath (const std::string& RelativePath) const
{
	if (RelativePath.empty())
	{
		return {};
	}
	std::filesystem::path path = RelativePath;
	if (path.is_relative() && !SourcePath.empty())
	{
		path = SourcePath.parent_path() / path;
	}
	return path.lexically_normal();
}

bool SMaterial::IsEmissive () const
{
	return EmissiveIntensity > 0.0f && (Emissive.R > 0.0f || Emissive.G > 0.0f || Emissive.B > 0.0f);
}
}
