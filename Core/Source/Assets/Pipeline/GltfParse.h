#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


// cgltf's document type; cgltf.h itself stays inside Core's .cpp files (Assets/Pipeline/Cgltf.cpp compiles it).
struct cgltf_data;


/**
 * glTF parsing for the importers' Analyze (design §7.11, §5.5): cgltf over bytes FRT read itself, never a file name.
 *
 * cgltf is handed only JSON bytes, as cgltf_file_type_gltf: a .gltf file's content, or a .glb's JSON chunk, which
 * ReadGlbJsonChunk reads alone (the 12-byte header, the first chunk's 8-byte header, then that chunk) - Analyze needs
 * the document's structure, and a 500 MB GLB must not be read in full for it. cgltf_parse_file (narrow file names,
 * FILE*) and cgltf_load_buffers (reads every buffer) are never called: buffer and image URIs stay as written for the
 * caller to resolve (their bytes are not loaded, so accessor data is not available - counts and sizes are).
 *
 * Safe on worker threads: cgltf allocates with malloc; nothing here touches the engine's memory pool.
 */
namespace frt::assets
{
// Frees a document with cgltf_free.
struct SGltfDataDeleter
{
	FRT_CORE_API void operator() (cgltf_data* Data) const noexcept;
};
using TGltfDataPtr = std::unique_ptr<cgltf_data, SGltfDataDeleter>;

// The GLB container (glTF 2.0, "Binary glTF Layout"): a header of magic "glTF", version 2 and the total length (three
// little-endian uint32), then chunks of (uint32 length, uint32 type, data); the first chunk is the JSON.
inline constexpr uint32 GlbHeaderBytes = 12u;
inline constexpr uint32 GlbChunkHeaderBytes = 8u;
inline constexpr uint32 GlbMagic = 0x46546C67u;          // "glTF"
inline constexpr uint32 GlbJsonChunkType = 0x4E4F534Au;  // "JSON"
inline constexpr uint32 GlbBinChunkType = 0x004E4942u;   // "BIN\0"
// A JSON chunk past this is refused rather than read (a damaged or hostile length field).
inline constexpr uint32 MaxGlbJsonChunkBytes = 256u * 1024u * 1024u;

// Where a GLB's JSON chunk lies in the file.
struct SGlbJsonChunk
{
	uint64 Offset = 0u;   // always GlbHeaderBytes + GlbChunkHeaderBytes
	uint32 Length = 0u;
};

/**
 * Reads the GLB header and the first chunk's header from the file's first GlbHeaderBytes + GlbChunkHeaderBytes bytes.
 * FileSize is the file's real size. nullopt (with the reason) unless the magic, version 2, a total length that fits
 * the file and a first chunk of type JSON that fits the total length are all there.
 */
[[nodiscard]] FRT_CORE_API std::optional<SGlbJsonChunk> ParseGlbHeader (std::span<const uint8> Headers, uint64 FileSize, FString* OutError = nullptr);

// Reads only the JSON chunk of the GLB at Path through Platform/ (headers, then that chunk; never the BIN chunk).
[[nodiscard]] FRT_CORE_API std::optional<std::vector<uint8>> ReadGlbJsonChunk (const std::filesystem::path& Path, FString* OutError = nullptr);

/**
 * Parses glTF JSON - a .gltf file's bytes, or the chunk ReadGlbJsonChunk returned - with cgltf_parse as
 * cgltf_file_type_gltf. nullptr with the reason when cgltf rejects it. The document refers to no file.
 */
[[nodiscard]] FRT_CORE_API TGltfDataPtr ParseGltfJson (std::span<const uint8> Json, FString* OutError = nullptr);

// Object counts of a parsed document, as cgltf found them (glTF-native: no post-processing).
struct SGltfObjectCounts
{
	uint32 Meshes = 0u;
	uint32 Primitives = 0u;   // over every mesh
	uint32 Materials = 0u;
	uint32 Images = 0u;
	uint32 Textures = 0u;
	uint32 Buffers = 0u;
	uint32 BufferViews = 0u;
	uint32 Accessors = 0u;
	uint32 Nodes = 0u;
	uint32 Scenes = 0u;
	uint32 Animations = 0u;
	uint32 Skins = 0u;
};

[[nodiscard]] FRT_CORE_API SGltfObjectCounts CountGltfObjects (const cgltf_data& Data) noexcept;
}
