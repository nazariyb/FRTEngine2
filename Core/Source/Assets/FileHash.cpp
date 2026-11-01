#include "Assets/FileHash.h"

#include <span>

#include "Platform/FileSystem.h"


namespace frt::assets
{
std::optional<hash::SContentHash> HashFileStable (const std::filesystem::path& Path)
{
	hash::CContentHasher hasher;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&hasher] (std::span<const uint8> Bytes) { hasher.Update(Bytes); });
	if (read != platform::EStableRead::Stable)
	{
		return std::nullopt;
	}
	return hasher.Finalize();
}
}
