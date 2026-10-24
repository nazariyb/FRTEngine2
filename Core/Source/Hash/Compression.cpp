#include "Hash/Compression.h"

#include <zstd.h>


namespace frt::compression
{
std::vector<uint8> CompressZstd (std::span<const uint8> Bytes, int32 Level)
{
	std::vector<uint8> frame(ZSTD_compressBound(Bytes.size()));
	const size_t written = ZSTD_compress(frame.data(), frame.size(), Bytes.data(), Bytes.size(), Level);
	if (ZSTD_isError(written))
	{
		return {};
	}
	frame.resize(written);
	return frame;
}

std::optional<std::vector<uint8>> DecompressZstd (std::span<const uint8> Frame, size_t ExpectedSize)
{
	// One whole frame and nothing after it: ZSTD_decompress alone would also accept several.
	const size_t frameSize = ZSTD_findFrameCompressedSize(Frame.data(), Frame.size());
	if (ZSTD_isError(frameSize) || frameSize != Frame.size())
	{
		return std::nullopt;
	}

	// A frame that records its size must record this one; refused before anything is allocated.
	const unsigned long long recorded = ZSTD_getFrameContentSize(Frame.data(), Frame.size());
	if (recorded == ZSTD_CONTENTSIZE_ERROR || (recorded != ZSTD_CONTENTSIZE_UNKNOWN && recorded != ExpectedSize))
	{
		return std::nullopt;
	}

	// A frame that decodes to more than ExpectedSize fails with dstSize_tooSmall.
	std::vector<uint8> bytes(ExpectedSize);
	const size_t written = ZSTD_decompress(bytes.data(), bytes.size(), Frame.data(), Frame.size());
	if (ZSTD_isError(written) || written != ExpectedSize)
	{
		return std::nullopt;
	}
	return bytes;
}
}
