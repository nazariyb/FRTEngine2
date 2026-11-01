#include "Platform/Platform.h"

#include <vector>


/**
 * DecodeTextFile (Platform.h): what the engine makes of a text file's stored bytes. A file of its own
 * because builders compile what it returns: the shader compiler hashes a source's bytes as stored and
 * hands DXC this decoding of them, so a change here changes DXIL under an unchanged key. It is one of
 * the Shader.Dxil builder's sources (Assets/BuilderVersions.inl): editing it changes the builder's
 * source hash, and the pipeline lint checks it. No OS calls: the bytes are already read.
 */
namespace frt::platform
{
namespace
{
// The bytes after a UTF-16 BOM, as canonical WTF-8. nullopt for an odd byte count: not UTF-16 at
// all, or cut off.
std::optional<FString> DecodeUtf16File (FStringView Bytes, bool bBigEndian)
{
	if (Bytes.Len() % 2u != 0u)
	{
		return std::nullopt;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(Bytes.GetData());
	std::vector<wchar_t> units(Bytes.Len() / 2u);
	for (size_t i = 0u; i < units.size(); ++i)
	{
		const uint32 first = bytes[2u * i];
		const uint32 second = bytes[2u * i + 1u];
		units[i] = static_cast<wchar_t>(bBigEndian ? (first << 8u) | second : (second << 8u) | first);
	}
	return FString::FromWide(std::wstring_view(units.data(), units.size()));
}
}


std::optional<FString> DecodeTextFile (FString Bytes)
{
	if (Bytes.RemoveFromStart(Utf8Bom))
	{
		return Bytes;
	}
	const FStringView bytes = Bytes.View();
	if (bytes.StartsWith("\xFF\xFE"))
	{
		return DecodeUtf16File(bytes.RightChop(2u), false);
	}
	if (bytes.StartsWith("\xFE\xFF"))
	{
		return DecodeUtf16File(bytes.RightChop(2u), true);
	}
	return Bytes;
}
}
