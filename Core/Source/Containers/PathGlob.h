#pragma once

#include <optional>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt
{
// A compiled path pattern: the globs of the asset rules (`Assets.yml` Scan lists and `Match:`
// selectors, AssetManager-Design §3.5.2) and anything else that selects files by path.
//
// FStringView::MatchesWildcard is a text wildcard: its '*' crosses '/', so "Textures/*.png" also
// takes "Textures/Old/A.png", and it knows no case rule a file system would agree with. The
// syntax here is the one gitignore, EditorConfig and most build tools share, segment-aware:
//
//	- '/' separates segments; backslash is no separator.
//	- '*' is any run of characters inside one segment (never a '/'); '?' is one character.
//	- '**' as a whole segment is any number of segments, zero included ("a/**/b" takes "a/b"), except
//	  at the end, where it is everything below ("a/**" takes "a/x" and "a/x/y", never "a" itself).
//	  Inside a segment ("a**") it is an error: gitignore reads it as '*', but whoever writes it
//	  almost always meant the crossing kind.
//	- "[abc]", "[0-9]", "[!abc]" or "[^abc]": one character from the set, or not from it. A ']' first
//	  in the set is a member; '-' first or last is a member. A set never takes '/'.
//	- "{a,b}": alternatives, which may nest, hold wildcards and '/', or be empty ("Tex{,_old}.png").
//	  They expand first, as if each alternative were a pattern of its own (bash brace expansion), so
//	  the rules below apply per alternative.
//	- '\' takes the next character literally ("\[1\].png"); a '/' cannot be escaped.
//
// Anchoring follows gitignore. A pattern relative to a base (a mount, or the folder of a folder
// rule file) matches the whole path below that base, never a part of it. A pattern with no '/'
// other than a trailing one floats: "*.png" takes "A.png" and "Art/Ui/A.png". A pattern with a '/'
// at its start or in its middle is anchored to the base: "Ui/*.png" takes "Ui/A.png", not
// "Art/Ui/A.png"; a leading '/' just anchors ("/A.png" is "A.png" at the base only). Mount prefixes
// such as "/Project/" are the rules loader's to resolve (it picks the base); here a leading '/' is
// the base.
//
// Folders follow gitignore too, where git never looks inside an ignored folder: a pattern that
// matches a folder on the way takes everything below it. "Temp" takes a file named Temp at any
// depth and every file below a folder named Temp ("Temp/A.png", "Art/Temp/Old/B.png"), which is
// what "Scan.Exclude: [ Temp ]" and the built-in dot-name exclude mean; "/Docs" takes everything
// below the base's Docs; "Ui/*" takes "Ui/Old/A.png" as well as "Ui/A.png", because the folder
// "Ui/Old" matches it. A trailing '/' names a folder only: "Scans/" takes everything below any
// folder named Scans ("**/Scans/**"), never a file named Scans. CoversFolder asks the same of a
// whole folder, so a scanner can skip an excluded subtree without listing it.
//
// Case: characters compare under Unicode simple case folding from FRT's own tables (the
// TStringMapIgnoreCase rule, Containers/StringFwd.h), so "Ä*" takes "äpfel.png" and "Σ" takes "σ"
// and "ς". The answer never depends on the host's file system or locale, which is what lets
// effective settings and DDC keys be the same on every machine (AssetManager-Design §5.2). There
// is no Unicode normalisation: "é" precomposed and decomposed are different names, as they are on
// NTFS. A set compares by folding as well: "[a-z]" takes 'Q', and a range takes every character
// whose folding is the folding of one of its members.
//
// Invalid patterns are reported, never guessed at: Compile returns nullopt with the reason (an
// unclosed '[' or '{', a reversed range, an empty segment, '.' or '..' segments, too many brace
// alternatives). Paths must be valid relative paths ('/'-separated, no empty, '.' or '..'
// segments). An ill-formed UTF-8 unit in a path is one character: '?', '*' and negated sets take
// it, and no literal and no member of a set does ("a[!x].png" takes "a\xFF.png", "a[x].png" not).
//
// Uses std::vector, never TArray: rules are evaluated on worker threads and before the memory pool
// exists. A compiled glob is immutable, so any number of threads may match with it at once.
class FRT_CORE_API CPathGlob
{
public:
	// Braces may expand to at most this many patterns.
	static constexpr uint32 MaxAlternatives = 1024u;

	// Matches nothing.
	CPathGlob () = default;

	// nullopt for an invalid pattern, with a one-line reason in *OutError when it is given.
	[[nodiscard]] static std::optional<CPathGlob> Compile (FStringView Pattern, FString* OutError = nullptr);

	// RelativePath: relative to the pattern's base, '/'-separated, without a leading '/'.
	[[nodiscard]] bool Matches (FStringView RelativePath) const noexcept;
	// Base: the folder the pattern is relative to, relative to the same root as Path ('/'-separated,
	// no leading or trailing '/'; empty is the root). False when Path is not below Base; Base is
	// compared under the same case folding as the pattern.
	[[nodiscard]] bool MatchesBelow (FStringView Base, FStringView Path) const noexcept;

	// True when every path below Folder matches, so a scanner may skip the folder without listing
	// it: Folder or a folder above it matches, or the pattern ends in a '*' segment or a globstar
	// that takes everything below Folder ("Docs/**" and "Docs/*" cover "Docs"). Folder is relative
	// to the base like a path for Matches; empty is the base itself ("**" and "*" cover it). False
	// can still mean every path below matches when only several brace alternatives together take
	// it all ("a/{x*,[!x]*}" for "a"); a scanner then lists the folder and matches each entry,
	// which gives the same answer, only slower.
	[[nodiscard]] bool CoversFolder (FStringView Folder) const noexcept;
	// CoversFolder for a folder relative to the same root as Base (as in MatchesBelow); Folder may
	// be Base itself. False when Folder is neither Base nor below it.
	[[nodiscard]] bool CoversFolderBelow (FStringView Base, FStringView Folder) const noexcept;

	// The text it was compiled from.
	[[nodiscard]] FStringView GetPattern () const noexcept { return Pattern; }

private:
	enum class ETokenKind : uint8
	{
		Literal,   // Value: the folded code point
		AnyChar,   // '?'
		Star,      // '*'
		Set,       // Value: index into Sets
	};

	struct SToken
	{
		ETokenKind Kind = ETokenKind::Literal;
		uint32 Value = 0u;
	};

	// One pattern segment: a globstar, or tokens matched against exactly one path segment.
	struct SSegment
	{
		uint32 FirstToken = 0u;
		uint32 TokenCount = 0u;
		bool bGlobstar = false;
	};

	// One brace alternative, or the folder-rule copy of one: the segments it matches the whole path
	// with. bEverythingBelow: they end in '*' and a globstar, which take every path below what the
	// segments before them match (a trailing "/**", compiled).
	struct SAlternative
	{
		uint32 FirstSegment = 0u;
		uint32 SegmentCount = 0u;
		bool bEverythingBelow = false;
	};

	// A character set: the members' ranges as written, plus the foldings of members that fold to
	// something else. A path character c matches when Fold(c) is in either list (see the .cpp).
	struct SCharSet
	{
		uint32 FirstRange = 0u;
		uint32 RangeCount = 0u;
		uint32 FirstFolded = 0u;
		uint32 FoldedCount = 0u;
		bool bNegated = false;
	};

	struct SRange
	{
		char32_t First = 0u;
		char32_t Last = 0u;
	};

	friend class CPathGlobCompiler;

	// Path relative to Base (empty when Path is Base itself), or nullopt when it is not below it.
	[[nodiscard]] static std::optional<FStringView> RelativeToBase (FStringView Base, FStringView Path) noexcept;
	[[nodiscard]] bool MatchAlternative (const SAlternative& Alternative, FStringView Path) const noexcept;
	[[nodiscard]] bool MatchSegment (const SSegment& Segment, FStringView Text) const noexcept;
	[[nodiscard]] bool SetContains (const SCharSet& Set, uint32 FoldedKey) const noexcept;

	FString Pattern;
	std::vector<SAlternative> Alternatives;
	std::vector<SSegment> Segments;
	std::vector<SToken> Tokens;
	std::vector<SCharSet> Sets;
	std::vector<SRange> Ranges;
	std::vector<char32_t> Folded;
};
}
