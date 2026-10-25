// The §5.8 query text syntax lowered to SAssetQuery, before any registry exists: every row of the spec's table and
// its examples, `or` and parentheses as AnyOf groups, the builder API giving the same query as the text, the
// `is:` predicates (three of which match nothing until their milestone), positioned errors - the column of the term
// at fault - and a seeded fuzz run over random token streams: no input crashes, every error has a position.
#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "Assets/Registry/AssetQuery.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"

using namespace frt;
using namespace frt::assets;


namespace
{
FString Lower (FStringView Text)
{
	SDiagnostic error;
	const std::optional<SAssetQuery> query = SAssetQuery::Parse(Text, &error);
	EXPECT_TRUE(query.has_value()) << Text.GetData() << " -> " << error.Column << ": " << error.Message.CStr();
	return query ? query->Describe() : FString("<error>");
}

SDiagnostic ErrorOf (FStringView Text)
{
	SDiagnostic error;
	EXPECT_FALSE(SAssetQuery::Parse(Text, &error).has_value()) << Text.GetData();
	EXPECT_EQ(error.Severity, ESeverity::Error);
	EXPECT_EQ(error.Line, 1u);
	EXPECT_GE(error.Column, 1u);
	EXPECT_LE(error.Column, Text.Len() + 1u);
	EXPECT_FALSE(error.Message.IsEmpty());
	return error;
}
}


// ---------------- The spec's table ----------------

TEST(AssetQueryParseTest, TextTermsAndPhrases)
{
	EXPECT_EQ(Lower("rifle \"assault rifle\""), "text:\"rifle\" text:\"assault rifle\"");
	EXPECT_EQ(Lower("'single quoted'"), "text:\"single quoted\"");
	EXPECT_EQ(Lower("камінь 漢字"), "text:\"камінь\" text:\"漢字\"");
	EXPECT_EQ(Lower("\"say \\\"hi\\\"\""), "text:\"say \"hi\"\"");
	EXPECT_EQ(Lower("T_Rock /Project/Textures/T_Rock.png"), "text:\"T_Rock\" text:\"/Project/Textures/T_Rock.png\"");
	// Keywords are text once quoted.
	EXPECT_EQ(Lower("\"or\" \"rows\""), "text:\"or\" text:\"rows\"");
}

TEST(AssetQueryParseTest, TypesWithAndWithoutDerivedTypes)
{
	EXPECT_EQ(Lower("type:WeaponData"), "type:WeaponData");
	EXPECT_EQ(Lower("t:WeaponData"), "type:WeaponData");
	EXPECT_EQ(Lower("type=Game.WeaponData"), "type=Game.WeaponData");
	EXPECT_EQ(Lower("TYPE:Texture"), "type:Texture");
	// Two types in one AND list form the type component: either matches (FARFilter).
	EXPECT_EQ(Lower("type:Texture type:Model"), "type:Texture type:Model");
}

TEST(AssetQueryParseTest, FoldersLabelsAndNegation)
{
	EXPECT_EQ(Lower("dir:/Project/Data"), "dir:/Project/Data");
	EXPECT_EQ(Lower("dir:/Project/Data/"), "dir:/Project/Data");
	EXPECT_EQ(Lower("dir=/Project/Data"), "dir=/Project/Data");
	EXPECT_EQ(Lower("dir:\"/Project/Мої текстури\""), "dir:/Project/Мої текстури");
	EXPECT_EQ(Lower("dir:/Engine"), "dir:/Engine");
	EXPECT_EQ(Lower("label:Boss -label:Debug"), "label:Boss -label:Debug");
	EXPECT_EQ(Lower("label:\"Boss Fight\""), "label:Boss Fight");
}

TEST(AssetQueryParseTest, TypedComparisonsAndSetMembership)
{
	EXPECT_EQ(Lower("Damage>=20 Rarity=Epic Name:Rif"), "where:Damage Ge 20 where:Rarity Eq Epic where:Name Match Rif");
	EXPECT_EQ(Lower("Width>4096 Width<8192 Height<=10 Height!=3"),
		"where:Width Gt 4096 where:Width Lt 8192 where:Height Le 10 where:Height Ne 3");
	EXPECT_EQ(Lower("Rarity in (Rare,Epic)"), "where:Rarity In (Rare,Epic)");
	EXPECT_EQ(Lower("Rarity IN ( Rare , 'Very Epic' ,x)"), "where:Rarity In (Rare,'Very Epic',x)");
	EXPECT_EQ(Lower("$Size>1000 $Type:Texture"), "where:$Size Gt 1000 where:$Type Match Texture");
	EXPECT_EQ(Lower("Offset>-5 Scale=1.5e3"), "where:Offset Gt -5 where:Scale Eq 1.5e3");
	// Quoted literals stay text: compile never makes '1' a number.
	EXPECT_EQ(Lower("Code='1' Name:\"A B\""), "where:Code Eq '1' where:Name Match 'A B'");

	const std::optional<SAssetQuery> query = SAssetQuery::Parse("Code='1' Count=1");
	ASSERT_TRUE(query.has_value());
	ASSERT_EQ(query->Predicates.size(), 2u);
	EXPECT_TRUE(query->Predicates[0].Values[0].IsQuotedLiteral());
	EXPECT_EQ(query->Predicates[0].Values[0].Text, "1");
	EXPECT_TRUE(query->Predicates[1].Values[0].IsLiteral());
	EXPECT_FALSE(query->Predicates[1].Values[0].IsQuotedLiteral());
	EXPECT_EQ(query->Predicates[1].Values[0].Type, ETagType::None);
}

TEST(AssetQueryParseTest, RowsTablesAndGraphTerms)
{
	EXPECT_EQ(Lower("rows table:Ammo"), "records:Rows table:Ammo");
	EXPECT_EQ(Lower("table:Ammo"), "records:Rows table:Ammo");
	EXPECT_EQ(Lower("rows type:AmmoRow"), "type:AmmoRow records:Rows");
	EXPECT_EQ(Lower("ref:T_Rock"), "ref:T_Rock");
	EXPECT_EQ(Lower("ref:~T_Rock"), "ref:~T_Rock");
	EXPECT_EQ(Lower("dep:Hero dep:~/Project/Hero.frt#mesh:Body"), "dep:Hero dep:~/Project/Hero.frt#mesh:Body");
	EXPECT_EQ(Lower("ref:\"/Project/My Icons/rifle.png\""), "ref:/Project/My Icons/rifle.png");
}

TEST(AssetQueryParseTest, SystemPredicatesParseAndThreeWaitForTheirMilestone)
{
	EXPECT_EQ(Lower("is:broken-ref is:unused is:redirected is:unsaved is:primary is:problem is:legacy-ref is:stale-tags "
		"is:pending-meta is:excluded"),
		"is:broken-ref is:unused is:redirected is:unsaved is:primary is:problem is:legacy-ref is:stale-tags "
		"is:pending-meta is:excluded");
	EXPECT_EQ(Lower("is:Broken-Ref"), "is:broken-ref");

	for (uint32 i = 0u; i < SystemPredicateCount; ++i)
	{
		const ESystemPredicate predicate = static_cast<ESystemPredicate>(i);
		EXPECT_EQ(FindSystemPredicate(GetSystemPredicateName(predicate)), std::optional<ESystemPredicate>(predicate));
		const bool bWaits = predicate == ESystemPredicate::Redirected || predicate == ESystemPredicate::Unsaved
			|| predicate == ESystemPredicate::PendingMeta;
		EXPECT_EQ(HasSystemPredicateData(predicate), !bWaits) << GetSystemPredicateName(predicate).CStr();
	}
	EXPECT_FALSE(FindSystemPredicate("broken").has_value());
}

TEST(AssetQueryParseTest, OrderAndLimitApplyToTheWholeQuery)
{
	EXPECT_EQ(Lower("type:Texture order:Width desc limit:50"), "type:Texture order:Width desc limit:50");
	EXPECT_EQ(Lower("order:Name"), "order:Name asc");
	EXPECT_EQ(Lower("order:Name ASC rifle"), "text:\"rifle\" order:Name asc");
	// asc/desc only right after order:; elsewhere they are text.
	EXPECT_EQ(Lower("desc order:Name"), "text:\"desc\" order:Name asc");
	EXPECT_EQ(Lower("rows"), "records:Rows");
	EXPECT_EQ(Lower("limit:1"), "limit:1");
	EXPECT_EQ(Lower("type:A or type:B limit:5"), "any(type:A | type:B) limit:5");
}

TEST(AssetQueryParseTest, SpecExamplesLowerAsDescribed)
{
	EXPECT_EQ(Lower("rows type:EnemyRow Level>10 Biome=Desert order:XP desc"),
		"type:EnemyRow records:Rows where:Level Gt 10 where:Biome Eq Desert order:XP desc");
	EXPECT_EQ(Lower("type:WeaponData ref:~/Project/UI/Icons/rifle.png"), "type:WeaponData ref:~/Project/UI/Icons/rifle.png");
	EXPECT_EQ(Lower("type:Texture Width>=4096 -label:Hero"), "type:Texture -label:Hero where:Width Ge 4096");
}


// ---------------- or and parentheses ----------------

TEST(AssetQueryParseTest, OrAndParenthesesBecomeAnyOfGroups)
{
	EXPECT_EQ(Lower("type:Texture or type:Model"), "any(type:Texture | type:Model)");
	EXPECT_EQ(Lower("label:A OR label:B or rifle"), "any(label:A | label:B | text:\"rifle\")");
	EXPECT_EQ(Lower("dir:/Project/Data (label:Boss or Damage>50)"), "dir:/Project/Data any(label:Boss | where:Damage Gt 50)");
	// Parentheses without `or` only group.
	EXPECT_EQ(Lower("(type:Texture Width>1)"), "type:Texture where:Width Gt 1");
	EXPECT_EQ(Lower("((a))"), "text:\"a\"");
	EXPECT_EQ(Lower("(a or b) (c or d)"), "any(text:\"a\" | text:\"b\") any(text:\"c\" | text:\"d\")");
	EXPECT_EQ(Lower("a b or c"), "any(text:\"a\" text:\"b\" | text:\"c\")");
	EXPECT_EQ(Lower("(a or (b c)) d"), "text:\"d\" any(text:\"a\" | text:\"b\" text:\"c\")");
	// A word containing "or" is text; so is `orange`.
	EXPECT_EQ(Lower("orange"), "text:\"orange\"");
}

TEST(AssetQueryParseTest, BlankTextRestrictsNothing)
{
	const std::optional<SAssetQuery> query = SAssetQuery::Parse("   \t ");
	ASSERT_TRUE(query.has_value());
	EXPECT_TRUE(query->IsUnrestricted());
	EXPECT_EQ(query->Describe(), "");
	EXPECT_TRUE(SAssetQuery::Parse("").has_value());
}


// ---------------- The builder ----------------

TEST(AssetQueryParseTest, BuilderMakesTheSameQueryAsTheText)
{
	SAssetQuery built;
	const STagValue rarities[] = { STagValue::FromLiteral("Rare", false), STagValue::FromLiteral("Epic", false) };
	built.OfType("WeaponData")
		.InFolder("/Project/Data")
		.WithLabel("Boss")
		.WithoutLabel("Debug")
		.Where("Damage", ECompare::Ge, STagValue::FromLiteral("20", false))
		.WhereIn("Rarity", rarities)
		.References("T_Rock", true)
		.Is(ESystemPredicate::Primary)
		.Text("rifle")
		.OrderBy("Damage", false)
		.Limit(50u);
	EXPECT_EQ(built.Describe(), Lower("rifle type:WeaponData dir:/Project/Data label:Boss -label:Debug Damage>=20 "
		"Rarity in (Rare,Epic) ref:~T_Rock is:primary order:Damage desc limit:50"));

	SAssetQuery either;
	either.AnyOf({ SAssetQuery().OfType("Texture"), SAssetQuery().OfType("Model") });
	EXPECT_EQ(either.Describe(), Lower("type:Texture or type:Model"));

	SAssetQuery rows;
	rows.RowsOf("Ammo");
	EXPECT_EQ(rows.Describe(), Lower("table:Ammo"));
	EXPECT_EQ(rows.Records, ERecordScope::Rows);

	SAssetQuery typed;
	typed.Where("Width", ECompare::Ge, STagValue::FromInt64(4096)).Where("Name", ECompare::Exists).ReferencedBy("Hero");
	EXPECT_EQ(typed.Describe(), "where:Width Ge 4096 where:Name Exists dep:Hero");
	EXPECT_FALSE(typed.IsUnrestricted());
}

TEST(AssetQueryParseTest, TermsRecordTheirColumns)
{
	const std::optional<SAssetQuery> query = SAssetQuery::Parse("type:Texture  Width>=4096 (ref:~X or -label:Ä) камінь Y");
	ASSERT_TRUE(query.has_value());
	ASSERT_EQ(query->Types.size(), 1u);
	EXPECT_EQ(query->Types[0].Column, 1u);
	ASSERT_EQ(query->Predicates.size(), 1u);
	EXPECT_EQ(query->Predicates[0].Column, 15u);
	ASSERT_EQ(query->AnyOfGroups.size(), 1u);
	ASSERT_EQ(query->AnyOfGroups[0].Alternatives.size(), 2u);
	EXPECT_EQ(query->AnyOfGroups[0].Alternatives[0].Graph[0].Column, 28u);
	EXPECT_EQ(query->AnyOfGroups[0].Alternatives[1].Labels[0].Column, 38u);
	ASSERT_EQ(query->TextTerms.size(), 2u);
	// "-label:Ä)" is 10 bytes ('Ä' takes two) plus the space: the Cyrillic word starts at byte 49, and its 12 bytes
	// and a space put Y at byte 62.
	EXPECT_EQ(query->TextTerms[0].Column, 49u);
	EXPECT_EQ(query->TextTerms[1].Column, 62u);
}


// ---------------- Errors ----------------

TEST(AssetQueryParseTest, ErrorsPointAtTheTermAtFault)
{
	SDiagnostic error = ErrorOf("type:Texture \"unclosed");
	EXPECT_EQ(error.Column, 14u);
	EXPECT_TRUE(error.Message.Contains("quote")) << error.Message.CStr();

	error = ErrorOf("a (b or c");
	EXPECT_EQ(error.Column, 3u);
	EXPECT_TRUE(error.Message.Contains("'('")) << error.Message.CStr();

	error = ErrorOf("a b)");
	EXPECT_EQ(error.Column, 4u);
	EXPECT_TRUE(error.Message.Contains("')'")) << error.Message.CStr();

	error = ErrorOf("()");
	EXPECT_EQ(error.Column, 1u);

	error = ErrorOf("-type:Texture");
	EXPECT_EQ(error.Column, 1u);
	EXPECT_TRUE(error.Message.Contains("label")) << error.Message.CStr();
	ErrorOf("-rifle");

	error = ErrorOf("type:Texture limit:0");
	EXPECT_EQ(error.Column, 20u);
	ErrorOf("limit:abc");
	ErrorOf("limit:-1");
	ErrorOf("limit:99999999999");
	ErrorOf("limit:'5'");

	error = ErrorOf("order:A order:B");
	EXPECT_EQ(error.Column, 9u);
	EXPECT_TRUE(error.Message.Contains("twice")) << error.Message.CStr();
	ErrorOf("limit:1 limit:2");

	error = ErrorOf("is:nope");
	EXPECT_EQ(error.Column, 4u);
	EXPECT_TRUE(error.Message.Contains("broken-ref")) << error.Message.CStr();

	error = ErrorOf("dir:Project/Data");
	EXPECT_EQ(error.Column, 5u);
	ErrorOf("dir:/Project/../Engine");
	ErrorOf("dir:/");

	error = ErrorOf("Rarity in ()");
	EXPECT_EQ(error.Column, 11u);
	ErrorOf("Rarity in (a b)");
	ErrorOf("Rarity in (a,");
	ErrorOf("type in (A,B)");

	error = ErrorOf("Damage>=");
	EXPECT_EQ(error.Column, 7u);
	EXPECT_TRUE(error.Message.Contains(">=")) << error.Message.CStr();
	ErrorOf("Name:");
	ErrorOf("type:");
	ErrorOf("ref:~");

	error = ErrorOf("(type:A order:B)");
	EXPECT_EQ(error.Column, 9u);
	ErrorOf("(rows)");
	ErrorOf("(limit:4)");

	error = ErrorOf("or type:A");
	EXPECT_EQ(error.Column, 1u);
	error = ErrorOf("type:A or");
	EXPECT_EQ(error.Column, 10u);
	ErrorOf("type:A or or type:B");
	error = ErrorOf("order:X or type:A");
	EXPECT_EQ(error.Column, 1u);
	ErrorOf("type:A or order:X");

	error = ErrorOf("ref=X");
	EXPECT_EQ(error.Column, 4u);
	ErrorOf("is=primary");
	ErrorOf("type>Texture");
	ErrorOf("label<A");

	error = ErrorOf("\"\"");
	EXPECT_TRUE(error.Message.Contains("empty")) << error.Message.CStr();
	error = ErrorOf("Name:\"x\"y");
	EXPECT_EQ(error.Column, 9u);

	FString deep;
	for (uint32 i = 0u; i < 40u; ++i)
	{
		deep += '(';
	}
	deep += 'a';
	for (uint32 i = 0u; i < 40u; ++i)
	{
		deep += ')';
	}
	error = ErrorOf(deep.View());
	EXPECT_TRUE(error.Message.Contains("deeper")) << error.Message.CStr();
}


// ---------------- Fuzz ----------------

TEST(AssetQueryParseTest, RandomTokenStreamsNeverCrashAndEveryErrorHasAPosition)
{
	static constexpr const char* tokens[] = {
		"type:", "t:", "type=", "dir:", "dir=", "/Project", "/Project/Data", "label:", "-label:", "-", "table:", "rows",
		"ref:", "ref:~", "dep:", "is:", "broken-ref", "pending-meta", "order:", "limit:", "50", "0", "desc", "asc", "or",
		"OR", "in", "(", ")", ",", "\"", "'", "\\", "~", "Damage", ">=", "<", "!=", "=", ":", "Texture", "rifle",
		"камінь", "😀", "$Size", "1.5", "-5", "\xFF", "\xC3", "#", "mesh:Body", "\t", "\n",
	};
	std::mt19937 random(8675309u);
	uint32 parsed = 0u;
	uint32 failed = 0u;
	for (uint32 iteration = 0u; iteration < 20000u; ++iteration)
	{
		FString text;
		const uint32 count = random() % 12u;
		for (uint32 i = 0u; i < count; ++i)
		{
			text += tokens[random() % std::size(tokens)];
			if (random() % 3u != 0u)
			{
				text += ' ';
			}
		}
		SDiagnostic error;
		const std::optional<SAssetQuery> query = SAssetQuery::Parse(text.View(), &error);
		if (query)
		{
			++parsed;
			(void)query->Describe();
		}
		else
		{
			++failed;
			ASSERT_EQ(error.Line, 1u) << text.CStr();
			ASSERT_GE(error.Column, 1u) << text.CStr();
			ASSERT_LE(error.Column, text.Len() + 1u) << text.CStr();
			ASSERT_FALSE(error.Message.IsEmpty()) << text.CStr();
		}
	}
	// Both outcomes occur, so the stream exercises the grammar and its errors.
	EXPECT_GT(parsed, 1000u);
	EXPECT_GT(failed, 1000u);
}
