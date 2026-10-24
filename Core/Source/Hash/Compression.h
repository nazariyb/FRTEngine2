#pragma once

#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"


/**
 * zstd for cached and cooked bytes (vendored, ThirdParty/zstd; seen only by Compression.cpp).
 *
 * One frame per call, single-threaded, so the output is a pure function of the input, the level and
 * the zstd version: two machines compressing the same value write the same bytes, which content-
 * addressed stores and byte-identical cooks rely on. The frame records the content size; no frame
 * checksum, because every caller already holds a hash of the raw bytes and verifies that instead.
 *
 * std::vector (CRT heap) rather than TArray: these run on worker threads and before the memory pool
 * exists. Thread-safe.
 */
namespace frt::compression
{
// zstd's own default level, and the one the derived-data cache stores its values with.
inline constexpr int32 DefaultZstdLevel = 3;

// One zstd frame holding Bytes (an empty span gives a valid frame). Level as zstd takes it: 1 to
// ZSTD_maxCLevel() (22), negative levels trade ratio for speed, 0 is the default; out-of-range levels
// are clamped. Empty only if zstd failed (it allocates its context), which a valid frame never is.
[[nodiscard]] FRT_CORE_API std::vector<uint8> CompressZstd (std::span<const uint8> Bytes, int32 Level = DefaultZstdLevel);

// The bytes of exactly one frame, as CompressZstd writes it, which must decode to exactly ExpectedSize
// bytes. nullopt for anything else: not zstd, corrupt or truncated, trailing bytes, a frame whose
// recorded size differs. ExpectedSize comes from the caller's own record, never from the frame, so a
// damaged header cannot make it allocate more.
[[nodiscard]] FRT_CORE_API std::optional<std::vector<uint8>> DecompressZstd (std::span<const uint8> Frame, size_t ExpectedSize);
}
