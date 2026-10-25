#include "Assets/Pipeline/GltfParse.h"

#include <cgltf.h>

#include "Platform/BlockingFileReader.h"


namespace frt::assets
{
namespace
{
[[nodiscard]] uint32 ReadU32 (std::span<const uint8> Bytes, size_t Offset) noexcept
{
	return static_cast<uint32>(Bytes[Offset]) | (static_cast<uint32>(Bytes[Offset + 1u]) << 8u)
		| (static_cast<uint32>(Bytes[Offset + 2u]) << 16u) | (static_cast<uint32>(Bytes[Offset + 3u]) << 24u);
}

void SetError (FString* OutError, FStringView Message)
{
	if (OutError != nullptr)
	{
		*OutError = FString(Message);
	}
}

[[nodiscard]] FStringLiteral DescribeResult (cgltf_result Result) noexcept
{
	switch (Result)
	{
	case cgltf_result_success:         return "success";
	case cgltf_result_data_too_short:  return "the data ends too early";
	case cgltf_result_unknown_format:  return "not glTF";
	case cgltf_result_invalid_json:    return "invalid JSON";
	case cgltf_result_invalid_gltf:    return "invalid glTF";
	case cgltf_result_invalid_options: return "invalid options";
	case cgltf_result_file_not_found:  return "a file was not found";
	case cgltf_result_io_error:        return "an I/O error";
	case cgltf_result_out_of_memory:   return "out of memory";
	case cgltf_result_legacy_gltf:     return "glTF 1.0, which is not supported";
	default:                           return "an unknown error";
	}
}
}


void SGltfDataDeleter::operator() (cgltf_data* Data) const noexcept
{
	cgltf_free(Data);
}

std::optional<SGlbJsonChunk> ParseGlbHeader (std::span<const uint8> Headers, uint64 FileSize, FString* OutError)
{
	if (Headers.size() < GlbHeaderBytes + GlbChunkHeaderBytes || FileSize < GlbHeaderBytes + GlbChunkHeaderBytes)
	{
		SetError(OutError, "shorter than a GLB header");
		return std::nullopt;
	}
	if (ReadU32(Headers, 0u) != GlbMagic)
	{
		SetError(OutError, "not a GLB file (no \"glTF\" magic)");
		return std::nullopt;
	}
	const uint32 version = ReadU32(Headers, 4u);
	if (version != 2u)
	{
		SetError(OutError, FString::Format("GLB version {} is not 2", version));
		return std::nullopt;
	}
	const uint32 totalLength = ReadU32(Headers, 8u);
	if (totalLength > FileSize || totalLength < GlbHeaderBytes + GlbChunkHeaderBytes)
	{
		SetError(OutError, FString::Format("the GLB header's length {} does not fit the file's {} bytes", totalLength, FileSize));
		return std::nullopt;
	}
	const uint32 chunkLength = ReadU32(Headers, GlbHeaderBytes);
	const uint32 chunkType = ReadU32(Headers, GlbHeaderBytes + 4u);
	if (chunkType != GlbJsonChunkType)
	{
		SetError(OutError, "the GLB's first chunk is not JSON");
		return std::nullopt;
	}
	if (static_cast<uint64>(chunkLength) > totalLength - GlbHeaderBytes - GlbChunkHeaderBytes)
	{
		SetError(OutError, FString::Format("the GLB's JSON chunk ({} bytes) runs past the file's end", chunkLength));
		return std::nullopt;
	}
	if (chunkLength > MaxGlbJsonChunkBytes)
	{
		SetError(OutError, FString::Format("the GLB's JSON chunk ({} bytes) is larger than {} bytes", chunkLength, MaxGlbJsonChunkBytes));
		return std::nullopt;
	}
	SGlbJsonChunk chunk;
	chunk.Offset = GlbHeaderBytes + GlbChunkHeaderBytes;
	chunk.Length = chunkLength;
	return chunk;
}

std::optional<std::vector<uint8>> ReadGlbJsonChunk (const std::filesystem::path& Path, FString* OutError)
{
	platform::CBlockingFileReader reader;
	if (!reader.Open(Path, OutError))
	{
		return std::nullopt;
	}
	uint8 headers[GlbHeaderBytes + GlbChunkHeaderBytes] = {};
	if (reader.GetSize() < sizeof(headers))
	{
		SetError(OutError, "shorter than a GLB header");
		return std::nullopt;
	}
	if (!reader.ReadExactAt(0u, headers, OutError))
	{
		return std::nullopt;
	}
	const std::optional<SGlbJsonChunk> chunk = ParseGlbHeader(headers, reader.GetSize(), OutError);
	if (!chunk)
	{
		return std::nullopt;
	}
	std::vector<uint8> json(chunk->Length);
	if (!json.empty() && !reader.ReadExactAt(chunk->Offset, json, OutError))
	{
		return std::nullopt;
	}
	return json;
}

TGltfDataPtr ParseGltfJson (std::span<const uint8> Json, FString* OutError)
{
	cgltf_options options = {};
	options.type = cgltf_file_type_gltf;
	cgltf_data* data = nullptr;
	const cgltf_result result = cgltf_parse(&options, Json.data(), Json.size(), &data);
	TGltfDataPtr document(data);
	if (result != cgltf_result_success)
	{
		SetError(OutError, FString::Format("cgltf: {}", DescribeResult(result)));
		return nullptr;
	}
	return document;
}

SGltfObjectCounts CountGltfObjects (const cgltf_data& Data) noexcept
{
	SGltfObjectCounts counts;
	counts.Meshes = static_cast<uint32>(Data.meshes_count);
	for (cgltf_size i = 0u; i < Data.meshes_count; ++i)
	{
		counts.Primitives += static_cast<uint32>(Data.meshes[i].primitives_count);
	}
	counts.Materials = static_cast<uint32>(Data.materials_count);
	counts.Images = static_cast<uint32>(Data.images_count);
	counts.Textures = static_cast<uint32>(Data.textures_count);
	counts.Buffers = static_cast<uint32>(Data.buffers_count);
	counts.BufferViews = static_cast<uint32>(Data.buffer_views_count);
	counts.Accessors = static_cast<uint32>(Data.accessors_count);
	counts.Nodes = static_cast<uint32>(Data.nodes_count);
	counts.Scenes = static_cast<uint32>(Data.scenes_count);
	counts.Animations = static_cast<uint32>(Data.animations_count);
	counts.Skins = static_cast<uint32>(Data.skins_count);
	return counts;
}
}
