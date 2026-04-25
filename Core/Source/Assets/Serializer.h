#pragma once

#include <filesystem>
#include <memory>

#include "Core.h"
#include "Containers/Array.h"
#include "Assets/AssetFormat.h"


namespace frt::assets
{
// Per-asset-type serializer interface. One concrete impl per (asset type, format) pair.
// Most impls will internally use IReadArchive / IWriteArchive + free Serialize() overloads,
// but legacy or specialized formats can implement Read/Write directly.
template <typename TAsset>
class ISerializer
{
public:
	virtual ~ISerializer () = default;

	virtual EAssetFormat GetFormat () const = 0;

	// Lightweight content sniff: does this serializer recognize the file at Path?
	// Used by the registry to dispatch to the right backend on load.
	virtual bool CanRead (const std::filesystem::path& Path) const = 0;

	virtual bool Read (const std::filesystem::path& Path, TAsset& Out) const = 0;
	virtual bool Write (const std::filesystem::path& Path, const TAsset& In) const = 0;
};

// Registry holds all serializers for a given asset type.
// On read: tries serializers in registration order, picks the first whose CanRead matches.
// On write: returns the serializer for the configured default write format.
template <typename TAsset>
class CSerializerRegistry
{
public:
	using SerializerPtr = std::unique_ptr<ISerializer<TAsset>>;

	CSerializerRegistry () = default;
	CSerializerRegistry (const CSerializerRegistry&) = delete;
	CSerializerRegistry& operator= (const CSerializerRegistry&) = delete;
	CSerializerRegistry (CSerializerRegistry&&) noexcept = default;
	CSerializerRegistry& operator= (CSerializerRegistry&&) noexcept = default;

	void Register (SerializerPtr Serializer)
	{
		if (Serializer)
		{
			Serializers.Add(std::move(Serializer));
		}
	}

	void SetDefaultWriteFormat (EAssetFormat Format) { DefaultWriteFormat = Format; }
	EAssetFormat GetDefaultWriteFormat () const { return DefaultWriteFormat; }

	const ISerializer<TAsset>* PickReader (const std::filesystem::path& Path) const
	{
		for (uint32 i = 0; i < Serializers.Count(); ++i)
		{
			const ISerializer<TAsset>* serializer = Serializers[i].get();
			if (serializer && serializer->CanRead(Path))
			{
				return serializer;
			}
		}
		return nullptr;
	}

	const ISerializer<TAsset>* PickWriter () const
	{
		return Find(DefaultWriteFormat);
	}

	const ISerializer<TAsset>* Find (EAssetFormat Format) const
	{
		for (uint32 i = 0; i < Serializers.Count(); ++i)
		{
			const ISerializer<TAsset>* serializer = Serializers[i].get();
			if (serializer && serializer->GetFormat() == Format)
			{
				return serializer;
			}
		}
		return nullptr;
	}

private:
	TArray<SerializerPtr> Serializers;
	EAssetFormat DefaultWriteFormat = EAssetFormat::Yaml;
};
}
