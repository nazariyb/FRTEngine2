// Asset identity (Assets/AssetId.h): GUID text round trips and the spellings Parse accepts, RFC 9562
// version and variant bits, the numeric-text re-roll, and frozen values of every derivation
// (MakeDerivedGuid, MakeAutoGuid, MakeAssetId64, MakeSubId). Those values are written into metas,
// cooked data and saves: a change here breaks every project, so each one is pinned, and each is also
// recomputed from the formula its header documents.
#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "Assets/AssetId.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"

using namespace frt;
using namespace frt::assets;


namespace
{
SGuid GuidFromText (FStringView Text)
{
	const std::optional<SGuid> guid = SGuid::Parse(Text);
	EXPECT_TRUE(guid.has_value()) << Text;
	return guid.value_or(SGuid());
}

void AppendLe64 (std::vector<uint8>& Out, uint64 Value)
{
	for (uint32 i = 0u; i < 8u; ++i)
	{
		Out.push_back(static_cast<uint8>(Value >> (8u * i)));
	}
}

void AppendField (std::vector<uint8>& Out, std::span<const uint8> Bytes)
{
	AppendLe64(Out, Bytes.size());
	Out.insert(Out.end(), Bytes.begin(), Bytes.end());
}

void SetBits (SGuid& Guid, uint8 Version)
{
	Guid.Bytes[6] = static_cast<uint8>((Guid.Bytes[6] & 0x0Fu) | (Version << 4u));
	Guid.Bytes[8] = static_cast<uint8>((Guid.Bytes[8] & 0x3Fu) | 0x80u);
}

constexpr FStringLiteral SampleGuidText = "9fc0d4010bbf28b4594072e72b8655ab";
}


TEST(AssetIdTest, GuidTextIsLowercaseHexInByteOrder)
{
	const SGuid guid = GuidFromText(SampleGuidText);
	EXPECT_EQ(guid.Bytes[0], 0x9Fu);
	EXPECT_EQ(guid.Bytes[15], 0xABu);
	EXPECT_EQ(guid.ToString(), SampleGuidText);

	FString text = "Guid: ";
	guid.AppendText(text);
	EXPECT_EQ(text, "Guid: 9fc0d4010bbf28b4594072e72b8655ab");
}

TEST(AssetIdTest, GuidParseAcceptsTheCommonSpellings)
{
	const SGuid expected = GuidFromText(SampleGuidText);
	for (const FStringView text : {
		"9FC0D4010BBF28B4594072E72B8655AB",
		"9fc0d401-0bbf-28b4-5940-72e72b8655ab",
		"9FC0D401-0BBF-28B4-5940-72E72B8655AB",
		"{9fc0d401-0bbf-28b4-5940-72e72b8655ab}",
		"{9FC0D401-0bbf-28B4-5940-72e72b8655AB}",
		"{9fc0d4010bbf28b4594072e72b8655ab}" })
	{
		EXPECT_EQ(SGuid::Parse(text), expected) << text;
	}
}

TEST(AssetIdTest, GuidParseRejectsEverythingElse)
{
	for (const FStringView text : {
		"",
		"9fc0d4010bbf28b4594072e72b8655a",            // 31 digits
		"9fc0d4010bbf28b4594072e72b8655abc",          // 33
		"9fc0d4010bbf28b4594072e72b8655ag",           // not hex
		"9fc0d401-0bbf-28b4-5940-72e72b8655a",        // short group
		"9fc0d4010-bbf-28b4-5940-72e72b8655ab",       // dash moved
		"9fc0d401-0bbf-28b4-594072e72b8655ab",        // a dash missing
		"9fc0d401-0bbf-28b4-5940-72e7-2b8655ab",      // a dash too many
		"{9fc0d401-0bbf-28b4-5940-72e72b8655ab",      // unbalanced braces
		"9fc0d401-0bbf-28b4-5940-72e72b8655ab}",
		"(9fc0d401-0bbf-28b4-5940-72e72b8655ab)",
		" 9fc0d4010bbf28b4594072e72b8655ab",          // whitespace
		"9fc0d4010bbf28b4594072e72b8655ab\n",
		"urn:uuid:9fc0d401-0bbf-28b4-5940-72e72b8655ab",
		"9fc0d4010bbf28b4594072e72b8655я",            // 32 bytes, not hex
		"{}" })
	{
		EXPECT_FALSE(SGuid::Parse(text).has_value()) << text;
	}
}

TEST(AssetIdTest, NilGuidParsesButIsNotValid)
{
	const std::optional<SGuid> nil = SGuid::Parse("00000000-0000-0000-0000-000000000000");
	ASSERT_TRUE(nil.has_value());
	EXPECT_FALSE(nil->IsValid());
	EXPECT_EQ(*nil, SGuid());
	EXPECT_TRUE(GuidFromText(SampleGuidText).IsValid());
}

TEST(AssetIdTest, GuidsCompareAsBytes)
{
	SGuid low;
	SGuid high;
	low.Bytes[0] = 0x7Fu;
	high.Bytes[0] = 0x80u;
	EXPECT_LT(low, high);
	low.Bytes[0] = 0x80u;
	low.Bytes[15] = 0x01u;
	EXPECT_GT(low, high);
	EXPECT_LT(GuidFromText("00000000000000000000000000000001"), GuidFromText("10000000000000000000000000000000"));
}

TEST(AssetIdTest, RandomGuidsAreVersion4WithTheRfcVariant)
{
	std::set<SGuid> seen;
	for (int32 i = 0; i < 2000; ++i)
	{
		const SGuid guid = SGuid::NewRandom();
		EXPECT_EQ(guid.GetVersion(), 4u);
		EXPECT_TRUE(guid.HasRfcVariant());
		EXPECT_TRUE(guid.IsValid());
		EXPECT_FALSE(guid.HasNumericText());
		EXPECT_EQ(SGuid::Parse(guid.ToString()), guid);
		EXPECT_TRUE(seen.insert(guid).second) << "duplicate random GUID " << guid.ToString();
	}
}

TEST(AssetIdTest, NumericTextIsDetected)
{
	EXPECT_TRUE(GuidFromText("12345678901234567890123456789012").HasNumericText());
	EXPECT_TRUE(GuidFromText("00000000000000000000000000000000").HasNumericText());
	EXPECT_TRUE(GuidFromText("1234567890123456e890123456789012").HasNumericText());   // digits and one e: 1.2e+...
	EXPECT_TRUE(GuidFromText("e2345678901234567890123456789012").HasNumericText());
	EXPECT_FALSE(GuidFromText("1234567890123456e8901234567890e2").HasNumericText());  // two e
	EXPECT_FALSE(GuidFromText("1234567890123456a890123456789012").HasNumericText());  // another letter
	EXPECT_FALSE(GuidFromText("1234567890123456e89012345678901f").HasNumericText());
	EXPECT_FALSE(GuidFromText(SampleGuidText).HasNumericText());
}

TEST(AssetIdTest, DerivedGuidIsTheDocumentedUuidV8)
{
	const SGuid parent = GuidFromText(SampleGuidText);
	const SGuid derived = MakeDerivedGuid(parent, "variant", "Win64-D3D12");
	EXPECT_EQ(derived.GetVersion(), 8u);
	EXPECT_TRUE(derived.HasRfcVariant());

	// derive_key("FRT derived GUID v1", Parent || le64(len) || Generator || le64(len) || Label).
	std::vector<uint8> material(std::begin(parent.Bytes), std::end(parent.Bytes));
	AppendField(material, hash::TextBytes("variant"));
	AppendField(material, hash::TextBytes("Win64-D3D12"));
	SGuid expected;
	hash::DeriveKeyBytes("FRT derived GUID v1", material, expected.Bytes);
	SetBits(expected, 8u);
	EXPECT_EQ(derived, expected);

	// Lengths keep the fields apart.
	EXPECT_NE(MakeDerivedGuid(parent, "ab", "c"), MakeDerivedGuid(parent, "a", "bc"));
	EXPECT_NE(MakeDerivedGuid(parent, "path", "x"), MakeDerivedGuid(SGuid(), "path", "x"));
	EXPECT_EQ(MakeDerivedGuid(parent, "variant", "Win64-D3D12"), derived);
}

TEST(AssetIdTest, DerivedGuidValuesAreFrozen)
{
	EXPECT_EQ(MakeDerivedGuid(SGuid(), "path", "/Project/Textures/Rock.png").ToString(), "85828d1c8b9786f5894c92299e9e25c5");
	EXPECT_EQ(MakeDerivedGuid(GuidFromText(SampleGuidText), "variant", "Win64-D3D12").ToString(), "6ff7353176e082bf852ae63c67433ed0");
	EXPECT_EQ(MakeDerivedGuid(GuidFromText(SampleGuidText), "transient", "Текстура 漢字").ToString(), "b9608680b06988ad99c72b11aea8c954");
}

TEST(AssetIdTest, AutoGuidIsTheDocumentedUuidV4)
{
	const hash::SContentHash content = hash::HashContent(hash::TextBytes("PNG bytes"));
	std::vector<uint8> input;
	const std::span<const uint8> path = hash::TextBytes("/project/textures/rock.png");
	input.insert(input.end(), path.begin(), path.end());
	input.insert(input.end(), std::begin(content.Bytes), std::end(content.Bytes));

	const SGuid guid = MakeAutoGuid("Demo", "auto-meta", input);
	EXPECT_EQ(guid.GetVersion(), 4u);
	EXPECT_TRUE(guid.HasRfcVariant());

	// derive_key("FRT auto GUID v1", le64(len) || Salt || le64(len) || Generator || le64(len) || Input).
	std::vector<uint8> material;
	AppendField(material, hash::TextBytes("Demo"));
	AppendField(material, hash::TextBytes("auto-meta"));
	AppendField(material, input);
	SGuid expected;
	hash::DeriveKeyBytes("FRT auto GUID v1", material, expected.Bytes);
	SetBits(expected, 4u);
	EXPECT_EQ(guid, expected);
	EXPECT_EQ(MakeAutoGuidCandidate("Demo", "auto-meta", input, 0u), expected);

	// Attempt n > 0 appends le32(n).
	const uint8 counter[4] = { 2u, 0u, 0u, 0u };
	material.insert(material.end(), std::begin(counter), std::end(counter));
	hash::DeriveKeyBytes("FRT auto GUID v1", material, expected.Bytes);
	SetBits(expected, 4u);
	EXPECT_EQ(MakeAutoGuidCandidate("Demo", "auto-meta", input, 2u), expected);

	// Two machines agree; another project (salt) or another file (input) does not.
	EXPECT_EQ(MakeAutoGuid("Demo", "auto-meta", input), guid);
	EXPECT_NE(MakeAutoGuid("Blank", "auto-meta", input), guid);
	EXPECT_NE(MakeAutoGuid("Demo", "auto-meta", hash::TextBytes("/project/textures/rock.png")), guid);
}

TEST(AssetIdTest, AutoGuidValuesAreFrozen)
{
	EXPECT_EQ(MakeAutoGuid("FRT.Engine", "auto-meta", hash::TextBytes("/engine/defaults/white.png")).ToString(), "8358dce72ab7428490a721ef68dc27ba");
	EXPECT_EQ(MakeAutoGuid("Demo", "migrate-metas", hash::TextBytes("/project/модель.gltf")).ToString(), "273321575b9b495aa75a1db1df1f1a87");
	EXPECT_EQ(MakeAutoGuidCandidate("Demo", "auto-meta", {}, 1u).ToString(), "afe6511d0fa6425f85b35290a26f5fbb");
}

TEST(AssetIdTest, AutoGuidRerollsWhenALiveGuidHasIt)
{
	const std::span<const uint8> input = hash::TextBytes("/project/a.png");
	const SGuid first = MakeAutoGuidCandidate("Demo", "dedupe", input, 0u);
	const SGuid second = MakeAutoGuidCandidate("Demo", "dedupe", input, 1u);
	const SGuid third = MakeAutoGuidCandidate("Demo", "dedupe", input, 2u);
	ASSERT_NE(first, second);

	EXPECT_EQ(MakeAutoGuid("Demo", "dedupe", input, [] (const SGuid&) { return false; }), first);
	EXPECT_EQ(MakeAutoGuid("Demo", "dedupe", input, [&] (const SGuid& Guid) { return Guid == first; }), second);
	const std::set<SGuid> live = { first, second };
	EXPECT_EQ(MakeAutoGuid("Demo", "dedupe", input, [&] (const SGuid& Guid) { return live.contains(Guid); }), third);
}

namespace
{
struct SNumericAutoGuid
{
	uint32 Index;                   // Input is le32(Index)
	FStringLiteral FirstCandidate;  // numeric
	FStringLiteral Result;          // candidate 1
};

// Inputs whose first candidate has numeric text, found by searching le32(0, 1, 2, ...) once: the
// all-digit case, and the digits-and-one-e case (a YAML 1.1 reader takes "...7e8..." for a float).
constexpr SNumericAutoGuid NumericAutoGuids[] =
{
	{ 1774475u, "73268818840046578426846337839757", "1a157f5380fe465c9292704c18947053" },
	{ 977861u, "e8214857537941759601723199963549", "54369a118a8d4cdfaa00f8a3e78e2175" },
};
}

TEST(AssetIdTest, AutoGuidRerollsNumericText)
{
	for (const SNumericAutoGuid& rerolled : NumericAutoGuids)
	{
		const uint8 input[4] = { static_cast<uint8>(rerolled.Index), static_cast<uint8>(rerolled.Index >> 8u),
			static_cast<uint8>(rerolled.Index >> 16u), static_cast<uint8>(rerolled.Index >> 24u) };
		const SGuid first = MakeAutoGuidCandidate("FRT.Test", "auto-meta", input, 0u);
		EXPECT_EQ(first.ToString(), rerolled.FirstCandidate);
		EXPECT_TRUE(first.HasNumericText());

		const SGuid guid = MakeAutoGuid("FRT.Test", "auto-meta", input);
		EXPECT_EQ(guid, MakeAutoGuidCandidate("FRT.Test", "auto-meta", input, 1u));
		EXPECT_FALSE(guid.HasNumericText());
		EXPECT_EQ(guid.ToString(), rerolled.Result);
	}
}

TEST(AssetIdTest, AssetId64IsTheDocumentedXxh3)
{
	const SAssetId id { GuidFromText(SampleGuidText), MakeSubId("mesh:Body") };
	uint8 bytes[24] = {};
	std::memcpy(bytes, id.Guid.Bytes, 16u);
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[16u + i] = static_cast<uint8>(id.SubId >> (8u * i));
	}
	EXPECT_EQ(MakeAssetId64(id).Value, hash::Xxh3_64(bytes, 0x5445535341545246ull));
	EXPECT_EQ(AssetId64Seed, 0x5445535341545246ull);

	// "FRTASSET" read as a little-endian uint64.
	uint64 seed = 0u;
	std::memcpy(&seed, "FRTASSET", 8u);
	EXPECT_EQ(seed, AssetId64Seed);

	EXPECT_NE(MakeAssetId64(id), MakeAssetId64(SAssetId { id.Guid, 0u }));
	EXPECT_TRUE(MakeAssetId64(id).IsValid());
	EXPECT_FALSE(SAssetId64().IsValid());
}

TEST(AssetIdTest, AssetId64ValuesAreFrozen)
{
	const SGuid guid = GuidFromText(SampleGuidText);
	EXPECT_EQ(MakeAssetId64(SAssetId { guid, 0u }).Value, 0x7AB17033936612A9ull);
	EXPECT_EQ(MakeAssetId64(SAssetId { guid, 1u }).Value, 0x4D1A794AA0C7B5E4ull);
	EXPECT_EQ(MakeAssetId64(SAssetId { guid, MakeSubId("mesh:Body") }).Value, 0xD40F1D980DFBEA5Dull);
	EXPECT_EQ(MakeAssetId64(SAssetId { SGuid(), 0u }).Value, 0xD92E1E48BEBC00A9ull);
}

TEST(AssetIdTest, SubIdIsXxh3OfTheLabel)
{
	EXPECT_EQ(MakeSubId("mesh:Body"), hash::Xxh3_64("mesh:Body"));
	EXPECT_EQ(MakeSubId("row:Rifle"), hash::Xxh3_64("row:Rifle"));
	EXPECT_NE(MakeSubId("mesh:Body"), MakeSubId("mesh:body"));
	EXPECT_NE(MakeSubId("mesh:Body"), 0u);
}

TEST(AssetIdTest, SubIdValuesAreFrozen)
{
	EXPECT_EQ(MakeSubId("mesh:Body"), 0x568F654962776E11ull);
	EXPECT_EQ(MakeSubId("material:Brick"), 0x824C133E0F9645F3ull);
	EXPECT_EQ(MakeSubId("row:Rifle"), 0x05AD1A2C786676A9ull);
	EXPECT_EQ(MakeSubId("image:Текстура"), 0x047A20C1309F5534ull);
	EXPECT_EQ(MakeSubId(""), 0x2D06800538D394C2ull);
}

TEST(AssetIdTest, AssetIdOrdersByGuidThenSubId)
{
	const SGuid guid = GuidFromText(SampleGuidText);
	const SAssetId main { guid, 0u };
	const SAssetId sub { guid, 5u };
	EXPECT_TRUE(main.IsMainAsset());
	EXPECT_FALSE(sub.IsMainAsset());
	EXPECT_TRUE(main.IsValid());
	EXPECT_FALSE(SAssetId().IsValid());
	EXPECT_LT(main, sub);
	EXPECT_LT((SAssetId { SGuid(), 99u }), main);
}
