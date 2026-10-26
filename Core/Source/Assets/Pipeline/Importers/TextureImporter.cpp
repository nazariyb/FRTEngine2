#include "Assets/Pipeline/Importers/ImporterUtils.h"

#include <algorithm>
#include <climits>
#include <span>
#include <vector>

// The implementation is compiled once, in Graphics/Model.cpp: this file sees only the declarations.
#include <stb_image.h>

#include "Platform/BlockingFileReader.h"
#include "Platform/Platform.h"


namespace frt::assets::importers
{
namespace
{
// Enough for the header of every format stb reads: PNG's IHDR, TGA's and BMP's fixed headers, and JPEG's SOF
// after the usual APP segments.
constexpr uint32 HeaderPrefixBytes = 64u * 1024u;
// Only two formats can keep what stb needs past the prefix: a JPEG's SOF after large APP segments (an EXIF
// thumbnail, an ICC profile) and a paletted PNG's PLTE / tRNS after large ancillary chunks. Such a file is read
// further in growing steps (x4 from 256 KiB) up to this cap; anything else - or a file whose signature is neither
// - fails from the prefix, so a large misnamed or corrupt file costs a worker 64 KiB, not the whole file.
constexpr uint64 FirstLongerReadBytes = 256u * 1024u;
constexpr uint64 MaxHeaderReadBytes = 16u * 1024u * 1024u;

bool MayNeedMoreThanThePrefix (std::span<const uint8> Prefix) noexcept
{
	static constexpr uint8 PngSignature[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	static constexpr uint8 JpegStart[] = { 0xFF, 0xD8, 0xFF };
	const auto startsWith = [&] (std::span<const uint8> Signature)
	{
		return Prefix.size() >= Signature.size() && std::equal(Signature.begin(), Signature.end(), Prefix.begin());
	};
	return startsWith(PngSignature) || startsWith(JpegStart);
}

struct SImageInfo
{
	int Width = 0;
	int Height = 0;
	int Channels = 0;
};

std::optional<SImageInfo> GetImageInfo (std::span<const uint8> Bytes) noexcept
{
	if (Bytes.empty() || Bytes.size() > static_cast<size_t>(INT_MAX))
	{
		return std::nullopt;
	}
	SImageInfo info;
	if (stbi_info_from_memory(Bytes.data(), static_cast<int>(Bytes.size()), &info.Width, &info.Height, &info.Channels) == 0)
	{
		return std::nullopt;
	}
	return info;
}

/**
 * Texture (§7.11, Analyze in M3): the image's header facts - Width, Height, Channels as stored in the file (1
 * grey, 2 grey + alpha, 3 RGB, 4 RGBA, as stb_image counts them: a palette with transparency counts 4) and
 * HasAlpha (2 or 4 channels). Reads a header prefix through Platform/ (a JPEG or PNG further, up to 16 MiB, when
 * its header lies past it) and hands stb bytes only, never a file name; decodes and hashes nothing (MeanColor is
 * AfterBuild, M8).
 */
class CTextureImporter final : public IAssetImporter
{
public:
	const SImporterDesc& GetInfo () const noexcept override
	{
		static const SImporterDesc* const desc = FindImporter("Texture");
		return *desc;
	}

	bool Analyze (const SAnalyzeInput& In, const SResolvedSettings& /*Settings*/, SAnalyzeResult& Out) const override
	{
		platform::CBlockingFileReader reader;
		FString error;
		if (!reader.Open(In.Path, &error))
		{
			Out.bSourceChanged = reader.GetLastFailure() == platform::EFileFailure::InUse;
			Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("cannot open the image: {}", error)));
			return false;
		}
		const uint64 size = reader.GetSize();
		std::vector<uint8> bytes;
		// Reads the file's bytes up to Length (or its end) into `bytes`; false on a read failure, reported.
		const auto readUpTo = [&] (uint64 Length)
		{
			const size_t had = bytes.size();
			bytes.resize(static_cast<size_t>(std::min(Length, size)));
			const std::optional<uint64> read = had < bytes.size()
				? reader.ReadAt(had, std::span<uint8>(bytes).subspan(had), &error) : std::optional<uint64>(0u);
			if (!read)
			{
				Out.bSourceChanged = reader.GetLastFailure() == platform::EFileFailure::InUse;
				Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("cannot read the image: {}", error)));
				return false;
			}
			bytes.resize(had + static_cast<size_t>(*read));
			return true;
		};
		if (!readUpTo(HeaderPrefixBytes))
		{
			return false;
		}
		std::optional<SImageInfo> info = GetImageInfo(bytes);
		if (!info && MayNeedMoreThanThePrefix(bytes))
		{
			for (uint64 length = FirstLongerReadBytes; !info && bytes.size() < size && length <= MaxHeaderReadBytes; length *= 4u)
			{
				const size_t had = bytes.size();
				if (!readUpTo(length))
				{
					return false;
				}
				if (bytes.size() == had)
				{
					break;   // the file ended early (it is shrinking)
				}
				info = GetImageInfo(bytes);
			}
			if (!info && bytes.size() < size)
			{
				Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format(
					"stb_image found no image header in the first {} of the image's {} bytes (it reads at most {} MiB for one)",
					bytes.size(), size, MaxHeaderReadBytes / (1024u * 1024u))));
				return false;
			}
		}
		reader.Close();
		if (!info && size == 0u)
		{
			Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, "the image file is empty"));
			return false;
		}
		if (!info)
		{
			const char* reason = stbi_failure_reason();
			Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("stb_image cannot read the image's header: {}",
				reason != nullptr ? FStringView(reason) : FStringView("unknown format"))));
			return false;
		}

		Out.Tags.push_back({ "Width", STagValue::FromInt64(info->Width) });
		Out.Tags.push_back({ "Height", STagValue::FromInt64(info->Height) });
		Out.Tags.push_back({ "Channels", STagValue::FromInt64(info->Channels) });
		Out.Tags.push_back({ "HasAlpha", STagValue::FromBool(info->Channels == 2 || info->Channels == 4) });
		return true;
	}
};
}

const IAssetImporter& GetTextureImporter () noexcept
{
	static const CTextureImporter importer;
	return importer;
}
}
