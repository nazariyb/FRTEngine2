// The zstd wrapper (Hash/Compression.h): every frame decodes to exactly the bytes it was made from,
// compression is a pure function of input and level (content-addressed stores and byte-identical cooks
// depend on it) and frozen frames pin it across builds and zstd updates, and anything but one intact
// frame of the expected size is refused rather than returned as wrong bytes.
#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <vector>

#include "Containers/String.h"
#include "Hash/Compression.h"
#include "Hash/StableHash.h"

using namespace frt;


namespace
{
// Text-like data that compresses well: a few repeating sentences with a counter.
std::vector<uint8> MakeCompressible (size_t Length)
{
	constexpr char sentence[] = "The quick brown fox jumps over the lazy dog. Привіт, світе! ";
	std::vector<uint8> bytes(Length);
	for (size_t i = 0u; i < Length; ++i)
	{
		bytes[i] = static_cast<uint8>(sentence[i % (sizeof(sentence) - 1u)] + (i / 4096u) % 3u);
	}
	return bytes;
}

// Pseudo-random words from a small vocabulary: matches at many offsets and lengths and entropy-coded
// literals, the paths a real asset's frame takes, where MakeCompressible is mostly long repeats.
std::vector<uint8> MakeWords (size_t Length)
{
	constexpr char words[][16] = { "mesh", "texture", "material", "shader", "scene", "light", "camera", "sound",
		"font", "level", "actor", "prefab", "Ґанок", "світло", "0.25", "1024" };
	std::vector<uint8> bytes;
	bytes.reserve(Length + sizeof(words[0]));
	uint64 state = 0x2545F4914F6CDD1Dull;
	while (bytes.size() < Length)
	{
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		for (const char* c = words[state >> 60u]; *c != 0; ++c)
		{
			bytes.push_back(static_cast<uint8>(*c));
		}
		bytes.push_back((state >> 40u) % 7u == 0u ? uint8 { 0x0A } : uint8 { 0x20 });   // newline or space
	}
	bytes.resize(Length);
	return bytes;
}

// Data zstd cannot shrink: a 64-bit LCG's high bytes.
std::vector<uint8> MakeIncompressible (size_t Length)
{
	std::vector<uint8> bytes(Length);
	uint64 state = 0x9E3779B97F4A7C15ull;
	for (uint8& byte : bytes)
	{
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		byte = static_cast<uint8>(state >> 56u);
	}
	return bytes;
}
}


TEST(CompressionTest, RoundTripRestoresTheBytes)
{
	for (const std::vector<uint8>& input : { MakeCompressible(1u << 20u), MakeIncompressible(100000u), MakeCompressible(1u) })
	{
		const std::vector<uint8> frame = compression::CompressZstd(input);
		ASSERT_FALSE(frame.empty());
		const std::optional<std::vector<uint8>> output = compression::DecompressZstd(frame, input.size());
		ASSERT_TRUE(output.has_value()) << "input of " << input.size() << " bytes";
		EXPECT_EQ(*output, input);
	}
}

TEST(CompressionTest, CompressibleDataShrinks)
{
	const std::vector<uint8> input = MakeCompressible(1u << 20u);
	EXPECT_LT(compression::CompressZstd(input).size(), input.size() / 20u);
	EXPECT_LT(compression::CompressZstd(input, 19).size(), compression::CompressZstd(input, 1).size());
}

TEST(CompressionTest, EmptyInputIsAValidFrame)
{
	const std::vector<uint8> frame = compression::CompressZstd({});
	EXPECT_FALSE(frame.empty());
	const std::optional<std::vector<uint8>> output = compression::DecompressZstd(frame, 0u);
	ASSERT_TRUE(output.has_value());
	EXPECT_TRUE(output->empty());
}

TEST(CompressionTest, OutputIsDeterministic)
{
	const std::vector<uint8> input = MakeCompressible(300000u);
	for (const int32 level : { -5, 1, compression::DefaultZstdLevel, 9, 19 })
	{
		EXPECT_EQ(compression::CompressZstd(input, level), compression::CompressZstd(input, level)) << "level " << level;
	}
	// Out-of-range levels are clamped, not refused.
	const std::vector<uint8> clamped = compression::CompressZstd(input, 1000);
	ASSERT_FALSE(clamped.empty());
	EXPECT_EQ(compression::DecompressZstd(clamped, input.size()), input);
}

// The frames themselves, not only "the same twice in one process": a build flag (ZSTD_MULTITHREAD,
// another default parameter) or a zstd update that changes the bytes changes every cooked and cached
// value, so it must fail here. Frozen once from zstd 1.5.7 (ThirdParty/zstd/VERSION.txt); a deliberate
// re-freeze is a cooked-output change.
TEST(CompressionTest, FramesAreFrozen)
{
	enum class EInput { Compressible, Words, Incompressible };
	struct SFrozenFrame
	{
		EInput Input;
		size_t InputSize;
		int32 Level;
		size_t FrameSize;
		FStringLiteral FrameHash;   // BLAKE3-160 of the frame
	};
	constexpr int32 defaultLevel = compression::DefaultZstdLevel;
	constexpr SFrozenFrame frozen[] =
	{
		{ EInput::Compressible, 300000u, defaultLevel, 516u, "822e30f803db34e1f8fcb64809cb03f18179da12" },
		{ EInput::Compressible, 300000u, 19, 414u, "f166cf76c9e7c7b5371b38a4740b33f5aa6bb2fc" },
		{ EInput::Words, 300000u, defaultLevel, 54517u, "9da431ff0268c9864d632be9826e91f6b40145e7" },
		{ EInput::Words, 300000u, 19, 38705u, "59c4592fd88897dbaa131c948f44053c7b776196" },
		{ EInput::Incompressible, 100000u, defaultLevel, 100012u, "1a6d447e958456b08185555b61c81fe1dd4ae5bd" },
	};
	for (const SFrozenFrame& expected : frozen)
	{
		const std::vector<uint8> input = expected.Input == EInput::Compressible ? MakeCompressible(expected.InputSize)
			: expected.Input == EInput::Words ? MakeWords(expected.InputSize) : MakeIncompressible(expected.InputSize);
		const std::vector<uint8> frame = compression::CompressZstd(input, expected.Level);
		EXPECT_EQ(frame.size(), expected.FrameSize) << input.size() << " bytes, level " << expected.Level;
		EXPECT_EQ(hash::HashContent(frame).ToString(), expected.FrameHash) << input.size() << " bytes, level "
			<< expected.Level;
		EXPECT_EQ(compression::DecompressZstd(frame, input.size()), input) << input.size() << " bytes, level "
			<< expected.Level;
	}
}

TEST(CompressionTest, WrongExpectedSizeIsRefused)
{
	const std::vector<uint8> input = MakeCompressible(5000u);
	const std::vector<uint8> frame = compression::CompressZstd(input);
	EXPECT_FALSE(compression::DecompressZstd(frame, input.size() - 1u));
	EXPECT_FALSE(compression::DecompressZstd(frame, input.size() + 1u));
	EXPECT_FALSE(compression::DecompressZstd(frame, 0u));
}

TEST(CompressionTest, DamagedFramesAreRefused)
{
	const std::vector<uint8> input = MakeIncompressible(5000u);
	const std::vector<uint8> frame = compression::CompressZstd(input);
	const std::span<const uint8> all(frame);

	EXPECT_FALSE(compression::DecompressZstd({}, input.size()));
	EXPECT_FALSE(compression::DecompressZstd(all.first(all.size() - 1u), input.size()));   // truncated
	EXPECT_FALSE(compression::DecompressZstd(all.first(4u), input.size()));               // magic only

	std::vector<uint8> trailing = frame;
	trailing.push_back(0u);
	EXPECT_FALSE(compression::DecompressZstd(trailing, input.size()));

	std::vector<uint8> twoFrames = frame;
	twoFrames.insert(twoFrames.end(), frame.begin(), frame.end());
	EXPECT_FALSE(compression::DecompressZstd(twoFrames, 2u * input.size()));

	std::vector<uint8> badMagic = frame;
	badMagic[0] ^= 0xFFu;
	EXPECT_FALSE(compression::DecompressZstd(badMagic, input.size()));

	// Raw (incompressible) blocks carry no checksum of their own, so a flipped payload byte can decode
	// "successfully" to other bytes: callers verify the raw hash (Hash/StableHash.h). It must never be
	// out of bounds or the wrong size, though.
	std::vector<uint8> flipped = frame;
	flipped[flipped.size() / 2u] ^= 0x5Au;
	const std::optional<std::vector<uint8>> output = compression::DecompressZstd(flipped, input.size());
	if (output)
	{
		EXPECT_EQ(output->size(), input.size());
	}
}
