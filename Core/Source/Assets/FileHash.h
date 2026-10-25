#pragma once

#include <filesystem>
#include <optional>

#include "Core.h"
#include "Hash/StableHash.h"
#include "Platform/Platform.h"


/**
 * The content hash of a source file, taken only when nothing changed while it was read (design §5.4
 * "Hashing is race-free"). One function for every asset-system caller that hashes a whole file - the
 * watcher's self-write check, the registry's lazy hashes (pairing, a build that is ensured) - so they
 * agree on the bytes and the algorithm: BLAKE3-160 (hash::CContentHasher) over
 * platform::ReadFileChunksStable, which stats before and after and fails on any difference. The builder
 * sources (ShaderCompiler, ShaderCache) keep their own reads: their bytes are versioned by the builder
 * guard, which sees only their own files.
 *
 * Safe on worker threads (std containers and the CRT heap only). It reads the file, so it is never
 * called during a stat scan (§5.4: unchanged files are never opened).
 */
namespace frt::assets
{
// nullopt unless the read was platform::EStableRead::Stable: a file that changed, vanished or was held by
// a writer during the read, or that could not be read at all. The caller retries after the settle window.
[[nodiscard]] FRT_CORE_API std::optional<hash::SContentHash> HashFileStable (const std::filesystem::path& Path);

template <platform::NarrowText T>
std::optional<hash::SContentHash> HashFileStable (const T&) = delete;
}
