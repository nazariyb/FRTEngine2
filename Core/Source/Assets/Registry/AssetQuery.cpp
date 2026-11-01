#include "Assets/Registry/AssetQuery.h"

#include <algorithm>
#include <iterator>

#include "Diagnostics/Diagnostics.h"
#include "Unicode/Ascii.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral SystemPredicateNames[] = {
	"broken-ref", "unused", "legacy-ref", "excluded", "problem", "stale-tags", "primary", "redirected", "unsaved", "pending-meta",
};
static_assert(std::size(SystemPredicateNames) == SystemPredicateCount);
}


FStringLiteral GetSystemPredicateName (ESystemPredicate Predicate) noexcept
{
	static constexpr FStringLiteral Unknown = "unknown";
	const uint32 index = static_cast<uint32>(Predicate);
	return index < SystemPredicateCount ? SystemPredicateNames[index] : Unknown;
}

std::optional<ESystemPredicate> FindSystemPredicate (FStringView Name) noexcept
{
	for (uint32 i = 0u; i < SystemPredicateCount; ++i)
	{
		if (Name.Equals<ESearchCase::IgnoreCaseAscii>(SystemPredicateNames[i]))
		{
			return static_cast<ESystemPredicate>(i);
		}
	}
	return std::nullopt;
}

bool HasSystemPredicateData (ESystemPredicate Predicate) noexcept
{
	return Predicate != ESystemPredicate::Redirected && Predicate != ESystemPredicate::Unsaved && Predicate != ESystemPredicate::PendingMeta;
}


// ---------------- Builder ----------------

SAssetQuery::SAssetQuery () = default;
SAssetQuery::~SAssetQuery () = default;
SAssetQuery::SAssetQuery (const SAssetQuery& Other) = default;
SAssetQuery::SAssetQuery (SAssetQuery&& Other) noexcept = default;
SAssetQuery& SAssetQuery::operator= (const SAssetQuery& Other) = default;
SAssetQuery& SAssetQuery::operator= (SAssetQuery&& Other) noexcept = default;

SAssetQuery& SAssetQuery::OfType (FStringView TypeName, bool bIncludeDerived)
{
	Types.push_back({ FString(TypeName), bIncludeDerived, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::InFolder (FStringView MountPath, bool bRecursive)
{
	Folders.push_back({ FString(MountPath), bRecursive, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::WithLabel (FStringView Label)
{
	Labels.push_back({ FString(Label), false, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::WithoutLabel (FStringView Label)
{
	Labels.push_back({ FString(Label), true, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::WithId (const SAssetId& Id)
{
	Guids.push_back(Id);
	return *this;
}

SAssetQuery& SAssetQuery::Scope (ERecordScope InRecords)
{
	Records = InRecords;
	return *this;
}

SAssetQuery& SAssetQuery::RowsOf (const SAssetId& Table)
{
	Tables.push_back({ Table, FString(), 0u });
	Records = ERecordScope::Rows;
	return *this;
}

SAssetQuery& SAssetQuery::RowsOf (FStringView TableName)
{
	Tables.push_back({ SAssetId {}, FString(TableName), 0u });
	Records = ERecordScope::Rows;
	return *this;
}

SAssetQuery& SAssetQuery::Where (FStringView TagName, ECompare Op, STagValue Value)
{
	SQueryPredicate predicate;
	predicate.Key = FString(TagName);
	predicate.Op = Op;
	predicate.Values.push_back(std::move(Value));
	Predicates.push_back(std::move(predicate));
	return *this;
}

SAssetQuery& SAssetQuery::Where (FStringView TagName, ECompare Op)
{
	SQueryPredicate predicate;
	predicate.Key = FString(TagName);
	predicate.Op = Op;
	Predicates.push_back(std::move(predicate));
	return *this;
}

SAssetQuery& SAssetQuery::WhereIn (FStringView TagName, std::span<const STagValue> Values)
{
	SQueryPredicate predicate;
	predicate.Key = FString(TagName);
	predicate.Op = ECompare::In;
	predicate.Values.assign(Values.begin(), Values.end());
	Predicates.push_back(std::move(predicate));
	return *this;
}

SAssetQuery& SAssetQuery::AnyOf (std::initializer_list<SAssetQuery> Alternatives)
{
	return AnyOf(std::vector<SAssetQuery>(Alternatives));
}

SAssetQuery& SAssetQuery::AnyOf (std::vector<SAssetQuery> Alternatives)
{
	SQueryAnyOf group;
	group.Alternatives = std::move(Alternatives);
	AnyOfGroups.push_back(std::move(group));
	return *this;
}

SAssetQuery& SAssetQuery::References (const SAssetId& Target, bool bTransitive, EDepFlags Mask)
{
	Graph.push_back({ EQueryGraphDirection::References, Target, FString(), bTransitive, Mask, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::References (FStringView Target, bool bTransitive, EDepFlags Mask)
{
	Graph.push_back({ EQueryGraphDirection::References, SAssetId {}, FString(Target), bTransitive, Mask, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::ReferencedBy (const SAssetId& Source, bool bTransitive, EDepFlags Mask)
{
	Graph.push_back({ EQueryGraphDirection::ReferencedBy, Source, FString(), bTransitive, Mask, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::ReferencedBy (FStringView Source, bool bTransitive, EDepFlags Mask)
{
	Graph.push_back({ EQueryGraphDirection::ReferencedBy, SAssetId {}, FString(Source), bTransitive, Mask, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::Is (ESystemPredicate Predicate)
{
	SystemPredicates.push_back({ Predicate, 0u });
	return *this;
}

SAssetQuery& SAssetQuery::Text (FStringView Substring)
{
	TextTerms.push_back({ FString(Substring), 0u });
	return *this;
}

SAssetQuery& SAssetQuery::OrderBy (FStringView TagName, bool bAscending)
{
	Order = SQueryOrder { FString(TagName), bAscending, 0u };
	return *this;
}

SAssetQuery& SAssetQuery::Limit (uint32 Count, uint32 Offset)
{
	LimitCount = Count;
	OffsetCount = Offset;
	return *this;
}

bool SAssetQuery::IsUnrestricted () const noexcept
{
	return Types.empty() && Folders.empty() && Labels.empty() && Guids.empty() && Tables.empty() && Predicates.empty()
		&& AnyOfGroups.empty() && TextTerms.empty() && Graph.empty() && SystemPredicates.empty();
}


// ---------------- Describe ----------------

namespace
{
// A value as Describe prints it: quoted text in '...', typed values by type.
void AppendValue (FString& Out, const STagValue& Value)
{
	switch (Value.Type)
	{
	case ETagType::None:
		if (Value.IsQuotedLiteral())
		{
			Out += '\'';
			Out += Value.Text;
			Out += '\'';
		}
		else
		{
			Out += Value.Text;
		}
		return;
	case ETagType::Int64:
		Out.AppendNumber(Value.AsInt64());
		return;
	case ETagType::Double:
		Out.AppendNumber(Value.AsDouble());
		return;
	case ETagType::Bool:
		Out += Value.AsBool() ? "true" : "false";
		return;
	case ETagType::Name:
	case ETagType::String:
		Out += '"';
		Out += Value.Text;
		Out += '"';
		return;
	case ETagType::Enum:
	case ETagType::AssetRef:
	case ETagType::RowRef:
		Out.AppendFormat("{}#{}", enum_::ToString(Value.Type), Value.Bits);
		return;
	}
}

void AppendId (FString& Out, const SAssetId& Id)
{
	Out += Id.Guid.ToString();
	if (Id.SubId != 0u)
	{
		Out.AppendFormat(":{:016x}", Id.SubId);
	}
}

void AppendSeparator (FString& Out)
{
	if (!Out.IsEmpty())
	{
		Out += ' ';
	}
}

void DescribeInto (const SAssetQuery& Query, FString& Out)
{
	for (const SQueryText& text : Query.TextTerms)
	{
		AppendSeparator(Out);
		Out.AppendFormat("text:\"{}\"", text.Text);
	}
	for (const SQueryType& type : Query.Types)
	{
		AppendSeparator(Out);
		Out.AppendFormat("type{}{}", type.bIncludeDerived ? ":" : "=", type.Name);
	}
	for (const SQueryFolder& folder : Query.Folders)
	{
		AppendSeparator(Out);
		Out.AppendFormat("dir{}{}", folder.bRecursive ? ":" : "=", folder.MountPath);
	}
	for (const SQueryLabel& label : Query.Labels)
	{
		AppendSeparator(Out);
		Out.AppendFormat("{}label:{}", label.bExclude ? "-" : "", label.Label);
	}
	for (const SAssetId& id : Query.Guids)
	{
		AppendSeparator(Out);
		Out += "id:";
		AppendId(Out, id);
	}
	if (Query.Records != ERecordScope::Assets)
	{
		AppendSeparator(Out);
		Out.AppendFormat("records:{}", enum_::ToString(Query.Records));
	}
	for (const SQueryTable& table : Query.Tables)
	{
		AppendSeparator(Out);
		Out += "table:";
		if (table.Table.IsValid())
		{
			AppendId(Out, table.Table);
		}
		else
		{
			Out += table.Name;
		}
	}
	for (const SQueryPredicate& predicate : Query.Predicates)
	{
		AppendSeparator(Out);
		Out.AppendFormat("where:{} {}", predicate.Key, enum_::ToString(predicate.Op));
		if (predicate.Op == ECompare::In)
		{
			Out += " (";
			for (size_t i = 0u; i < predicate.Values.size(); ++i)
			{
				if (i != 0u)
				{
					Out += ',';
				}
				AppendValue(Out, predicate.Values[i]);
			}
			Out += ')';
		}
		else
		{
			for (const STagValue& value : predicate.Values)
			{
				Out += ' ';
				AppendValue(Out, value);
			}
		}
	}
	for (const SQueryGraph& graph : Query.Graph)
	{
		AppendSeparator(Out);
		Out += graph.Direction == EQueryGraphDirection::References ? "ref:" : "dep:";
		if (graph.bTransitive)
		{
			Out += '~';
		}
		if (graph.Target.IsValid())
		{
			AppendId(Out, graph.Target);
		}
		else
		{
			Out += graph.TargetText;
		}
		if (static_cast<uint16>(graph.Mask) != 0u)
		{
			Out.AppendFormat("[mask {:#x}]", static_cast<uint16>(graph.Mask));
		}
	}
	for (const SQuerySystemPredicate& system : Query.SystemPredicates)
	{
		AppendSeparator(Out);
		Out.AppendFormat("is:{}", GetSystemPredicateName(system.Predicate));
	}
	for (const SQueryAnyOf& group : Query.AnyOfGroups)
	{
		AppendSeparator(Out);
		Out += "any(";
		for (size_t i = 0u; i < group.Alternatives.size(); ++i)
		{
			if (i != 0u)
			{
				Out += " | ";
			}
			FString alternative;
			DescribeInto(group.Alternatives[i], alternative);
			Out += alternative;
		}
		Out += ')';
	}
	if (Query.Order)
	{
		AppendSeparator(Out);
		Out.AppendFormat("order:{} {}", Query.Order->Key, Query.Order->bAscending ? "asc" : "desc");
	}
	if (Query.LimitCount != 0u || Query.OffsetCount != 0u)
	{
		AppendSeparator(Out);
		Out.AppendFormat("limit:{}", Query.LimitCount);
		if (Query.OffsetCount != 0u)
		{
			Out.AppendFormat(" offset:{}", Query.OffsetCount);
		}
	}
}
}

FString SAssetQuery::Describe () const
{
	FString text;
	DescribeInto(*this, text);
	return text;
}


// ---------------- Parse ----------------

namespace
{
constexpr uint32 MaxGroupDepth = 32u;

[[nodiscard]] bool IsSpace (char Ch) noexcept
{
	return Ch == ' ' || Ch == '\t' || Ch == '\r' || Ch == '\n' || Ch == '\f' || Ch == '\v';
}

[[nodiscard]] bool IsKeyStart (char Ch) noexcept
{
	return (Ch >= 'A' && Ch <= 'Z') || (Ch >= 'a' && Ch <= 'z') || Ch == '_' || Ch == '$';
}

[[nodiscard]] bool IsKeyChar (char Ch) noexcept
{
	return IsKeyStart(Ch) || (Ch >= '0' && Ch <= '9') || Ch == '.';
}

/**
 * Recursive descent straight over the bytes (no token list): a term's shape depends on what follows a key (an
 * operator, `in (`, or nothing, which makes it text), so the parser looks ahead in the text itself. Every error
 * records the byte offset of the term at fault; the first error stops the parse.
 *
 *	query   := orExpr
 *	orExpr  := andExpr ( "or" andExpr )*
 *	andExpr := term+
 *	term    := "(" orExpr ")" | "-" labelTerm | quoted | "rows" | key op value | key "in" "(" list ")" | word
 */
class CQueryParser
{
public:
	explicit CQueryParser (FStringView InText) noexcept
		: Text(InText)
	{}

	bool Run (SAssetQuery& Out)
	{
		Top = &Out;
		SkipSpace();
		if (AtEnd())
		{
			return true;   // a blank query restricts nothing
		}
		if (!ParseOr(Out, 0u))
		{
			return false;
		}
		SkipSpace();
		if (!AtEnd())
		{
			// ParseOr stops only at the end, at ')' or at an error.
			return Fail(Position, "')' without a matching '('");
		}
		return true;
	}

	void FillError (SDiagnostic& Out) const
	{
		Out.Severity = ESeverity::Error;
		Out.Code = 0u;
		Out.Line = 1u;
		Out.Column = ErrorPosition + 1u;
		Out.Message = ErrorMessage;
	}

private:
	enum class EOperator : uint8 { Match, Eq, Ne, Lt, Le, Gt, Ge };

	FStringView Text;
	uint32 Position = 0u;
	SAssetQuery* Top = nullptr;
	bool bHasOrder = false;
	bool bHasLimit = false;
	uint32 ErrorPosition = 0u;
	FString ErrorMessage;

	[[nodiscard]] bool AtEnd () const noexcept { return Position >= Text.Len(); }
	[[nodiscard]] char Peek (uint32 Ahead = 0u) const noexcept
	{
		return Position + Ahead < Text.Len() ? Text[Position + Ahead] : '\0';
	}

	bool Fail (uint32 At, FStringView Message)
	{
		ErrorPosition = At < Text.Len() ? At : Text.Len();
		ErrorMessage = FString(Message);
		return false;
	}

	void SkipSpace () noexcept
	{
		while (!AtEnd() && IsSpace(Text[Position]))
		{
			++Position;
		}
	}

	// A term ends at a space, a parenthesis or the end; inside an `in` list also at ','.
	[[nodiscard]] bool IsDelimiterAt (uint32 At, bool bInList = false) const noexcept
	{
		if (At >= Text.Len())
		{
			return true;
		}
		const char ch = Text[At];
		return IsSpace(ch) || ch == '(' || ch == ')' || (bInList && ch == ',');
	}

	// The bytes at At equal the ASCII Word, ASCII case ignored. Compared as raw bytes, never through a sub-view: At + the
	// word's length may fall inside a multi-byte character of the query.
	[[nodiscard]] bool MatchesAsciiAt (uint32 At, FStringView Word) const noexcept
	{
		if (At > Text.Len() || Word.Len() > Text.Len() - At)
		{
			return false;
		}
		const char* const bytes = Text.GetData() + At;
		for (uint32 i = 0u; i < Word.Len(); ++i)
		{
			if (ascii::ToLower(bytes[i]) != ascii::ToLower(Word[i]))
			{
				return false;
			}
		}
		return true;
	}

	// Word (ASCII case ignored) at At, as a whole term.
	[[nodiscard]] bool IsWordAt (uint32 At, FStringView Word) const noexcept
	{
		return MatchesAsciiAt(At, Word) && IsDelimiterAt(At + Word.Len());
	}

	[[nodiscard]] uint32 Column (uint32 Offset) const noexcept { return Offset + 1u; }

	// What an AND list held: filters, and whole-query options (rows, order:, limit:), which go to the top query.
	struct STermCounts
	{
		uint32 Filters = 0u;
		uint32 Options = 0u;
	};

	// Every `or` branch needs a filter of its own: an empty branch or one of options only would match everything.
	// A query of options only ("order:Name", "rows") is fine.
	bool ParseOr (SAssetQuery& Into, uint32 Depth)
	{
		std::vector<SAssetQuery> branches(1u);
		const uint32 firstStart = Position;
		STermCounts first;
		if (!ParseAnd(branches.back(), Depth, first))
		{
			return false;
		}
		if (first.Filters == 0u && first.Options == 0u)
		{
			if (Depth == 0u && Peek() == ')')
			{
				return Fail(Position, "')' without a matching '('");
			}
			return Fail(Position, IsWordAt(Position, "or") ? "expected a term before 'or'" : "expected a term");
		}
		while (IsWordAt(Position, "or"))
		{
			if (first.Filters == 0u)
			{
				return Fail(firstStart, "expected a term before 'or' (rows, order: and limit: apply to the whole query)");
			}
			const uint32 orPosition = Position;
			Position += 2u;
			SkipSpace();
			branches.emplace_back();
			STermCounts counts;
			if (!ParseAnd(branches.back(), Depth, counts))
			{
				return false;
			}
			if (counts.Filters == 0u)
			{
				return Fail(counts.Options != 0u ? orPosition : Position,
					counts.Options != 0u ? "expected a term after 'or' (rows, order: and limit: apply to the whole query)" : "expected a term after 'or'");
			}
		}
		if (branches.size() == 1u)
		{
			Merge(Into, std::move(branches.front()));
		}
		else
		{
			Into.AnyOf(std::move(branches));
		}
		return true;
	}

	// Terms until the end, a ')' or an `or`.
	bool ParseAnd (SAssetQuery& Into, uint32 Depth, STermCounts& OutCounts)
	{
		for (;;)
		{
			SkipSpace();
			if (AtEnd() || Peek() == ')' || IsWordAt(Position, "or"))
			{
				return true;
			}
			bool bOption = false;
			if (!ParseTerm(Into, Depth, bOption))
			{
				return false;
			}
			++(bOption ? OutCounts.Options : OutCounts.Filters);
		}
	}

	static void Merge (SAssetQuery& Into, SAssetQuery&& From)
	{
		const auto append = [] (auto& Destination, auto& Source)
		{
			Destination.insert(Destination.end(), std::make_move_iterator(Source.begin()), std::make_move_iterator(Source.end()));
		};
		append(Into.Types, From.Types);
		append(Into.Folders, From.Folders);
		append(Into.Labels, From.Labels);
		append(Into.Guids, From.Guids);
		append(Into.Tables, From.Tables);
		append(Into.Predicates, From.Predicates);
		append(Into.AnyOfGroups, From.AnyOfGroups);
		append(Into.TextTerms, From.TextTerms);
		append(Into.Graph, From.Graph);
		append(Into.SystemPredicates, From.SystemPredicates);
	}

	bool ParseTerm (SAssetQuery& Into, uint32 Depth, bool& bOutOption)
	{
		const uint32 start = Position;
		const char ch = Peek();
		if (ch == '(')
		{
			if (Depth + 1u > MaxGroupDepth)
			{
				return Fail(start, FString::Format("parentheses nested deeper than {}", MaxGroupDepth));
			}
			++Position;
			SkipSpace();
			if (Peek() == ')')
			{
				return Fail(start, "empty parentheses");
			}
			if (AtEnd())
			{
				return Fail(start, "'(' without a matching ')'");
			}
			if (!ParseOr(Into, Depth + 1u))
			{
				return false;
			}
			SkipSpace();
			if (Peek() != ')')
			{
				return Fail(start, "'(' without a matching ')'");
			}
			++Position;
			return true;
		}
		if (ch == '"' || ch == '\'')
		{
			FString phrase;
			if (!ParseQuoted(phrase) || !ExpectDelimiter(false))
			{
				return false;
			}
			if (phrase.IsEmpty())
			{
				return Fail(start, "an empty quoted phrase");
			}
			Into.TextTerms.push_back({ std::move(phrase), Column(start) });
			return true;
		}
		if (ch == '-')
		{
			return ParseNegated(Into);
		}
		if (IsWordAt(start, "rows"))
		{
			if (Depth != 0u)
			{
				return Fail(start, "rows applies to the whole query: move it out of the parentheses");
			}
			Position += 4u;
			Top->Records = ERecordScope::Rows;
			bOutOption = true;
			return true;
		}

		// key op value, key in (...), or a text word.
		uint32 keyEnd = start;
		if (IsKeyStart(ch))
		{
			while (keyEnd < Text.Len() && IsKeyChar(Text[keyEnd]))
			{
				++keyEnd;
			}
			EOperator op = EOperator::Match;
			uint32 opLength = 0u;
			if (ReadOperator(keyEnd, op, opLength))
			{
				const FStringView key = Text.Mid(start, keyEnd - start);
				Position = keyEnd + opLength;
				return ParseKeyedTerm(Into, key, op, start, keyEnd, Depth, bOutOption);
			}
			if (IsDelimiterAt(keyEnd) && IsInListAhead(keyEnd))
			{
				return ParseIn(Into, Text.Mid(start, keyEnd - start), start);
			}
		}
		uint32 wordEnd = start;
		while (!IsDelimiterAt(wordEnd))
		{
			++wordEnd;
		}
		Into.TextTerms.push_back({ FString(Text.Mid(start, wordEnd - start)), Column(start) });
		Position = wordEnd;
		return true;
	}

	[[nodiscard]] bool ReadOperator (uint32 At, EOperator& OutOp, uint32& OutLength) const noexcept
	{
		const char first = At < Text.Len() ? Text[At] : '\0';
		const char second = At + 1u < Text.Len() ? Text[At + 1u] : '\0';
		switch (first)
		{
		case ':': OutOp = EOperator::Match; OutLength = 1u; return true;
		case '=': OutOp = EOperator::Eq; OutLength = 1u; return true;
		case '!':
			if (second == '=')
			{
				OutOp = EOperator::Ne;
				OutLength = 2u;
				return true;
			}
			return false;
		case '<':
			OutOp = second == '=' ? EOperator::Le : EOperator::Lt;
			OutLength = second == '=' ? 2u : 1u;
			return true;
		case '>':
			OutOp = second == '=' ? EOperator::Ge : EOperator::Gt;
			OutLength = second == '=' ? 2u : 1u;
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] static FStringLiteral GetOperatorText (EOperator Op) noexcept
	{
		switch (Op)
		{
		case EOperator::Match: return ":";
		case EOperator::Eq:    return "=";
		case EOperator::Ne:    return "!=";
		case EOperator::Lt:    return "<";
		case EOperator::Le:    return "<=";
		case EOperator::Gt:    return ">";
		case EOperator::Ge:    return ">=";
		}
		return "?";
	}

	[[nodiscard]] static ECompare ToCompare (EOperator Op) noexcept
	{
		switch (Op)
		{
		case EOperator::Match: return ECompare::Match;
		case EOperator::Eq:    return ECompare::Eq;
		case EOperator::Ne:    return ECompare::Ne;
		case EOperator::Lt:    return ECompare::Lt;
		case EOperator::Le:    return ECompare::Le;
		case EOperator::Gt:    return ECompare::Gt;
		case EOperator::Ge:    return ECompare::Ge;
		}
		return ECompare::Eq;
	}

	// After a key that ends at a delimiter: spaces, "in", optional spaces, "(".
	[[nodiscard]] bool IsInListAhead (uint32 KeyEnd) const noexcept
	{
		uint32 at = KeyEnd;
		if (at >= Text.Len() || !IsSpace(Text[at]))
		{
			return false;
		}
		while (at < Text.Len() && IsSpace(Text[at]))
		{
			++at;
		}
		if (!MatchesAsciiAt(at, "in"))
		{
			return false;
		}
		at += 2u;
		while (at < Text.Len() && IsSpace(Text[at]))
		{
			++at;
		}
		return at < Text.Len() && Text[at] == '(';
	}

	// At an opening quote ('"' or '\''): the text up to the matching quote, with \", \' and \\ unescaped (any other
	// backslash is kept as written).
	bool ParseQuoted (FString& Out)
	{
		const uint32 start = Position;
		const char quote = Text[Position++];
		Out.Clear();
		while (!AtEnd())
		{
			const char ch = Text[Position];
			if (ch == quote)
			{
				++Position;
				return true;
			}
			if (ch == '\\' && Position + 1u < Text.Len() && (Text[Position + 1u] == '"' || Text[Position + 1u] == '\'' || Text[Position + 1u] == '\\'))
			{
				Out += Text[Position + 1u];
				Position += 2u;
				continue;
			}
			Out += ch;
			++Position;
		}
		return Fail(start, "the quote is not closed");
	}

	bool ExpectDelimiter (bool bInList)
	{
		if (!IsDelimiterAt(Position, bInList))
		{
			return Fail(Position, bInList ? "expected ',', ')' or a space after the closing quote" : "expected a space after the closing quote");
		}
		return true;
	}

	// A value after an operator (or in a list): quoted, or bare up to a delimiter. Empty is an error at OpPosition.
	bool ParseValue (FString& Out, bool& bOutQuoted, bool bInList, uint32 ErrorAt, FStringView What)
	{
		bOutQuoted = false;
		if (Peek() == '"' || Peek() == '\'')
		{
			bOutQuoted = true;
			return ParseQuoted(Out) && ExpectDelimiter(bInList);
		}
		const uint32 start = Position;
		while (!IsDelimiterAt(Position, bInList))
		{
			++Position;
		}
		if (Position == start)
		{
			return Fail(ErrorAt, FString::Format("expected {}", What));
		}
		Out = FString(Text.Mid(start, Position - start));
		return true;
	}

	// A value that names something (a type, label, table, target, order key): as ParseValue, and a quoted empty text
	// ("" or '') is an error at its opening quote, since no name is empty.
	bool ParseName (FString& Out, bool& bOutQuoted, uint32 ErrorAt, FStringView What)
	{
		const uint32 start = Position;
		if (!ParseValue(Out, bOutQuoted, false, ErrorAt, What))
		{
			return false;
		}
		if (Out.IsEmpty())
		{
			return Fail(start, FString::Format("expected {}, not an empty quoted text", What));
		}
		return true;
	}

	bool ParseNegated (SAssetQuery& Into)
	{
		const uint32 start = Position;
		const uint32 keyStart = start + 1u;
		uint32 keyEnd = keyStart;
		while (keyEnd < Text.Len() && IsKeyChar(Text[keyEnd]))
		{
			++keyEnd;
		}
		EOperator op = EOperator::Match;
		uint32 opLength = 0u;
		if (keyEnd - keyStart != 5u || !MatchesAsciiAt(keyStart, "label") || !ReadOperator(keyEnd, op, opLength)
			|| (op != EOperator::Match && op != EOperator::Eq))
		{
			return Fail(start, "only label: terms can be negated (-label:Name); quote a text that starts with '-'");
		}
		Position = keyEnd + opLength;
		FString label;
		bool bQuoted = false;
		if (!ParseName(label, bQuoted, keyEnd, "a label after '-label:'"))
		{
			return false;
		}
		Into.Labels.push_back({ std::move(label), true, Column(start) });
		return true;
	}

	bool ParseIn (SAssetQuery& Into, FStringView Key, uint32 Start)
	{
		if (IsReservedKey(Key))
		{
			return Fail(Start, FString::Format("{}: does not take 'in'", Key));
		}
		// Past "<key> in": IsInListAhead checked the shape.
		Position = Start + Key.Len();
		SkipSpace();
		Position += 2u;   // "in"
		SkipSpace();
		const uint32 open = Position;
		++Position;       // "("
		SQueryPredicate predicate;
		predicate.Key = FString(Key);
		predicate.Op = ECompare::In;
		predicate.Column = Column(Start);
		for (;;)
		{
			SkipSpace();
			if (AtEnd())
			{
				return Fail(open, "'(' without a matching ')'");
			}
			if (Peek() == ')' && predicate.Values.empty())
			{
				return Fail(open, "an empty 'in' list");
			}
			FString value;
			bool bQuoted = false;
			if (!ParseValue(value, bQuoted, true, Position, "a value in the 'in' list"))
			{
				return false;
			}
			predicate.Values.push_back(STagValue::FromLiteral(value.View(), bQuoted));
			SkipSpace();
			if (Peek() == ',')
			{
				++Position;
				continue;
			}
			if (Peek() == ')')
			{
				++Position;
				break;
			}
			if (AtEnd())
			{
				return Fail(open, "'(' without a matching ')'");
			}
			return Fail(Position, "expected ',' or ')' in the 'in' list");
		}
		Into.Predicates.push_back(std::move(predicate));
		return true;
	}

	[[nodiscard]] static bool IsReservedKey (FStringView Key) noexcept
	{
		static constexpr FStringLiteral Reserved[] = { "type", "t", "dir", "label", "table", "ref", "dep", "is", "order", "limit" };
		for (const FStringLiteral word : Reserved)
		{
			if (Key.Equals<ESearchCase::IgnoreCaseAscii>(word))
			{
				return true;
			}
		}
		return false;
	}

	bool RequireOperator (EOperator Op, bool bAllowEq, FStringView Key, uint32 OpPosition)
	{
		if (Op == EOperator::Match || (bAllowEq && Op == EOperator::Eq))
		{
			return true;
		}
		return Fail(OpPosition, bAllowEq ? FString::Format("{} takes ':' or '=', not '{}'", Key, GetOperatorText(Op))
			: FString::Format("{} takes ':', not '{}'", Key, GetOperatorText(Op)));
	}

	bool ParseKeyedTerm (SAssetQuery& Into, FStringView Key, EOperator Op, uint32 Start, uint32 OpPosition, uint32 Depth, bool& bOutOption)
	{
		const auto is = [Key] (FStringView Word) { return Key.Equals<ESearchCase::IgnoreCaseAscii>(Word); };
		FString value;
		bool bQuoted = false;

		if (is("type") || is("t"))
		{
			if (!RequireOperator(Op, true, "type", OpPosition) || !ParseName(value, bQuoted, OpPosition, "a type name"))
			{
				return false;
			}
			Into.Types.push_back({ std::move(value), Op == EOperator::Match, Column(Start) });
			return true;
		}
		if (is("dir"))
		{
			if (!RequireOperator(Op, true, "dir", OpPosition) || !ParseValue(value, bQuoted, false, OpPosition, "a folder's mount path"))
			{
				return false;
			}
			// "/Project/Data/" names the same folder as "/Project/Data".
			while (value.Len() > 2u && value.EndsWith('/'))
			{
				value.LeftChopInline(1u);
			}
			if (!IsWellFormedMountPath(value.View()))
			{
				return Fail(OpPosition + 1u, "dir: takes a folder's mount path, such as /Project/Data");
			}
			Into.Folders.push_back({ std::move(value), Op == EOperator::Match, Column(Start) });
			return true;
		}
		if (is("label"))
		{
			if (!RequireOperator(Op, true, "label", OpPosition) || !ParseName(value, bQuoted, OpPosition, "a label"))
			{
				return false;
			}
			Into.Labels.push_back({ std::move(value), false, Column(Start) });
			return true;
		}
		if (is("table"))
		{
			if (!RequireOperator(Op, true, "table", OpPosition) || !ParseName(value, bQuoted, OpPosition, "a table"))
			{
				return false;
			}
			Into.Tables.push_back({ SAssetId {}, std::move(value), Column(Start) });
			Top->Records = ERecordScope::Rows;
			return true;
		}
		if (is("ref") || is("dep"))
		{
			if (!RequireOperator(Op, false, is("ref") ? "ref" : "dep", OpPosition))
			{
				return false;
			}
			const bool bTransitive = Peek() == '~';
			if (bTransitive)
			{
				++Position;
			}
			if (!ParseName(value, bQuoted, OpPosition, "a target (a mount path or a name)"))
			{
				return false;
			}
			SQueryGraph graph;
			graph.Direction = is("ref") ? EQueryGraphDirection::References : EQueryGraphDirection::ReferencedBy;
			graph.TargetText = std::move(value);
			graph.bTransitive = bTransitive;
			graph.Column = Column(Start);
			Into.Graph.push_back(std::move(graph));
			return true;
		}
		if (is("is"))
		{
			if (!RequireOperator(Op, false, "is", OpPosition) || !ParseValue(value, bQuoted, false, OpPosition, "a predicate after 'is:'"))
			{
				return false;
			}
			const std::optional<ESystemPredicate> predicate = FindSystemPredicate(value.View());
			if (!predicate)
			{
				FString names;
				for (uint32 i = 0u; i < SystemPredicateCount; ++i)
				{
					names += i == 0u ? "" : ", ";
					names += SystemPredicateNames[i];
				}
				return Fail(OpPosition + 1u, FString::Format("unknown predicate is:{} (known: {})", value, names));
			}
			Into.SystemPredicates.push_back({ *predicate, Column(Start) });
			return true;
		}
		if (is("order"))
		{
			if (Depth != 0u)
			{
				return Fail(Start, "order: applies to the whole query: move it out of the parentheses");
			}
			if (bHasOrder)
			{
				return Fail(Start, "order: is given twice");
			}
			if (!RequireOperator(Op, false, "order", OpPosition) || !ParseName(value, bQuoted, OpPosition, "a key to order by"))
			{
				return false;
			}
			bool bAscending = true;
			const uint32 afterKey = Position;
			SkipSpace();
			if (IsWordAt(Position, "desc"))
			{
				bAscending = false;
				Position += 4u;
			}
			else if (IsWordAt(Position, "asc"))
			{
				Position += 3u;
			}
			else
			{
				Position = afterKey;
			}
			Top->Order = SQueryOrder { std::move(value), bAscending, Column(Start) };
			bHasOrder = true;
			bOutOption = true;
			return true;
		}
		if (is("limit"))
		{
			if (Depth != 0u)
			{
				return Fail(Start, "limit: applies to the whole query: move it out of the parentheses");
			}
			if (bHasLimit)
			{
				return Fail(Start, "limit: is given twice");
			}
			if (!RequireOperator(Op, false, "limit", OpPosition) || !ParseValue(value, bQuoted, false, OpPosition, "a number after 'limit:'"))
			{
				return false;
			}
			const bool bDigits = std::all_of(value.View().begin(), value.View().end(), [] (char Ch) { return Ch >= '0' && Ch <= '9'; });
			const std::optional<uint32> count = bDigits ? value.View().ToUInt32() : std::nullopt;
			if (bQuoted || !count || *count == 0u)
			{
				return Fail(OpPosition + 1u, "limit: takes a whole number from 1 to 4294967295");
			}
			Top->LimitCount = *count;
			bHasLimit = true;
			bOutOption = true;
			return true;
		}

		// A tag comparison.
		if (!ParseValue(value, bQuoted, false, OpPosition, FString::Format("a value after '{}'", GetOperatorText(Op))))
		{
			return false;
		}
		SQueryPredicate predicate;
		predicate.Key = FString(Key);
		predicate.Op = ToCompare(Op);
		predicate.Values.push_back(STagValue::FromLiteral(value.View(), bQuoted));
		predicate.Column = Column(Start);
		Into.Predicates.push_back(std::move(predicate));
		return true;
	}
};
}

std::optional<SAssetQuery> SAssetQuery::Parse (FStringView Text, SDiagnostic* OutError)
{
	SAssetQuery query;
	CQueryParser parser(Text);
	if (!parser.Run(query))
	{
		if (OutError != nullptr)
		{
			parser.FillError(*OutError);
		}
		return std::nullopt;
	}
	return query;
}
}
