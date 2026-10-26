#include "Assets/Pipeline/Importers/ImporterUtils.h"

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
// after the usual APP segments (EXIF thumbnails can push it further: then the whole file is read).
constexpr uint32 HeaderPrefixBytes = 64u * 1024u;
// A file stb cannot describe from the prefix is read whole once, unless it is larger than this (stb takes an int
// length, and a texture source this large is not worth reading to learn its header).
constexpr uint64 MaxWholeReadBytes = 512u * 1024u * 1024u;

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
 * HasAlpha (2 or 4 channels). Reads a header prefix through Platform/ and hands stb bytes only, never a file name;
 * decodes and hashes nothing (MeanColor is AfterBuild, M8).
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
		std::vector<uint8> prefix(static_cast<size_t>(size < HeaderPrefixBytes ? size : HeaderPrefixBytes));
		const std::optional<uint64> read = reader.ReadAt(0u, prefix, &error);
		if (!read)
		{
			Out.bSourceChanged = reader.GetLastFailure() == platform::EFileFailure::InUse;
			Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("cannot read the image: {}", error)));
			return false;
		}
		prefix.resize(static_cast<size_t>(*read));
		reader.Close();

		std::optional<SImageInfo> info = GetImageInfo(prefix);
		if (!info && size > prefix.size() && size <= MaxWholeReadBytes)
		{
			if (const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(In.Path))
			{
				info = GetImageInfo(*bytes);
			}
		}
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
