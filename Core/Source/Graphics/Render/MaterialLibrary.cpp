#include "MaterialLibrary.h"

#include "Material.h"

#include "Assets/FileWatcherService.h"
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

CMaterialLibrary::~CMaterialLibrary ()
{
	Unwatch();
}

memory::TRefShared<SMaterial> CMaterialLibrary::LoadOrCreateMaterial (
	const std::filesystem::path& MaterialPath,
	const SMaterial& DefaultMaterial)
{
	const FString key = MakeKey(MaterialPath);
	auto it = Materials.find(key);
	if (it != Materials.end())
	{
		return it->second.Material;
	}

	SMaterialRecord record;
	record.Material = LoadMaterialFromFile(MaterialPath, DefaultMaterial, true);
	Materials.emplace(key, record);
	Watch(MaterialPath);
	return record.Material;
}

void CMaterialLibrary::SetFileWatcher (assets::CFileWatcherService* Watcher)
{
	if (Watcher == FileWatcher)
	{
		return;
	}
	Unwatch();
	FileWatcher = Watcher;
	for (const auto& entry : Materials)
	{
		if (entry.second.Material && !entry.second.Material->SourcePath.empty())
		{
			Watch(entry.second.Material->SourcePath);
		}
	}
}

void CMaterialLibrary::Watch (const std::filesystem::path& MaterialPath)
{
	if (!FileWatcher)
	{
		return;
	}
	const std::filesystem::path directory = MaterialPath.parent_path();
	const FString key = MakeKey(directory);
	if (WatchedDirectories.contains(key))
	{
		return;
	}
	assets::SWatchFilter filter;
	filter.Extensions.push_back(".frtmat.yml");
	const SDelegateHandle handle = FileWatcher->Subscribe(directory, filter,
		[this] (std::span<const assets::SFileChange> Changes) { OnFilesChanged(Changes); });
	WatchedDirectories.try_emplace(key, handle);
}

void CMaterialLibrary::Unwatch ()
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

void CMaterialLibrary::OnFilesChanged (std::span<const assets::SFileChange> Changes)
{
	// Level-triggered: a change only says where to look. A material is read again when its file's
	// write time differs from the one it was read with, so the library's own writes (create, upgrade)
	// and repeated reports of one edit read nothing twice.
	for (auto& entry : Materials)
	{
		SMaterialRecord& record = entry.second;
		if (record.bChanged || !record.Material || record.Material->SourcePath.empty())
		{
			continue;
		}
		const std::filesystem::path& path = record.Material->SourcePath;
		for (const assets::SFileChange& change : Changes)
		{
			if (!assets::IsFileAffected(change, path))
			{
				continue;
			}
			std::error_code ec;
			const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
			if (!ec && writeTime != record.Material->LastWriteTime)
			{
				record.bChanged = true;
				bAnyChanged = true;
			}
			break;
		}
	}
}

bool CMaterialLibrary::ReloadModifiedMaterials ()
{
	if (!bAnyChanged)
	{
		return false;
	}
	bAnyChanged = false;

	bool anyReloaded = false;
	for (auto& entry : Materials)
	{
		SMaterialRecord& record = entry.second;
		if (!record.bChanged)
		{
			continue;
		}
		record.bChanged = false;

		// The write time is taken before the content: a write that lands while the file is read
		// leaves a newer time behind, and its own report reads it again.
		SMaterial& material = *record.Material;
		std::error_code ec;
		const std::filesystem::file_time_type newWriteTime = std::filesystem::last_write_time(material.SourcePath, ec);
		if (ec)
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

void CMaterialLibrary::ForEachMaterial (TFunctionRef<void (const memory::TRefShared<SMaterial>&)> Visit) const
{
	for (const auto& entry : Materials)
	{
		if (entry.second.Material)
		{
			Visit(entry.second.Material);
		}
	}
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

	if (material.Name.IsEmpty())
	{
		material.Name = FString::FromPath(MaterialPath.stem());
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
	const bool bWantsBaseColorTexture = !Material.BaseColorTexturePath.IsEmpty();
	Material.Flags << (EMaterialFlags::UseBaseColorTexture + bWantsBaseColorTexture);
	// Raster preview blends what the path tracer treats stochastically.
	Material.bAlphaBlend = Material.bAlphaBlend || Material.AlphaMode == EAlphaMode::Blend;
	++Material.Version;
}

FString CMaterialLibrary::MakeKey (const std::filesystem::path& MaterialPath)
{
	// A TPathMap key: case-insensitive as NTFS is, so "Brick.frtmat.yml" and "brick.frtmat.yml"
	// share one material.
	return FString::FromPath(MaterialPath.lexically_normal(), EPathStyle::Generic);
}
}


namespace frt::graphics
{
std::filesystem::path SMaterial::ResolveTexturePath (FStringView RelativePath) const
{
	if (RelativePath.IsEmpty())
	{
		return {};
	}
	std::filesystem::path path = RelativePath.ToPath();
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
