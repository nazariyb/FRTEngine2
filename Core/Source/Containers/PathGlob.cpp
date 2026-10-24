#include "Containers/PathGlob.h"

#include <algorithm>
#include <utility>

#include "Unicode/Ascii.h"
#include "Unicode/UnicodeTablesInternal.h"


namespace
{
using namespace frt;

constexpr uint32 NotFound = FStringView::NotFound;
// The key of an ill-formed path unit: never a code point, so no literal and no set member has it.
constexpr uint32 IllFormedKey = 0xFFFFFFFFu;
// Deeper nesting is surely a mistake, and it bounds the expander's recursion.
constexpr uint32 MaxBraceDepth = 32u;

unicode::SDecodedCodePoint DecodeAt (FStringView Text, uint32 Offset) noexcept
{
	return unicode::DecodeUtf8(Text.GetData() + Offset, Text.Len() - Offset);
}

// Simple case folding (unicode::FoldCase) over the inline table lookup.
char32_t Fold (char32_t C) noexcept
{
	return unicode::tables::LookupFoldCase(C);
}

// The folded key of the path unit at Offset, and its length.
struct SUnitKey
{
	uint32 Key = IllFormedKey;
	uint32 Length = 1u;
};

SUnitKey ReadKey (FStringView Text, uint32 Offset) noexcept
{
	const char byte = Text.GetData()[Offset];
	if (ascii::IsAscii(byte))
	{
		return { static_cast<unsigned char>(ascii::ToLower(byte)), 1u };
	}

	const unicode::SDecodedCodePoint unit = DecodeAt(Text, Offset);
	return { unit.bWellFormed ? static_cast<uint32>(Fold(unit.CodePoint)) : IllFormedKey, unit.Length };
}

// The end of the path segment starting at Start: the next '/', or the end of the path.
uint32 FindSegmentEnd (FStringView Path, uint32 Start) noexcept
{
	const uint32 slash = Path.Find('/', Start);
	return slash == NotFound ? Path.Len() : slash;
}

// 1-based, in characters: what an error message calls a position.
uint32 CharacterNumber (FStringView Text, uint32 Offset) noexcept
{
	return Text.Left(Offset).CountCodePoints() + 1u;
}

// Every code point whose simple case folding is another code point, ascending: what a set's ranges
// fold to besides themselves. About 1,500 of them; found once per process, by asking the tables
// about every code point (a few milliseconds), so they can never disagree with Fold.
const std::vector<char32_t>& GetCodePointsThatFold ()
{
	static const std::vector<char32_t> codePoints = []
	{
		std::vector<char32_t> result;
		for (char32_t c = 0u; c <= unicode::MaxCodePoint; ++c)
		{
			if (Fold(c) != c)
			{
				result.push_back(c);
			}
		}
		return result;
	}();
	return codePoints;
}


// "[...]" at Text[Begin] (a '['), as written: its members (single characters are one-character
// ranges, escapes resolved), whether it is negated, and the offset after its ']'.
struct SParsedSet
{
	std::vector<std::pair<char32_t, char32_t>> Ranges;
	uint32 End = 0u;
	bool bNegated = false;
};

bool ParseSet (FStringView Text, uint32 Begin, SParsedSet& Out, FString* OutError)
{
	const auto fail = [&] (FString Message)
	{
		if (OutError)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	const auto unclosed = [&] ()
	{
		return fail(FString::Printf("'[' at character %u is never closed", CharacterNumber(Text, Begin)));
	};

	Out = SParsedSet();
	uint32 i = Begin + 1u;
	if (i < Text.Len() && (Text[i] == '!' || Text[i] == '^'))
	{
		Out.bNegated = true;
		++i;
	}

	// One member at i: a character, or '\' and the character it escapes.
	const auto readMember = [&] (char32_t& OutMember) -> bool
	{
		const uint32 at = i;
		if (Text[i] == '\\')
		{
			if (i + 1u >= Text.Len())
			{
				return unclosed();
			}
			++i;
		}
		const unicode::SDecodedCodePoint unit = DecodeAt(Text, i);
		i += unit.Length;
		OutMember = unit.CodePoint;
		if (OutMember == U'/')
		{
			return fail(FString::Printf("'/' in the set at character %u: a set never takes a path separator",
				CharacterNumber(Text, at)));
		}
		return true;
	};

	for (bool bFirst = true;; bFirst = false)
	{
		if (i >= Text.Len())
		{
			return unclosed();
		}
		if (Text[i] == ']' && !bFirst)
		{
			Out.End = i + 1u;
			return true;
		}

		const uint32 memberStart = i;
		char32_t first = 0u;
		if (!readMember(first))
		{
			return false;
		}
		char32_t last = first;
		// '-' between two members makes a range; first or last in the set it is a member itself.
		if (i + 1u < Text.Len() && Text[i] == '-' && Text[i + 1u] != ']')
		{
			++i;
			if (!readMember(last))
			{
				return false;
			}
			if (last < first)
			{
				return fail(FString::Printf("the range at character %u ends before it starts",
					CharacterNumber(Text, memberStart)));
			}
		}
		Out.Ranges.emplace_back(first, last);
	}
}
}


namespace frt
{
/**
 * Compile in three passes, so every error names the place the writer can find:
 *	1. Validate the pattern as written: UTF-8, escapes, sets, brace balance (positions in it).
 *	2. Expand the braces into alternatives, escapes and sets copied verbatim.
 *	3. Split each alternative into segments, anchor it, and tokenize the segments (an error quotes
 *	   the alternative, since positions in it are not positions in the pattern).
 */
class CPathGlobCompiler
{
public:
	CPathGlobCompiler (CPathGlob& InGlob, FStringView InPattern)
		: Glob(InGlob)
		, Pattern(InPattern)
	{}

	bool Compile ()
	{
		if (!Validate())
		{
			return false;
		}

		uint32 position = 0u;
		std::vector<FString> alternatives;
		if (!Expand(position, 0u, false, alternatives))
		{
			return false;
		}

		for (const FString& alternative : alternatives)
		{
			if (!CompileAlternative(alternative))
			{
				return false;
			}
		}
		return true;
	}

	FString Error;

private:
	bool Fail (FString Message)
	{
		Error = std::move(Message);
		return false;
	}

	bool Validate ()
	{
		if (Pattern.IsEmpty())
		{
			return Fail("the pattern is empty");
		}
		if (!Pattern.IsValidUtf8())
		{
			return Fail("the pattern is not valid UTF-8");
		}

		std::vector<uint32> openBraces;
		for (uint32 i = 0u; i < Pattern.Len();)
		{
			switch (Pattern[i])
			{
			case '\\':
				if (i + 1u >= Pattern.Len())
				{
					return Fail("'\\' at the end of the pattern escapes nothing");
				}
				if (Pattern[i + 1u] == '/')
				{
					return Fail(FString::Printf("'\\/' at character %u: a path separator cannot be escaped",
						CharacterNumber(Pattern, i)));
				}
				i += 1u + DecodeAt(Pattern, i + 1u).Length;
				break;
			case '[':
			{
				SParsedSet set;
				if (!ParseSet(Pattern, i, set, &Error))
				{
					return false;
				}
				i = set.End;
				break;
			}
			case '{':
				if (openBraces.size() >= MaxBraceDepth)
				{
					return Fail(FString::Printf("braces nest deeper than %u", MaxBraceDepth));
				}
				openBraces.push_back(i);
				++i;
				break;
			case '}':
				if (openBraces.empty())
				{
					return Fail(FString::Printf("'}' at character %u closes no '{'", CharacterNumber(Pattern, i)));
				}
				openBraces.pop_back();
				++i;
				break;
			default:
				i += DecodeAt(Pattern, i).Length;
				break;
			}
		}

		if (!openBraces.empty())
		{
			return Fail(FString::Printf("'{' at character %u is never closed",
				CharacterNumber(Pattern, openBraces.back())));
		}
		return true;
	}

	// The patterns Pattern[Position..] spells up to the ',' or '}' that ends the current alternative
	// (inside braces) or to the end. Validate has run, so every set and brace is closed.
	bool Expand (uint32& Position, uint32 Depth, bool bInBraces, std::vector<FString>& Out)
	{
		Out.assign(1u, FString());
		while (Position < Pattern.Len())
		{
			const char c = Pattern[Position];
			if (bInBraces && (c == ',' || c == '}'))
			{
				return true;
			}

			if (c == '{')
			{
				++Position;
				std::vector<FString> choices;
				for (;;)
				{
					std::vector<FString> choice;
					if (!Expand(Position, Depth + 1u, true, choice))
					{
						return false;
					}
					for (FString& text : choice)
					{
						choices.push_back(std::move(text));
					}
					if (choices.size() > CPathGlob::MaxAlternatives)
					{
						return TooManyAlternatives();
					}
					if (Pattern[Position++] == '}')
					{
						break;
					}
				}

				if (Out.size() * choices.size() > CPathGlob::MaxAlternatives)
				{
					return TooManyAlternatives();
				}
				std::vector<FString> product;
				product.reserve(Out.size() * choices.size());
				for (const FString& prefix : Out)
				{
					for (const FString& choice : choices)
					{
						product.push_back(prefix + choice);
					}
				}
				Out = std::move(product);
				continue;
			}

			uint32 next = 0u;
			if (c == '\\')
			{
				next = Position + 1u + DecodeAt(Pattern, Position + 1u).Length;
			}
			else if (c == '[')
			{
				SParsedSet set;
				ParseSet(Pattern, Position, set, nullptr);
				next = set.End;
			}
			else
			{
				next = Position + DecodeAt(Pattern, Position).Length;
			}

			const FStringView piece = Pattern.Mid(Position, next - Position);
			for (FString& text : Out)
			{
				text.Append(piece);
			}
			Position = next;
		}
		return true;
	}

	bool TooManyAlternatives ()
	{
		return Fail(FString::Printf("the braces expand to more than %u patterns", CPathGlob::MaxAlternatives));
	}

	bool CompileAlternative (FStringView Alternative)
	{
		// Segments at every '/' outside escapes and sets (Validate rejected '/' in both).
		const bool bAnchored = Alternative.StartsWith('/');
		const uint32 begin = bAnchored ? 1u : 0u;
		std::vector<FStringView> parts;
		uint32 partStart = begin;
		for (uint32 i = begin; i < Alternative.Len();)
		{
			const char c = Alternative[i];
			if (c == '\\')
			{
				i += 1u + DecodeAt(Alternative, i + 1u).Length;
			}
			else if (c == '[')
			{
				SParsedSet set;
				ParseSet(Alternative, i, set, nullptr);
				i = set.End;
			}
			else if (c == '/')
			{
				parts.push_back(Alternative.Mid(partStart, i - partStart));
				partStart = ++i;
			}
			else
			{
				i += DecodeAt(Alternative, i).Length;
			}
		}

		// A trailing '/' names a folder: everything below it.
		const bool bFolder = partStart == Alternative.Len() && Alternative.Len() > begin;
		if (!bFolder)
		{
			parts.push_back(Alternative.Mid(partStart));
		}

		for (const FStringView part : parts)
		{
			if (part.IsEmpty())
			{
				return Fail(Alternative.Len() <= 1u ? FString("the pattern names no path")
					: FString::Printf("'%s' has an empty path segment", FString(Alternative).CStr()));
			}
			if (part == "." || part == "..")
			{
				return Fail(FString::Printf("'%s' has a '%s' segment: patterns match normalized paths only",
					FString(Alternative).CStr(), FString(part).CStr()));
			}
		}

		CPathGlob::SAlternative alternative;
		alternative.FirstSegment = static_cast<uint32>(Glob.Segments.size());

		// gitignore: no '/' but a trailing one, and the pattern floats to any depth.
		if (!bAnchored && parts.size() == 1u)
		{
			AddGlobstar(alternative.FirstSegment);
		}
		for (const FStringView part : parts)
		{
			if (part == "**")
			{
				AddGlobstar(alternative.FirstSegment);
			}
			else if (!CompileSegment(part, Alternative))
			{
				return false;
			}
		}
		if (bFolder)
		{
			AddGlobstar(alternative.FirstSegment);
		}

		// A trailing globstar is everything below: at least one more segment, the file itself.
		if (Glob.Segments.back().bGlobstar)
		{
			Glob.Segments.pop_back();
			AddEverythingBelow(alternative.FirstSegment);
			alternative.bEverythingBelow = true;
		}
		alternative.SegmentCount = static_cast<uint32>(Glob.Segments.size()) - alternative.FirstSegment;
		Glob.Alternatives.push_back(alternative);

		// gitignore's folder rule: a folder that matches takes everything below it, which is the same
		// segments followed by "/**". An alternative that already ends so takes all below its matches.
		if (!alternative.bEverythingBelow)
		{
			CPathGlob::SAlternative below;
			below.FirstSegment = static_cast<uint32>(Glob.Segments.size());
			for (uint32 i = 0u; i < alternative.SegmentCount; ++i)
			{
				const CPathGlob::SSegment segment = Glob.Segments[alternative.FirstSegment + i];
				Glob.Segments.push_back(segment);
			}
			AddEverythingBelow(below.FirstSegment);
			below.SegmentCount = static_cast<uint32>(Glob.Segments.size()) - below.FirstSegment;
			below.bEverythingBelow = true;
			Glob.Alternatives.push_back(below);
		}
		return true;
	}

	// A trailing "/**": a '*' segment, so at least one segment more, then a globstar for the rest.
	void AddEverythingBelow (uint32 FirstSegment)
	{
		CPathGlob::SSegment anySegment;
		anySegment.FirstToken = static_cast<uint32>(Glob.Tokens.size());
		anySegment.TokenCount = 1u;
		Glob.Tokens.push_back({ CPathGlob::ETokenKind::Star, 0u });
		Glob.Segments.push_back(anySegment);
		AddGlobstar(FirstSegment);
	}

	// Consecutive globstars are one.
	void AddGlobstar (uint32 FirstSegment)
	{
		if (Glob.Segments.size() > FirstSegment && Glob.Segments.back().bGlobstar)
		{
			return;
		}
		CPathGlob::SSegment segment;
		segment.FirstToken = static_cast<uint32>(Glob.Tokens.size());
		segment.bGlobstar = true;
		Glob.Segments.push_back(segment);
	}

	bool CompileSegment (FStringView Text, FStringView Alternative)
	{
		CPathGlob::SSegment segment;
		segment.FirstToken = static_cast<uint32>(Glob.Tokens.size());
		for (uint32 i = 0u; i < Text.Len();)
		{
			const char c = Text[i];
			if (c == '*')
			{
				if (i + 1u < Text.Len() && Text[i + 1u] == '*')
				{
					return Fail(FString::Printf("'**' must be a whole path segment, as in \"a/**/b\": '%s'",
						FString(Alternative).CStr()));
				}
				Glob.Tokens.push_back({ CPathGlob::ETokenKind::Star, 0u });
				++i;
			}
			else if (c == '?')
			{
				Glob.Tokens.push_back({ CPathGlob::ETokenKind::AnyChar, 0u });
				++i;
			}
			else if (c == '[')
			{
				SParsedSet parsed;
				ParseSet(Text, i, parsed, nullptr);
				Glob.Tokens.push_back({ CPathGlob::ETokenKind::Set, AddSet(parsed) });
				i = parsed.End;
			}
			else
			{
				if (c == '\\')
				{
					++i;
				}
				const unicode::SDecodedCodePoint unit = DecodeAt(Text, i);
				Glob.Tokens.push_back({ CPathGlob::ETokenKind::Literal, static_cast<uint32>(Fold(unit.CodePoint)) });
				i += unit.Length;
			}
		}
		segment.TokenCount = static_cast<uint32>(Glob.Tokens.size()) - segment.FirstToken;
		Glob.Segments.push_back(segment);
		return true;
	}

	// The members' ranges, merged, and the foldings of every member that folds to another code
	// point: a path character c is in the set iff some member x has Fold(x) == Fold(c), which is
	// Fold(c) in the ranges (x = Fold(c) itself, folding being idempotent) or in those foldings.
	uint32 AddSet (SParsedSet& Parsed)
	{
		std::sort(Parsed.Ranges.begin(), Parsed.Ranges.end());
		std::vector<CPathGlob::SRange> merged;
		for (const auto& [first, last] : Parsed.Ranges)
		{
			if (!merged.empty() && first <= merged.back().Last + 1u)
			{
				merged.back().Last = (std::max)(merged.back().Last, last);
			}
			else
			{
				merged.push_back({ first, last });
			}
		}

		const std::vector<char32_t>& folding = GetCodePointsThatFold();
		std::vector<char32_t> folded;
		for (const CPathGlob::SRange& range : merged)
		{
			auto it = std::lower_bound(folding.begin(), folding.end(), range.First);
			for (; it != folding.end() && *it <= range.Last; ++it)
			{
				folded.push_back(Fold(*it));
			}
		}
		std::sort(folded.begin(), folded.end());
		folded.erase(std::unique(folded.begin(), folded.end()), folded.end());

		CPathGlob::SCharSet set;
		set.bNegated = Parsed.bNegated;
		set.FirstRange = static_cast<uint32>(Glob.Ranges.size());
		set.RangeCount = static_cast<uint32>(merged.size());
		set.FirstFolded = static_cast<uint32>(Glob.Folded.size());
		set.FoldedCount = static_cast<uint32>(folded.size());
		Glob.Ranges.insert(Glob.Ranges.end(), merged.begin(), merged.end());
		Glob.Folded.insert(Glob.Folded.end(), folded.begin(), folded.end());
		Glob.Sets.push_back(set);
		return static_cast<uint32>(Glob.Sets.size() - 1u);
	}

	CPathGlob& Glob;
	FStringView Pattern;
};


std::optional<CPathGlob> CPathGlob::Compile (FStringView InPattern, FString* OutError)
{
	CPathGlob glob;
	glob.Pattern = FString(InPattern);
	CPathGlobCompiler compiler(glob, glob.Pattern);
	if (!compiler.Compile())
	{
		if (OutError)
		{
			*OutError = std::move(compiler.Error);
		}
		return std::nullopt;
	}
	return glob;
}

bool CPathGlob::Matches (FStringView RelativePath) const noexcept
{
	for (const SAlternative& alternative : Alternatives)
	{
		if (MatchAlternative(alternative, RelativePath))
		{
			return true;
		}
	}
	return false;
}

bool CPathGlob::MatchesBelow (FStringView Base, FStringView Path) const noexcept
{
	// Path == Base leaves an empty path, which no pattern matches.
	const std::optional<FStringView> relative = RelativeToBase(Base, Path);
	return relative && Matches(*relative);
}

bool CPathGlob::CoversFolder (FStringView Folder) const noexcept
{
	// The folder, or one above it, matches: the folder-rule alternatives take everything below.
	if (Matches(Folder))
	{
		return true;
	}

	// A trailing "/**" takes every path below whatever the segments before it match, so the folder
	// is covered when they match it: "Docs/**" covers "Docs", and "Scans/" ("**/Scans/**") "a/Scans".
	// So does a trailing lone '*' together with its folder-rule copy: "Docs/*" takes the files in
	// Docs, and its folders with everything below them.
	for (const SAlternative& alternative : Alternatives)
	{
		uint32 tail = 0u;
		if (alternative.bEverythingBelow)
		{
			tail = 2u;
		}
		else
		{
			const SSegment& last = Segments[alternative.FirstSegment + alternative.SegmentCount - 1u];
			const bool bLoneStar = !last.bGlobstar && last.TokenCount == 1u
				&& Tokens[last.FirstToken].Kind == ETokenKind::Star;
			tail = bLoneStar ? 1u : 0u;
		}
		if (tail == 0u)
		{
			continue;
		}

		SAlternative head = alternative;
		head.SegmentCount -= tail;
		head.bEverythingBelow = false;
		if (MatchAlternative(head, Folder))
		{
			return true;
		}
	}
	return false;
}

bool CPathGlob::CoversFolderBelow (FStringView Base, FStringView Folder) const noexcept
{
	const std::optional<FStringView> relative = RelativeToBase(Base, Folder);
	return relative && CoversFolder(*relative);
}

std::optional<FStringView> CPathGlob::RelativeToBase (FStringView Base, FStringView Path) noexcept
{
	if (Base.EndsWith('/'))
	{
		Base = Base.Left(Base.Len() - 1u);
	}
	if (Base.IsEmpty())
	{
		return Path;
	}

	// The prefix is matched under the same folding as the pattern, so "art" is the base of "Art/A.png".
	const uint32 matched = string_impl::MatchPrefixFolded(Path, Base, ESearchCase::IgnoreCase);
	if (matched == NotFound)
	{
		return std::nullopt;
	}
	if (matched == Path.Len())
	{
		return FStringView();
	}
	if (Path[matched] != '/')
	{
		return std::nullopt;
	}
	return Path.Mid(matched + 1u);
}

// Greedy with backtracking to the last globstar, over whole segments: every other pattern segment
// matches exactly one path segment, which is what makes the classic wildcard algorithm exact here.
bool CPathGlob::MatchAlternative (const SAlternative& Alternative, FStringView Path) const noexcept
{
	const SSegment* segments = Segments.data() + Alternative.FirstSegment;
	const uint32 count = Alternative.SegmentCount;
	// Segment starts are byte offsets; Len() + 1 is past the last segment. An empty path has none.
	const uint32 end = Path.IsEmpty() ? 0u : Path.Len() + 1u;
	uint32 p = 0u;
	uint32 t = 0u;
	uint32 starPattern = NotFound;
	uint32 starText = 0u;
	while (t < end)
	{
		const uint32 segmentEnd = FindSegmentEnd(Path, t);
		if (p < count)
		{
			const SSegment& segment = segments[p];
			if (segment.bGlobstar)
			{
				starPattern = p++;
				starText = t;
				continue;
			}
			if (MatchSegment(segment, FStringView(Path.GetData() + t, segmentEnd - t)))
			{
				++p;
				t = segmentEnd + 1u;
				continue;
			}
		}

		if (starPattern == NotFound)
		{
			return false;
		}
		// Let the last globstar take one more segment.
		p = starPattern + 1u;
		starText = FindSegmentEnd(Path, starText) + 1u;
		t = starText;
	}

	while (p < count && segments[p].bGlobstar)
	{
		++p;
	}
	return p == count;
}

// FStringView::MatchesWildcard's algorithm, over folded keys, inside one segment.
bool CPathGlob::MatchSegment (const SSegment& Segment, FStringView Text) const noexcept
{
	const SToken* tokens = Tokens.data() + Segment.FirstToken;
	const uint32 count = Segment.TokenCount;
	if (count == 1u && tokens[0].Kind == ETokenKind::Star)
	{
		return true;
	}

	uint32 p = 0u;
	uint32 t = 0u;
	uint32 starPattern = NotFound;
	uint32 starText = 0u;
	while (t < Text.Len())
	{
		if (p < count)
		{
			const SToken& token = tokens[p];
			if (token.Kind == ETokenKind::Star)
			{
				starPattern = p++;
				starText = t;
				continue;
			}

			const SUnitKey unit = ReadKey(Text, t);
			bool bMatch = false;
			switch (token.Kind)
			{
			case ETokenKind::Literal:
				bMatch = unit.Key == token.Value;
				break;
			case ETokenKind::AnyChar:
				bMatch = true;
				break;
			case ETokenKind::Set:
				bMatch = SetContains(Sets[token.Value], unit.Key);
				break;
			default:
				break;
			}
			if (bMatch)
			{
				++p;
				t += unit.Length;
				continue;
			}
		}

		if (starPattern == NotFound)
		{
			return false;
		}
		// Let the last '*' take one more unit.
		p = starPattern + 1u;
		starText += ReadKey(Text, starText).Length;
		t = starText;
	}

	while (p < count && tokens[p].Kind == ETokenKind::Star)
	{
		++p;
	}
	return p == count;
}

bool CPathGlob::SetContains (const SCharSet& Set, uint32 FoldedKey) const noexcept
{
	bool bMember = false;
	if (FoldedKey != IllFormedKey)
	{
		for (uint32 i = 0u; i < Set.RangeCount && !bMember; ++i)
		{
			const SRange& range = Ranges[Set.FirstRange + i];
			bMember = FoldedKey >= range.First && FoldedKey <= range.Last;
		}
		if (!bMember && Set.FoldedCount > 0u)
		{
			const char32_t* first = Folded.data() + Set.FirstFolded;
			bMember = std::binary_search(first, first + Set.FoldedCount, static_cast<char32_t>(FoldedKey));
		}
	}
	return bMember != Set.bNegated;
}
}
