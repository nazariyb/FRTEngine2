#include "MaterialLibrary.h"

#include "Material.h"

#include <d3d12.h>

#include <stb_image.h>

#include "Graphics/Render/Renderer.h"
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

void CMaterialLibrary::SetRenderer (CRenderer* InRenderer)
{
	frt_assert(InRenderer);
	Renderer = InRenderer;
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
			EnsureBaseColorTexture(material);
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
	EnsureBaseColorTexture(material);
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

void CMaterialLibrary::EnsureBaseColorTexture (SMaterial& Material) const
{
	const bool bWantsBaseColorTexture = !Material.BaseColorTexturePath.empty();
	Material.Flags << (EMaterialFlags::UseBaseColorTexture + bWantsBaseColorTexture);

	if (!bWantsBaseColorTexture)
	{
		Material.bHasBaseColorTexture = false;
		Material.LoadedBaseColorTexturePath.clear();
		return;
	}

	if (!Renderer)
	{
		return;
	}

	std::filesystem::path texturePath = Material.BaseColorTexturePath;
	if (texturePath.is_relative() && !Material.SourcePath.empty())
	{
		texturePath = Material.SourcePath.parent_path() / texturePath;
	}
	texturePath = texturePath.lexically_normal();

	if (Material.bHasBaseColorTexture && Material.LoadedBaseColorTexturePath == texturePath)
	{
		return;
	}

	int32 channelNum = 0;
	int32 width = 0;
	int32 height = 0;
	uint32* texels = (uint32*)stbi_load(texturePath.string().c_str(), &width, &height, &channelNum, 4);
	if (!texels)
	{
		return;
	}

	STexture newTexture = {};
	newTexture.Width = width;
	newTexture.Height = height;
	newTexture.Texels = texels;

#if !defined(FRT_HEADLESS)
	{
		D3D12_RESOURCE_DESC Desc = {};
		Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		Desc.Width = newTexture.Width;
		Desc.Height = newTexture.Height;
		Desc.DepthOrArraySize = 1;
		Desc.MipLevels = 1;
		Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		Desc.SampleDesc.Count = 1;
		Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		newTexture.GpuTexture = Renderer->CreateTextureAsset(Desc);
		Renderer->EnqueueTextureUpload(
			newTexture.GpuTexture, Desc, newTexture.Texels,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = Desc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.PlaneSlice = 0;

		D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
		Renderer->CreateShaderResourceView(newTexture.GpuTexture, srvDesc, &cpuDescriptor, &newTexture.GpuDescriptor);
	}
#endif

	Material.BaseColorTexture = newTexture;
	Material.bHasBaseColorTexture = true;
	Material.LoadedBaseColorTexturePath = texturePath;
}

std::string CMaterialLibrary::MakeKey (const std::filesystem::path& MaterialPath)
{
	return MaterialPath.lexically_normal().string();
}
}
