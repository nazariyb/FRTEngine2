// The canonical KeyBlob (Assets/KeyWriter.h): byte for byte the documented format, the same bytes
// whatever order a builder writes its fields in, and refused - never silently written - when a value
// has no canonical form (NaN) or names collide. Cache keys hash these bytes: an encoding change would
// orphan every cached artifact, and a non-canonical one would rebuild forever or, worse, alias keys.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "Assets/AssetId.h"
#include "Assets/KeyWriter.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"

using namespace frt;
using namespace frt::assets;


namespace
{
hash::SContentHash MakeSampleHash ()
{
	hash::SContentHash value;
	for (uint32 i = 0u; i < hash::SContentHash::Size; ++i)
	{
		value.Bytes[i] = static_cast<uint8>(i + 1u);
	}
	return value;
}

SGuid MakeSampleGuid ()
{
	return SGuid::Parse("9fc0d4010bbf28b4594072e72b8655ab").value_or(SGuid());
}

// One field of every type, written in the order given by Order (a permutation of 0..10).
std::vector<uint8> WriteSample (std::span<const int32> Order)
{
	const uint8 bytes[] = { 0x00u, 0xFFu };
	CKeyWriter key;
	for (const int32 index : Order)
	{
		switch (index)
		{
		case 0: key.WriteBool("A", true); break;
		case 1: key.WriteBytes("B", bytes); break;
		case 2: key.WriteF64("D", -0.0); break;
		case 3: key.WriteEnum("E", "Linear"); break;
		case 4: key.WriteF32("F", 1.5f); break;
		case 5: key.WriteGuid("G", MakeSampleGuid()); break;
		case 6: key.WriteHash160("H", MakeSampleHash()); break;
		case 7: key.WriteI64("I", -2); break;
		case 8: key.BeginRecord("R"); key.WriteU64("x", 300u); key.EndRecord(); break;
		case 9: key.WriteStr("S", "Tex"); break;
		case 10: key.WriteU64("U", 1u); break;
		default: ADD_FAILURE() << "no sample field " << index; break;
		}
	}
	FString error;
	std::optional<std::vector<uint8>> blob = key.Finish(&error);
	EXPECT_TRUE(blob.has_value()) << error;
	return blob.value_or(std::vector<uint8>());
}

// The sample blob, encoded by hand from the format in KeyWriter.h.
const std::vector<uint8> SampleGolden =
{
	0x0B,                                                    // 11 fields
	0x07, 0x01, 'A', 0x01,                                   // Bool A = true
	0x0B, 0x01, 'B', 0x02, 0x00, 0xFF,                       // Bytes B = 00 FF
	0x09, 0x01, 'D', 0, 0, 0, 0, 0, 0, 0, 0,                 // F64 D = -0.0, written as +0.0
	0x0A, 0x01, 'E', 0x06, 'L', 'i', 'n', 'e', 'a', 'r',     // Enum E = Linear
	0x08, 0x01, 'F', 0x00, 0x00, 0xC0, 0x3F,                 // F32 F = 1.5 (0x3FC00000)
	0x02, 0x01, 'G',                                         // Guid G
	0x9F, 0xC0, 0xD4, 0x01, 0x0B, 0xBF, 0x28, 0xB4, 0x59, 0x40, 0x72, 0xE7, 0x2B, 0x86, 0x55, 0xAB,
	0x04, 0x01, 'H',                                         // Hash160 H = 01..14
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
	0x06, 0x01, 'I', 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,     // I64 I = -2
	0x03, 0x01, 'R',                                         // Record R
		0x01,                                                //   1 field
		0x05, 0x01, 'x', 0x2C, 0x01, 0, 0, 0, 0, 0, 0,       //   U64 x = 300
	0x01, 0x01, 'S', 0x03, 'T', 'e', 'x',                    // Str S = Tex
	0x05, 0x01, 'U', 0x01, 0, 0, 0, 0, 0, 0, 0,              // U64 U = 1
};

// Root field names of a blob whose fields are all Bool (type, name length < 128, name, value).
std::vector<FString> ReadBoolFieldNames (const std::vector<uint8>& Blob)
{
	std::vector<FString> names;
	size_t at = 1u;
	while (at + 2u <= Blob.size())
	{
		const size_t length = Blob[at + 1u];
		names.emplace_back(reinterpret_cast<const char*>(&Blob[at + 2u]), static_cast<uint32>(length));
		at += 2u + length + 1u;
	}
	return names;
}

FString FinishError (const CKeyWriter& Key)
{
	FString error;
	EXPECT_FALSE(Key.Finish(&error).has_value());
	return error;
}
}


TEST(KeyWriterTest, BlobMatchesTheGoldenBytes)
{
	const int32 order[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	EXPECT_EQ(WriteSample(order), SampleGolden);
}

TEST(KeyWriterTest, FieldOrderDoesNotMatter)
{
	const int32 reversed[] = { 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
	const int32 shuffled[] = { 5, 8, 0, 10, 3, 1, 9, 6, 2, 7, 4 };
	EXPECT_EQ(WriteSample(reversed), SampleGolden);
	EXPECT_EQ(WriteSample(shuffled), SampleGolden);

	// Inside nested records too.
	CKeyWriter first;
	first.BeginRecord("Source");
	first.WriteU64("Size", 7u);
	first.WriteHash160("Hash", MakeSampleHash());
	first.EndRecord();
	first.WriteStr("Builder", "Texture");

	CKeyWriter second;
	second.WriteStr("Builder", "Texture");
	second.BeginRecord("Source");
	second.WriteHash160("Hash", MakeSampleHash());
	second.WriteU64("Size", 7u);
	second.EndRecord();

	EXPECT_EQ(first.Finish(), second.Finish());
}

TEST(KeyWriterTest, NamesSortAsBytes)
{
	CKeyWriter key;
	for (const FStringView name : { "source", "Sourcea", "Source2", "Source", "S", "Ä", "Z" })
	{
		key.WriteBool(name, false);
	}
	const std::optional<std::vector<uint8>> blob = key.Finish();
	ASSERT_TRUE(blob.has_value());
	const std::vector<FString> expected = { "S", "Source", "Source2", "Sourcea", "Z", "source", "Ä" };
	EXPECT_EQ(ReadBoolFieldNames(*blob), expected);
}

TEST(KeyWriterTest, LengthsAreLeb128Varints)
{
	CKeyWriter key;
	key.WriteBytes("Blob", std::vector<uint8>(200u, 0x5Au));
	const std::optional<std::vector<uint8>> blob = key.Finish();
	ASSERT_TRUE(blob.has_value());
	ASSERT_EQ(blob->size(), 1u + 1u + 1u + 4u + 2u + 200u);
	const std::vector<uint8> head(blob->begin(), blob->begin() + 9);
	EXPECT_EQ(head, (std::vector<uint8> { 0x01, 0x0B, 0x04, 'B', 'l', 'o', 'b', 0xC8, 0x01 }));

	CKeyWriter longName;
	longName.WriteBool(FString::Repeat("n", 130u), true);
	const std::optional<std::vector<uint8>> named = longName.Finish();
	ASSERT_TRUE(named.has_value());
	EXPECT_EQ((*named)[2], 0x82u);
	EXPECT_EQ((*named)[3], 0x01u);
}

TEST(KeyWriterTest, FloatsAreCanonicalBits)
{
	CKeyWriter positive;
	positive.WriteF32("X", 0.0f);
	positive.WriteF64("Y", 0.0);
	CKeyWriter negative;
	negative.WriteF32("X", -0.0f);
	negative.WriteF64("Y", -0.0);
	EXPECT_EQ(positive.Finish(), negative.Finish());

	// Infinity has bits of its own; it is a value, unlike NaN.
	CKeyWriter infinite;
	infinite.WriteF32("X", std::numeric_limits<float>::infinity());
	const std::optional<std::vector<uint8>> blob = infinite.Finish();
	ASSERT_TRUE(blob.has_value());
	EXPECT_EQ(*blob, (std::vector<uint8> { 0x01, 0x08, 0x01, 'X', 0x00, 0x00, 0x80, 0x7F }));
}

TEST(KeyWriterTest, NaNIsRefused)
{
	CKeyWriter f32;
	f32.BeginRecord("Settings");
	f32.WriteF32("Gamma", std::numeric_limits<float>::quiet_NaN());
	f32.EndRecord();
	EXPECT_TRUE(f32.HasError());
	EXPECT_EQ(FinishError(f32), "Settings.Gamma: NaN cannot be part of a key");

	CKeyWriter f64;
	f64.WriteF64("Scale", -std::numeric_limits<double>::quiet_NaN());
	EXPECT_EQ(FinishError(f64), "Scale: NaN cannot be part of a key");
}

TEST(KeyWriterTest, EnumsAndStringsAreDifferentValues)
{
	CKeyWriter asEnum;
	asEnum.WriteEnum("Mode", "Linear");
	CKeyWriter asStr;
	asStr.WriteStr("Mode", "Linear");
	EXPECT_NE(asEnum.Finish(), asStr.Finish());

	CKeyWriter unnamed;
	unnamed.WriteEnum("Mode", "");
	EXPECT_EQ(FinishError(unnamed), "Mode: an enum needs its stable name");
}

TEST(KeyWriterTest, DuplicateNamesAreRefused)
{
	CKeyWriter root;
	root.WriteU64("Size", 1u);
	root.WriteStr("Size", "one");
	EXPECT_EQ(FinishError(root), "Size: duplicate field name");

	CKeyWriter nested;
	nested.BeginRecord("Target");
	nested.BeginRecord("Facets");
	nested.WriteStr("Rhi", "D3D12");
	nested.WriteStr("Rhi", "None");
	nested.EndRecord();
	nested.EndRecord();
	EXPECT_EQ(FinishError(nested), "Target.Facets.Rhi: duplicate field name");

	// A record and a field may not share a name either.
	CKeyWriter mixed;
	mixed.WriteU64("Source", 1u);
	mixed.BeginRecord("Source");
	mixed.EndRecord();
	EXPECT_EQ(FinishError(mixed), "Source: duplicate field name");
}

TEST(KeyWriterTest, MisuseIsReportedNotWritten)
{
	CKeyWriter emptyName;
	emptyName.WriteU64("", 1u);
	EXPECT_EQ(FinishError(emptyName), "<empty name>: empty field name");

	CKeyWriter unbalanced;
	unbalanced.EndRecord();
	EXPECT_EQ(FinishError(unbalanced), "EndRecord: no record is open");

	CKeyWriter open;
	open.BeginRecord("Target");
	open.BeginRecord("Facets");
	EXPECT_FALSE(open.HasError());
	EXPECT_EQ(FinishError(open), "Target.Facets: record not ended");

	// The first mistake is the one reported.
	CKeyWriter several;
	several.WriteF32("First", std::numeric_limits<float>::quiet_NaN());
	several.WriteU64("", 1u);
	EXPECT_EQ(several.GetError(), "First: NaN cannot be part of a key");
}

TEST(KeyWriterTest, FinishLeavesTheWriterUsable)
{
	CKeyWriter key;
	key.WriteStr("Builder", "Texture");
	const std::optional<std::vector<uint8>> first = key.Finish();
	EXPECT_EQ(key.Finish(), first);

	key.WriteU64("Version", 2u);
	const std::optional<std::vector<uint8>> second = key.Finish();
	ASSERT_TRUE(second.has_value());
	EXPECT_NE(second, first);
	EXPECT_EQ((*second)[0], 0x02u);
}

TEST(KeyWriterTest, EmptyBlobIsZeroFields)
{
	const std::optional<std::vector<uint8>> blob = CKeyWriter().Finish();
	ASSERT_TRUE(blob.has_value());
	EXPECT_EQ(*blob, std::vector<uint8> { 0x00 });
}

TEST(KeyWriterTest, StaticKeyIsDerivedFromTheBlob)
{
	EXPECT_EQ(MakeStaticKey(SampleGolden), hash::DeriveKey("FRT DDC static v1", SampleGolden));
	EXPECT_NE(MakeStaticKey(SampleGolden), hash::HashContent(SampleGolden));
	EXPECT_EQ(MakeStaticKey(SampleGolden).ToString(), "9a23c10c5369aee9732d8f9500e7f80f1c6c9847");
	EXPECT_EQ(MakeStaticKey(std::vector<uint8> { 0x00 }).ToString(), "5b52681066e0e287f511d324db46402afcc08ac0");
}
