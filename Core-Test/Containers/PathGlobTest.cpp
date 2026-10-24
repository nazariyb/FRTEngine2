// CPathGlob: the asset rules' path patterns (AssetManager-Design §3.5.2). '*' and '?' stay inside one
// segment, '**' spans segments, anchoring follows gitignore, case folds the same on every host, and
// a malformed pattern is an error with a reason, never a guess.
#include <gtest/gtest.h>

#include <optional>
#include <span>

#include "Containers/PathGlob.h"
#include "Containers/String.h"

using namespace frt;


namespace
{
struct SGlobCase
{
	FStringLiteral Pattern;
	FStringLiteral Path;
	bool bMatches = false;
};

void ExpectCases (std::span<const SGlobCase> Cases)
{
	for (const SGlobCase& entry : Cases)
	{
		FString error;
		const std::optional<CPathGlob> glob = CPathGlob::Compile(entry.Pattern, &error);
		ASSERT_TRUE(glob.has_value()) << "pattern \"" << entry.Pattern.CStr() << "\": " << error.CStr();
		EXPECT_EQ(glob->Matches(entry.Path), entry.bMatches)
			<< "pattern \"" << entry.Pattern.CStr() << "\", path \"" << entry.Path.CStr() << "\"";
	}
}

// The examples of the gitignore documentation (git-scm.com/docs/gitignore, "PATTERN FORMAT" and
// "EXAMPLES"), read as patterns over file paths relative to the folder of the rule file.
constexpr SGlobCase GitignoreCases[] = {
	// No '/': the name at any depth.
	{ "*.log", "debug.log", true },
	{ "*.log", "foo.log", true },
	{ "*.log", ".log", true },
	{ "*.log", "logs/debug.log", true },
	{ "*.log", "debug.log.txt", false },
	{ "debug.log", "debug.log", true },
	{ "debug.log", "logs/debug.log", true },
	{ "debug.log", "logs/debug.log/x", false },
	// A leading '/' anchors to the base.
	{ "/debug.log", "debug.log", true },
	{ "/debug.log", "logs/debug.log", false },
	// A '/' in the middle anchors as well.
	{ "logs/debug.log", "logs/debug.log", true },
	{ "logs/debug.log", "debug.log", false },
	{ "logs/debug.log", "build/logs/debug.log", false },
	{ "doc/frotz/", "doc/frotz/a.txt", true },
	{ "doc/frotz/", "a/doc/frotz/a.txt", false },
	// A trailing '/' alone floats, and takes everything below the folder.
	{ "logs/", "logs/debug.log", true },
	{ "logs/", "logs/latest/foo.bar", true },
	{ "logs/", "build/logs/foo.bar", true },
	{ "logs/", "build/logs/latest/debug.log", true },
	{ "logs/", "logs", false },
	{ "frotz/", "a/frotz/b.txt", true },
	// '**'.
	{ "**/logs/debug.log", "logs/debug.log", true },
	{ "**/logs/debug.log", "build/logs/debug.log", true },
	{ "**/logs/debug.log", "logs/build/debug.log", false },
	{ "logs/**/debug.log", "logs/debug.log", true },
	{ "logs/**/debug.log", "logs/monday/debug.log", true },
	{ "logs/**/debug.log", "logs/monday/pm/debug.log", true },
	{ "logs/**/debug.log", "build/logs/debug.log", false },
	{ "a/**/b", "a/b", true },
	{ "a/**/b", "a/x/b", true },
	{ "a/**/b", "a/x/y/b", true },
	{ "a/**/b", "a/bb", false },
	{ "a/**/b", "ab", false },
	{ "abc/**", "abc/x", true },
	{ "abc/**", "abc/x/y", true },
	{ "abc/**", "abc", false },
	{ "abc/**", "x/abc/y", false },
	{ "**", "a", true },
	{ "**", "a/b/c", true },
	// '*' inside a segment.
	{ "logs/*day/debug.log", "logs/monday/debug.log", true },
	{ "logs/*day/debug.log", "logs/tuesday/debug.log", true },
	{ "logs/*day/debug.log", "logs/latest/debug.log", false },
	{ "logs/*day/debug.log", "logs/mon/day/debug.log", false },
	// '?' and sets.
	{ "debug?.log", "debug0.log", true },
	{ "debug?.log", "debugg.log", true },
	{ "debug?.log", "debug10.log", false },
	{ "debug?.log", "debug.log", false },
	{ "debug[0-9].log", "debug0.log", true },
	{ "debug[0-9].log", "debug1.log", true },
	{ "debug[0-9].log", "debug10.log", false },
	{ "debug[01].log", "debug0.log", true },
	{ "debug[01].log", "debug1.log", true },
	{ "debug[01].log", "debug2.log", false },
	{ "debug[!01].log", "debug2.log", true },
	{ "debug[!01].log", "debug0.log", false },
	{ "debug[!01].log", "debug01.log", false },
	{ "debug[a-z].log", "debuga.log", true },
	{ "debug[a-z].log", "debugb.log", true },
	{ "debug[a-z].log", "debug1.log", false },
};

// '*' never crosses a '/' (FStringView::MatchesWildcard's does), and the forms gitignore lacks.
constexpr SGlobCase SegmentCases[] = {
	{ "Textures/*.png", "Textures/A.png", true },
	{ "Textures/*.png", "Textures/Old/A.png", false },
	{ "Textures/*.png", "A.png", false },
	{ "*", "A.png", true },
	{ "*", "Art/Ui/A.png", true },
	{ "/*", "A.png", true },
	{ "/*", "Art/A.png", false },
	{ "/**", "Art/A.png", true },
	{ "a?b", "a/b", false },
	{ "a*b", "a/b", false },
	{ "a/*/b", "a/x/y/b", false },
	{ "**/**/a", "a", true },
	{ "a/**/**/b", "a/b", true },
	// Braces: alternatives expand first, each a pattern of its own.
	{ "*.{png,psd}", "a.png", true },
	{ "*.{png,psd}", "Art/b.psd", true },
	{ "*.{png,psd}", "a.tga", false },
	{ "*.{png,psd}", "a.png.bak", false },
	{ "{Textures,Models}/**", "Textures/a.png", true },
	{ "{Textures,Models}/**", "Models/x/y.gltf", true },
	{ "{Textures,Models}/**", "Sounds/a.wav", false },
	{ "{*.png,Ui/*.tga}", "Art/b.png", true },
	{ "{*.png,Ui/*.tga}", "Ui/a.tga", true },
	{ "{*.png,Ui/*.tga}", "Art/Ui/a.tga", false },
	{ "{a,{b,c}d}.txt", "a.txt", true },
	{ "{a,{b,c}d}.txt", "bd.txt", true },
	{ "{a,{b,c}d}.txt", "cd.txt", true },
	{ "{a,{b,c}d}.txt", "b.txt", false },
	{ "Tex{,_old}.png", "Tex.png", true },
	{ "Tex{,_old}.png", "Tex_old.png", true },
	{ "Tex{,_old}.png", "Tex_new.png", false },
	{ "{[},]x,y}", "}x", true },
	{ "{[},]x,y}", ",x", true },
	{ "{[},]x,y}", "y", true },
	{ "a,b.png", "a,b.png", true },
	// Escapes and the set forms that spell special characters.
	{ "\\[1\\].png", "[1].png", true },
	{ "\\[1\\].png", "1.png", false },
	{ "\\*.png", "*.png", true },
	{ "\\*.png", "a.png", false },
	{ "\\{a,b\\}", "{a,b}", true },
	{ "[[]1].png", "[1].png", true },
	{ "[]a]", "]", true },
	{ "[]a]", "a", true },
	{ "[]a]", "b", false },
	{ "[a-]x", "-x", true },
	{ "[a-]x", "ax", true },
	{ "[a-]x", "bx", false },
	{ "[^a]", "b", true },
	{ "[^a]", "a", false },
	{ "[\\]]", "]", true },
	// '?' is one character, however many bytes it takes.
	{ "?.png", "ї.png", true },
	{ "?.png", "游.png", true },
	{ "?.png", "😀.png", true },
	{ "?.png", "ab.png", false },
	{ "??.png", "ї.png", false },
};

// Unicode simple case folding: "Ä" and "ä", "Σ", "σ" and "ς" (which all fold to σ), U+212A KELVIN
// SIGN and 'k'. Sets fold too.
constexpr SGlobCase CaseCases[] = {
	{ "*.PNG", "a.png", true },
	{ "textures/", "Textures/A.png", true },
	{ "TEXTURES/*.png", "textures/a.PNG", true },
	{ "Ä*.png", "äpfel.png", true },
	{ "Ä*.png", "ÄPFEL.PNG", true },
	{ "ä*.png", "Äpfel.png", true },
	{ "Ä*.png", "apfel.png", false },
	{ "Σ*", "σ.txt", true },
	{ "Σ*", "ς.txt", true },
	{ "Σ*", "Σ.txt", true },
	{ "*ς", "ΟΔΟΣ", true },
	{ "*σ", "οδος", true },    // "οδος", ending in final sigma
	{ "*σ", "οδοι", false },   // "οδοι"
	{ "Текстури/**", "текстури/Цегла.png", true },
	{ "ЦЕГЛА.png", "Текстури/цегла.png", true },
	{ "k*", "Kelvin", true },
	{ "K*", "kelvin", true },
	{ "[A-Z].txt", "q.txt", true },
	{ "[a-z].txt", "Q.txt", true },
	{ "[j-l]", "K", true },
	{ "[!a-c]", "d", true },
	{ "[!a-c]", "B", false },
	{ "[α-ω]", "Σ", true },
	{ "[α-ω]", "ς", true },
	{ "[Ä]", "ä", true },
	{ "[ -\U0010FFFF]", "Ä", true },
	{ "[ -\U0010FFFF]", "z", true },
	{ "[!ä]", "Ä", false },
	// Ill-formed bytes in a path: one character, which '?', '*' and negated sets take and no literal
	// and no set member does.
	{ "a?.png", "a\xFF.png", true },
	{ "a*.png", "a\xFF\xFE.png", true },
	{ "a[!x].png", "a\xFF.png", true },
	{ "a[x].png", "a\xFF.png", false },
	{ "aÿ.png", "a\xFF.png", false },
};
}


TEST(PathGlob, GitignoreExamplesMatchAsDocumented)
{
	ExpectCases(GitignoreCases);
}

TEST(PathGlob, WildcardsStayInsideOneSegment)
{
	ExpectCases(SegmentCases);
}

TEST(PathGlob, CaseFoldsUnderUnicodeSimpleCaseFolding)
{
	ExpectCases(CaseCases);
}

TEST(PathGlob, NameOnlyPatternsMatchFileNamesNotFolders)
{
	// gitignore's "Textures" would also take everything below a folder of that name; a file
	// pattern takes files, and "Textures/" says folder.
	const std::optional<CPathGlob> name = CPathGlob::Compile("Textures");
	ASSERT_TRUE(name);
	EXPECT_TRUE(name->Matches("Textures"));
	EXPECT_TRUE(name->Matches("Art/Textures"));
	EXPECT_FALSE(name->Matches("Textures/A.png"));

	const std::optional<CPathGlob> folder = CPathGlob::Compile("Textures/");
	ASSERT_TRUE(folder);
	EXPECT_FALSE(folder->Matches("Textures"));
	EXPECT_TRUE(folder->Matches("Textures/A.png"));
	EXPECT_TRUE(folder->Matches("Art/Textures/A.png"));
}

TEST(PathGlob, InvalidPatternsAreReportedWithAReason)
{
	struct SInvalid
	{
		FStringLiteral Pattern;
		FStringLiteral ErrorPart;
	};
	constexpr SInvalid Invalid[] = {
		{ "", "empty" },
		{ "a\xFF", "UTF-8" },
		{ "[abc", "'[' at character 1 is never closed" },
		{ "x/a[]", "'[' at character 4 is never closed" },
		{ "Ä[z-a]", "range at character 3" },
		{ "[a/b]", "path separator" },
		{ "{a,b", "'{' at character 1 is never closed" },
		{ "ä}", "'}' at character 2 closes no '{'" },
		{ "a\\", "escapes nothing" },
		{ "a\\/b", "cannot be escaped" },
		{ "a//b", "empty path segment" },
		{ "/", "names no path" },
		{ "a/", "" },
		{ "a**", "whole path segment" },
		{ "**b/c", "whole path segment" },
		{ "../a", "'..' segment" },
		{ "a/./b", "'.' segment" },
		{ "{,}", "names no path" },
		{ "{a,b,c,d}{a,b,c,d}{a,b,c,d}{a,b,c,d}{a,b,c,d}{a,b,c,d}", "more than 1024" },
		{ "{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{a}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}", "deeper than 32" },
	};

	for (const SInvalid& entry : Invalid)
	{
		FString error;
		const std::optional<CPathGlob> glob = CPathGlob::Compile(entry.Pattern, &error);
		if (entry.ErrorPart.IsEmpty())
		{
			// The control: valid, and so a pattern the table did not mean to reject.
			EXPECT_TRUE(glob) << entry.Pattern.CStr() << ": " << error.CStr();
			continue;
		}
		EXPECT_FALSE(glob) << entry.Pattern.CStr();
		EXPECT_TRUE(error.Contains(entry.ErrorPart)) << entry.Pattern.CStr() << ": \"" << error.CStr() << "\"";
	}

	// Without an error text to fill, still nullopt.
	EXPECT_FALSE(CPathGlob::Compile("[abc"));
}

TEST(PathGlob, MatchesBelowAnchorsToTheFolderOfTheRuleFile)
{
	// Content/Environment/Foliage/Assets.yml, "Scans/**" (AssetManager-Design §3.5.4).
	const std::optional<CPathGlob> scans = CPathGlob::Compile("Scans/**");
	ASSERT_TRUE(scans);
	EXPECT_TRUE(scans->MatchesBelow("Environment/Foliage", "Environment/Foliage/Scans/a.png"));
	EXPECT_TRUE(scans->MatchesBelow("Environment/Foliage", "environment/FOLIAGE/scans/x/A.PNG"));
	EXPECT_TRUE(scans->MatchesBelow("Environment/Foliage/", "Environment/Foliage/Scans/a.png"));
	EXPECT_FALSE(scans->MatchesBelow("Environment/Foliage", "Environment/Scans/a.png"));
	EXPECT_FALSE(scans->MatchesBelow("Environment/Foliage", "Environment/FoliageX/Scans/a.png"));
	EXPECT_FALSE(scans->MatchesBelow("Environment/Foliage", "Environment/Foliage"));
	EXPECT_FALSE(scans->MatchesBelow("Environment/Foliage", "Scans/a.png"));

	// A name-only pattern takes the name anywhere below the folder, and nowhere else.
	const std::optional<CPathGlob> images = CPathGlob::Compile("*.{png,psd}");
	ASSERT_TRUE(images);
	EXPECT_TRUE(images->MatchesBelow("Environment/Foliage", "Environment/Foliage/a.png"));
	EXPECT_TRUE(images->MatchesBelow("Environment/Foliage", "Environment/Foliage/Trees/Oak/b.psd"));
	EXPECT_FALSE(images->MatchesBelow("Environment/Foliage", "Environment/a.png"));
	EXPECT_TRUE(images->MatchesBelow("", "Environment/a.png"));

	// The base folds like the pattern: "Ä" is the base of "ä/...".
	const std::optional<CPathGlob> any = CPathGlob::Compile("**");
	ASSERT_TRUE(any);
	EXPECT_TRUE(any->MatchesBelow("Äpfel", "äpfel/a.png"));
	EXPECT_TRUE(any->MatchesBelow("K", "k/a.png"));
}

TEST(PathGlob, CopiesMatchAndAnEmptyGlobMatchesNothing)
{
	const CPathGlob empty;
	EXPECT_FALSE(empty.Matches("a.png"));
	EXPECT_TRUE(empty.GetPattern().IsEmpty());

	std::optional<CPathGlob> original = CPathGlob::Compile("Art/**/*.[pP][nN][gG]");
	ASSERT_TRUE(original);
	const CPathGlob copy = *original;
	original.reset();
	EXPECT_EQ(copy.GetPattern(), "Art/**/*.[pP][nN][gG]");
	EXPECT_TRUE(copy.Matches("Art/Ui/Icons/a.png"));
	EXPECT_FALSE(copy.Matches("Ui/a.png"));
}
