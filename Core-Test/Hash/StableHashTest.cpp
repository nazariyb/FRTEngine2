// The persistent hashes (Hash/StableHash.h) produce exactly the published outputs: XXH3 against
// xxHash's own sanity vectors (cli/xsum_sanity_check.c of 0.8.3), BLAKE3 in hash and derive_key mode
// against the official test_vectors.json. Their bytes end up in metas, cooked data and cache keys, so a
// library update or a build flag that changes one value must fail here, not in a user's project.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>

#include "Containers/String.h"
#include "Hash/StableHash.h"

using namespace frt;


namespace
{
// ---------------- xxHash sanity vectors ----------------

constexpr uint64 Prime32 = 2654435761ull;
constexpr uint64 Prime64 = 11400714785074694797ull;

// XSUM_fillTestBuffer: the pseudorandom input every sanity vector hashes a prefix of.
std::vector<uint8> MakeXxhSanityBuffer (size_t Length)
{
	std::vector<uint8> buffer(Length);
	uint64 generator = Prime32;
	for (uint8& byte : buffer)
	{
		byte = static_cast<uint8>(generator >> 56u);
		generator *= Prime64;
	}
	return buffer;
}

struct SXxh3Vector
{
	uint32 Length;
	uint64 Seed;
	uint64 Hash;
};

// XSUM_XXH3_testdata: every length class of XXH3's implementation, with and without a seed.
constexpr SXxh3Vector Xxh3Vectors[] =
{
	{    0u,       0u, 0x2D06800538D394C2ull },
	{    0u, Prime64, 0xA8A6B918B2F0364Aull },
	{    1u,       0u, 0xC44BDFF4074EECDBull },
	{    1u, Prime64, 0x032BE332DD766EF8ull },
	{    6u,       0u, 0x27B56A84CD2D7325ull },
	{    6u, Prime64, 0x84589C116AB59AB9ull },
	{   12u,       0u, 0xA713DAF0DFBB77E7ull },
	{   12u, Prime64, 0xE7303E1B2336DE0Eull },
	{   24u,       0u, 0xA3FE70BF9D3510EBull },
	{   24u, Prime64, 0x850E80FC35BDD690ull },
	{   48u,       0u, 0x397DA259ECBA1F11ull },
	{   48u, Prime64, 0xADC2CBAA44ACC616ull },
	{   80u,       0u, 0xBCDEFBBB2C47C90Aull },
	{   80u, Prime64, 0xC6DD0CB699532E73ull },
	{  195u,       0u, 0xCD94217EE362EC3Aull },
	{  195u, Prime64, 0xBA68003D370CB3D9ull },
	{  403u,       0u, 0xCDEB804D65C6DEA4ull },
	{  403u, Prime64, 0x6259F6ECFD6443FDull },
	{  512u,       0u, 0x617E49599013CB6Bull },
	{  512u, Prime64, 0x3CE457DE14C27708ull },
	{ 2048u,       0u, 0xDD59E2C3A5F038E0ull },
	{ 2048u, Prime64, 0x66F81670669ABABCull },
	{ 2099u,       0u, 0xC6B9D9B3FC9AC765ull },
	{ 2099u, Prime64, 0x184F316843663974ull },
	{ 2240u,       0u, 0x6E73A90539CF2948ull },
	{ 2240u, Prime64, 0x757BA8487D1B5247ull },
	{ 2367u,       0u, 0xCB37AEB9E5D361EDull },
	{ 2367u, Prime64, 0xD2DB3415B942B42Aull },
};


// ---------------- BLAKE3 test vectors ----------------

// test_vectors.json: input of length n is the bytes 0, 1, ..., 250, 0, 1, ... (i % 251).
std::vector<uint8> MakeBlake3Input (size_t Length)
{
	std::vector<uint8> input(Length);
	for (size_t i = 0u; i < Length; ++i)
	{
		input[i] = static_cast<uint8>(i % 251u);
	}
	return input;
}

constexpr FStringLiteral Blake3TestContext = "BLAKE3 2019-12-27 16:29:52 test vectors context";

struct SBlake3Vector
{
	uint32 Length;
	FStringLiteral Hash;        // first 32 bytes of "hash"
	FStringLiteral DeriveKey;   // first 32 bytes of "derive_key"
};

// The lengths that cross BLAKE3's block (64) and chunk (1024) boundaries, and long inputs that take
// the SIMD paths with many chunks in parallel.
constexpr SBlake3Vector Blake3Vectors[] =
{
	{ 0u, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
		"2cc39783c223154fea8dfb7c1b1660f2ac2dcbd1c1de8277b0b0dd39b7e50d7d" },
	{ 1u, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213",
		"b3e2e340a117a499c6cf2398a19ee0d29cca2bb7404c73063382693bf66cb06c" },
	{ 63u, "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b",
		"b6451e30b953c206e34644c6803724e9d2725e0893039cfc49584f991f451af3" },
	{ 64u, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98",
		"a5c4a7053fa86b64746d4bb688d06ad1f02a18fce9afd3e818fefaa7126bf73e" },
	{ 65u, "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee",
		"51fd05c3c1cfbc8ed67d139ad76f5cf8236cd2acd26627a30c104dfd9d3ff8a8" },
	{ 1023u, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11",
		"74a16c1c3d44368a86e1ca6df64be6a2f64cce8f09220787450722d85725dea5" },
	{ 1024u, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7",
		"7356cd7720d5b66b6d0697eb3177d9f8d73a4a5c5e968896eb6a689684302706" },
	{ 1025u, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444",
		"effaa245f065fbf82ac186839a249707c3bddf6d3fdda22d1b95a3c970379bcb" },
	{ 2048u, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a",
		"7b2945cb4fef70885cc5d78a87bf6f6207dd901ff239201351ffac04e1088a23" },
	{ 3073u, "7124b49501012f81cc7f11ca069ec9226cecb8a2c850cfe644e327d22d3e1cd3",
		"72613c9ec9ff7e40f8f5c173784c532ad852e827dba2bf85b2ab4b76f7079081" },
	{ 8193u, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b",
		"af1e0346e389b17c23200270a64aa4e1ead98c61695d917de7d5b00491c9b0f1" },
	{ 31744u, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47",
		"39772aef80e0ebe60596361e45b061e8f417429d529171b6764468c22928e28e" },
	{ 102400u, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085",
		"4652cff7a3f385a6103b5c260fc1593e13c778dbe608efb092fe7ee69df6e9c6" },
};

// The full 131-byte outputs for 1025 bytes: the extendable output past the first 32 bytes.
constexpr FStringLiteral Blake3Hash1025Long =
	"d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444f4c4a22b4b399155358a994e52bf255de60035742ec71bd08a"
	"c275a1b51cc6bfe332b0ef84b409108cda080e6269ed4b3e2c3f7d722aa4cdc98d16deb554e5627be8f955c98e1d5f9565a9194cad0c4285f9"
	"3700062d9595adb992ae68ff12800ab67a";
constexpr FStringLiteral Blake3DeriveKey1025Long =
	"effaa245f065fbf82ac186839a249707c3bddf6d3fdda22d1b95a3c970379bcb5d31013a167509e9066273ab6e2123bc835b408b067d88f96a"
	"ddb550d96b6852dad38e320b9d940f86db74d398c770f462118b35d2724efa13da97194491d96dd37c3c09cbef665953f2ee85ec83d88b88d1"
	"1547a6f911c8217cca46defa2751e7f3ad";

FString ToHex (std::span<const uint8> Bytes)
{
	return FString::FromHexBytes(Bytes, false);
}

// The first 20 bytes of a 32-byte vector, as SContentHash text.
FStringView First160 (FStringView Hex256)
{
	return Hex256.Left(hash::SContentHash::TextLength);
}
}


TEST(StableHashTest, Xxh3MatchesTheOfficialSanityVectors)
{
	const std::vector<uint8> buffer = MakeXxhSanityBuffer(2367u);
	for (const SXxh3Vector& kat : Xxh3Vectors)
	{
		const std::span<const uint8> input = std::span(buffer).first(kat.Length);
		EXPECT_EQ(hash::Xxh3_64(input, kat.Seed), kat.Hash) << "length " << kat.Length << ", seed " << kat.Seed;
	}
}

TEST(StableHashTest, Xxh3OfTextHashesItsUtf8Bytes)
{
	const FStringView text = "Привіт, 漢字 😀";
	EXPECT_EQ(hash::Xxh3_64(text), hash::Xxh3_64(text.AsBytes()));
	EXPECT_EQ(hash::Xxh3_64(text, 42u), hash::Xxh3_64(text.AsBytes(), 42u));
	EXPECT_NE(hash::Xxh3_64(text), hash::Xxh3_64(text, 42u));
	// The empty text is the empty input, whatever FStringView stores for it.
	EXPECT_EQ(hash::Xxh3_64(FStringView()), 0x2D06800538D394C2ull);
}

TEST(StableHashTest, Blake3HashMatchesTheOfficialVectors)
{
	for (const SBlake3Vector& kat : Blake3Vectors)
	{
		const std::vector<uint8> input = MakeBlake3Input(kat.Length);

		std::array<uint8, 32> full = {};
		hash::CContentHasher hasher;
		hasher.Update(input);
		hasher.FinalizeTo(full);
		EXPECT_EQ(ToHex(full), kat.Hash) << "length " << kat.Length;

		EXPECT_EQ(hash::HashContent(input).ToString(), First160(kat.Hash)) << "length " << kat.Length;
	}
}

TEST(StableHashTest, Blake3DeriveKeyMatchesTheOfficialVectors)
{
	for (const SBlake3Vector& kat : Blake3Vectors)
	{
		const std::vector<uint8> input = MakeBlake3Input(kat.Length);

		std::array<uint8, 32> full = {};
		hash::DeriveKeyBytes(Blake3TestContext, input, full);
		EXPECT_EQ(ToHex(full), kat.DeriveKey) << "length " << kat.Length;

		EXPECT_EQ(hash::DeriveKey(Blake3TestContext, input).ToString(), First160(kat.DeriveKey)) << "length " << kat.Length;
	}
}

TEST(StableHashTest, Blake3ExtendableOutputMatchesTheLongVectors)
{
	const std::vector<uint8> input = MakeBlake3Input(1025u);

	std::array<uint8, 131> output = {};
	hash::CContentHasher hasher;
	hasher.Update(input);
	hasher.FinalizeTo(output);
	EXPECT_EQ(ToHex(output), Blake3Hash1025Long);

	hash::DeriveKeyBytes(Blake3TestContext, input, output);
	EXPECT_EQ(ToHex(output), Blake3DeriveKey1025Long);
}

TEST(StableHashTest, IncrementalUpdatesEqualOneShot)
{
	const std::vector<uint8> input = MakeBlake3Input(102400u);
	const std::span<const uint8> all(input);

	// Pieces of every awkward size around BLAKE3's 64-byte blocks and 1024-byte chunks, cycled.
	constexpr size_t pieceSizes[] = { 1u, 7u, 63u, 64u, 65u, 1023u, 1024u, 1025u, 4096u, 0u, 3u };
	hash::CContentHasher plain;
	hash::CContentHasher derived = hash::CContentHasher::ForDeriveKey(Blake3TestContext);
	size_t offset = 0u;
	for (size_t i = 0u; offset < all.size(); ++i)
	{
		const size_t count = (std::min)(pieceSizes[i % std::size(pieceSizes)], all.size() - offset);
		plain.Update(all.subspan(offset, count));
		derived.Update(all.subspan(offset, count));
		offset += count;
	}

	EXPECT_EQ(plain.Finalize(), hash::HashContent(all));
	EXPECT_EQ(plain.Finalize().ToString(), First160(Blake3Vectors[std::size(Blake3Vectors) - 1u].Hash));
	EXPECT_EQ(derived.Finalize(), hash::DeriveKey(Blake3TestContext, all));
	EXPECT_EQ(derived.Finalize().ToString(), First160(Blake3Vectors[std::size(Blake3Vectors) - 1u].DeriveKey));
}

TEST(StableHashTest, FinalizeLeavesTheHasherUsable)
{
	const std::vector<uint8> input = MakeBlake3Input(3073u);
	const std::span<const uint8> all(input);

	hash::CContentHasher hasher;
	hasher.Update(all.first(1025u));
	const hash::SContentHash prefix = hasher.Finalize();
	EXPECT_EQ(prefix, hash::HashContent(all.first(1025u)));

	// A copy forks the state; the original goes on where it was.
	hash::CContentHasher fork = hasher;
	hasher.Update(all.subspan(1025u));
	EXPECT_EQ(hasher.Finalize(), hash::HashContent(all));
	EXPECT_EQ(fork.Finalize(), prefix);
}

TEST(StableHashTest, ResetStartsOverInTheSameMode)
{
	const std::vector<uint8> input = MakeBlake3Input(2048u);

	hash::CContentHasher plain;
	plain.Update(input);
	plain.Reset();
	EXPECT_EQ(plain.Finalize().ToString(), First160(Blake3Vectors[0].Hash));

	hash::CContentHasher derived = hash::CContentHasher::ForDeriveKey(Blake3TestContext);
	derived.Update(MakeBlake3Input(5u));
	derived.Reset();
	derived.Update(input);
	EXPECT_EQ(derived.Finalize(), hash::DeriveKey(Blake3TestContext, input));
}

TEST(StableHashTest, DeriveKeyIsDomainSeparated)
{
	const std::vector<uint8> input = MakeBlake3Input(64u);
	EXPECT_NE(hash::DeriveKey("FRT test context A", input), hash::DeriveKey("FRT test context B", input));
	EXPECT_NE(hash::DeriveKey("FRT test context A", input), hash::HashContent(input));
}

TEST(StableHashTest, ContentHashTextRoundTrip)
{
	const hash::SContentHash contentHash = hash::HashContent(MakeBlake3Input(1u));
	const FString text = contentHash.ToString();
	EXPECT_EQ(text, "2d3adedff11b61f14c886e35afa036736dcd87a7");
	EXPECT_EQ(text.Len(), hash::SContentHash::TextLength);

	EXPECT_EQ(hash::SContentHash::Parse(text), contentHash);
	EXPECT_EQ(hash::SContentHash::Parse("2D3ADEDFF11B61F14C886E35AFA036736DCD87A7"), contentHash);

	FString appended = "hash=";
	contentHash.AppendText(appended);
	EXPECT_EQ(appended, "hash=2d3adedff11b61f14c886e35afa036736dcd87a7");
}

TEST(StableHashTest, ContentHashParseRejectsEverythingElse)
{
	EXPECT_FALSE(hash::SContentHash::Parse(""));
	EXPECT_FALSE(hash::SContentHash::Parse("2d3adedff11b61f14c886e35afa036736dcd87a"));       // 39
	EXPECT_FALSE(hash::SContentHash::Parse("2d3adedff11b61f14c886e35afa036736dcd87a70"));     // 41
	EXPECT_FALSE(hash::SContentHash::Parse("2d3adedff11b61f14c886e35afa036736dcd87ag"));      // not hex
	EXPECT_FALSE(hash::SContentHash::Parse(" 2d3adedff11b61f14c886e35afa036736dcd87a"));      // space
	EXPECT_FALSE(hash::SContentHash::Parse("2d3adedff11b61f14c886e35afa036736dcd87я"));       // 40 bytes, not hex
}

TEST(StableHashTest, ContentHashComparesAsBytes)
{
	hash::SContentHash low;
	hash::SContentHash high;
	low.Bytes[0] = 0x01u;
	high.Bytes[0] = 0x80u;
	EXPECT_LT(low, high);
	EXPECT_FALSE(hash::SContentHash().IsValid());
	EXPECT_TRUE(low.IsValid());
	high.Bytes[0] = 0x01u;
	EXPECT_EQ(low, high);
}
