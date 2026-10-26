// The registry's storage primitives (Registry/PagedColumn.h): a write to a page of an older epoch copies the
// page and leaves the old one to whoever shares it; the paged hash index keeps duplicates, survives erases
// and rehashes; the string pool never moves a string, oversized ones included. And the fast paths of
// RegistryTables.h answer as the general functions they stand in for: the caseless hashes, the fold, the stem.
#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Registry/PagedColumn.h"
#include "Assets/Registry/RegistryTables.h"
#include "Containers/String.h"

using namespace frt;
using namespace frt::assets;


TEST(PagedColumnTest, AWriteToAFrozenPageCopiesItAndLeavesTheSharedCopyAlone)
{
	CPagedColumn<uint32> column;
	uint64 epoch = 1u;
	for (uint32 i = 0u; i < RegistryPageRows + 5u; ++i)
	{
		column.Append(i, epoch);
	}
	ASSERT_EQ(column.GetPageCount(), 2u);
	const void* firstPage = column.GetPageAddress(0u);

	// Same epoch: written in place.
	column.Set(3u, 300u, epoch);
	EXPECT_EQ(column.GetPageAddress(0u), firstPage);

	// A published copy freezes the pages: the next write copies only the page it touches.
	const CPagedColumn<uint32> published = column;
	++epoch;
	column.Set(4u, 400u, epoch);
	EXPECT_NE(column.GetPageAddress(0u), firstPage);
	EXPECT_EQ(column.GetPageAddress(1u), published.GetPageAddress(1u));
	EXPECT_EQ(published[4u], 4u);
	EXPECT_EQ(column[4u], 400u);
	EXPECT_EQ(column[3u], 300u);

	// SetIfChanged with the current value copies nothing.
	const CPagedColumn<uint32> second = column;
	++epoch;
	column.SetIfChanged(RegistryPageRows + 1u, RegistryPageRows + 1u, epoch);
	EXPECT_EQ(column.GetPageAddress(1u), second.GetPageAddress(1u));
}

TEST(PagedColumnTest, TheBitmapGrowsAndCopiesOnWrite)
{
	CPagedBitmap bits;
	bits.Grow(130u, 1u);
	bits.Set(129u, true, 1u);
	EXPECT_TRUE(bits.Test(129u));
	EXPECT_FALSE(bits.Test(128u));
	const CPagedBitmap published = bits;
	bits.Set(0u, true, 2u);
	EXPECT_TRUE(bits.Test(0u));
	EXPECT_FALSE(published.Test(0u));
	EXPECT_TRUE(published.Test(129u));
}

TEST(PagedColumnTest, TheHashIndexKeepsDuplicatesAndSurvivesErasesAndRehashes)
{
	CPagedHashIndex index;
	uint64 epoch = 1u;
	const auto collect = [&] (uint64 Hash)
	{
		std::multiset<uint32> values;
		index.ForEach(Hash, [&] (uint32 Value) { values.insert(Value); return true; });
		return values;
	};
	for (uint32 i = 0u; i < 10000u; ++i)
	{
		index.Insert(MixRegistryKey(i / 2u), i, epoch);   // two values per key
	}
	EXPECT_EQ(index.GetCount(), 10000u);
	EXPECT_EQ(collect(MixRegistryKey(7u)), (std::multiset<uint32> { 14u, 15u }));

	const CPagedHashIndex published = index;
	++epoch;
	EXPECT_TRUE(index.Erase(MixRegistryKey(7u), 14u, epoch));
	EXPECT_FALSE(index.Erase(MixRegistryKey(7u), 14u, epoch));
	EXPECT_EQ(collect(MixRegistryKey(7u)), std::multiset<uint32> { 15u });
	std::multiset<uint32> old;
	published.ForEach(MixRegistryKey(7u), [&] (uint32 Value) { old.insert(Value); return true; });
	EXPECT_EQ(old, (std::multiset<uint32> { 14u, 15u }));

	// Churn: erase and reinsert everything a few times (tombstones force rehashes).
	for (uint32 round = 0u; round < 3u; ++round)
	{
		for (uint32 i = 0u; i < 10000u; ++i)
		{
			if (i != 14u)
			{
				ASSERT_TRUE(index.Erase(MixRegistryKey(i / 2u), i, epoch));
			}
		}
		EXPECT_EQ(index.GetCount(), 0u);
		for (uint32 i = 0u; i < 10000u; ++i)
		{
			if (i != 14u)
			{
				index.Insert(MixRegistryKey(i / 2u), i, epoch);
			}
		}
	}
	EXPECT_EQ(collect(MixRegistryKey(4999u)), (std::multiset<uint32> { 9998u, 9999u }));
	EXPECT_LE(index.GetCapacity(), 65536u);
}

TEST(PagedColumnTest, TheStringPoolNeverMovesAString)
{
	CStringPool pool;
	std::vector<uint32> ids;
	std::vector<FString> texts;
	for (uint32 i = 0u; i < 5000u; ++i)
	{
		texts.push_back(FString::Format("/Project/Folder{}/Ïмя_{}.png", i % 17u, i));
		ids.push_back(pool.Add(texts.back(), 1u));
	}
	const FString huge = FString::Repeat("x", CStringPool::BlockBytes + 10u);
	const uint32 hugeId = pool.Add(huge, 1u);
	const uint32 afterHuge = pool.Add("after", 1u);
	const CStringPool published = pool;
	pool.Add("written after the copy", 2u);
	for (uint32 i = 0u; i < ids.size(); ++i)
	{
		ASSERT_EQ(published.Get(ids[i]), texts[i].View());
		ASSERT_EQ(pool.Get(ids[i]), texts[i].View());
	}
	EXPECT_EQ(pool.Get(hugeId), huge.View());
	EXPECT_EQ(pool.Get(afterHuge), "after");
	EXPECT_EQ(published.Size(), pool.Size() - 1u);
}

TEST(PagedColumnTest, TheNamePoolInternsExactlyAndKeepsAFoldedShadow)
{
	CNamePool names;
	const uint32 rock = names.Intern("Rock", 1u);
	EXPECT_EQ(names.Intern("Rock", 1u), rock);
	const uint32 lower = names.Intern("rock", 1u);
	EXPECT_NE(lower, rock);
	EXPECT_EQ(names.GetFolded(rock), "rock");
	EXPECT_EQ(names.GetFolded(lower), "rock");
	EXPECT_EQ(names.Find("Rock"), rock);
	EXPECT_FALSE(names.Find("ROCK").has_value());
	const uint32 kelvin = names.Intern("\xE2\x84\xAA", 1u);   // U+212A KELVIN SIGN folds to 'k'
	EXPECT_EQ(names.GetFolded(kelvin), "k");
}

TEST(PagedColumnTest, TheRegistrysFastCaseHashesAreTheContainersHashes)
{
	// HashPortable and HashMountRule lower or raise ASCII text into a buffer instead of streaming it: their values
	// must stay the general functors' (SStringHashIgnoreCase, SPathHash), and the equalities theirs, for every text.
	std::vector<FString> texts = { "", "a", "A", "/Project/Textures/T_Rock_Albedo.png", "/project/textures/t_rock_albedo.png", "@`[{", "Rock#mesh:Body",
		"/Project/Тека/Файл.png", "/Project/\xE2\x84\xAA.png", "/Project/k.png", "/Project/K.png", "Stra\xC3\x9F" "e", "\xED\xA0\x80" "ill-formed" };
	texts.push_back(FString::Repeat("Ab", 128u));          // exactly 256 bytes: the last fast length
	texts.push_back(FString::Repeat("Ab", 128u) + "C");    // 257 bytes: the streamed chunks
	texts.push_back(FString::Repeat("x", 1000u));
	for (const FString& a : texts)
	{
		EXPECT_EQ(HashPortable(a), static_cast<uint64>(SStringHashIgnoreCase {}(a))) << a.CStr();
		EXPECT_EQ(HashMountRule(a, false), static_cast<uint64>(SPathHash {}(a))) << a.CStr();
		EXPECT_EQ(HashMountRule(a, true), static_cast<uint64>(SStringHash {}(a))) << a.CStr();
		char buffer[256];
		FString heap;
		EXPECT_EQ(FoldPortable(a, buffer, heap), FString(a).FoldCase().View()) << a.CStr();
		for (const FString& b : texts)
		{
			EXPECT_EQ(EqualPortable(a, b), SStringEqualIgnoreCase {}(a, b)) << a.CStr() << " / " << b.CStr();
			EXPECT_EQ(EqualMountRule(a, b, false), SPathEqual {}(a, b)) << a.CStr() << " / " << b.CStr();
		}
	}
}

TEST(PagedColumnTest, TheRecordStemIsTheImporterTablesStem)
{
	// GetRecordStem takes a byte path for ASCII names; it must answer as GetAssetStem does for every name: every
	// claimed extension in any case, multi-dot claims, unclaimed extensions, dot-names, non-ASCII and bare names.
	std::vector<FString> names = { "", "a", ".gitkeep", "README", "a.b.c", "Wood.FRTMAT.YML", "Wood.frtmat.yml", "x.frtmat", "Rock.PNG",
		"Тека.png", "\xE2\x84\xAA.png", "rock.\xE2\x84\xAA", "name.\xC5\xBF" "vg", "trailing.", "..", "a..png", "map.frtinputmap" };
	for (const SImporterDesc& importer : GetImporters())
	{
		for (const FStringLiteral extension : importer.Extensions)
		{
			const FString text(static_cast<FStringView>(extension));
			names.push_back(text);                              // the extension alone: nothing before it
			names.push_back("Asset" + text);
			names.push_back("Asset" + FString(text).ToUpper());
			names.push_back("Asset.v2" + text);
			names.push_back("Ä" + text);
		}
	}
	for (const FString& name : names)
	{
		EXPECT_EQ(GetRecordStem(name), GetAssetStem(name)) << name.CStr();
	}
}
