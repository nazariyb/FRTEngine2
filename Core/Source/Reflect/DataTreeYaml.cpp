#include "Reflect/DataTreeYaml.h"

#include <exception>
#include <istream>
#include <streambuf>
#include <string>

#include <yaml-cpp/anchor.h>
#include <yaml-cpp/emitterstyle.h>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/mark.h>
#include <yaml-cpp/parser.h>

#include "Diagnostics/Diagnostics.h"


namespace frt::reflect
{
namespace
{
// yaml-cpp reads an std::istream; this one reads the caller's bytes in place instead of copying them into an
// std::istringstream. putback (yaml-cpp's encoding probe) works because the whole text is the get area.
class CMemoryStreamBuffer final : public std::streambuf
{
public:
	CMemoryStreamBuffer (const char* Data, size_t Size)
	{
		char* const begin = const_cast<char*>(Data);   // the get area is never written through
		setg(begin, begin, begin + Size);
	}
};

// Thrown inside the event handler after it failed the builder. Inside an override's Guard it starts the draining
// (yaml-cpp reads on, the handler ignores the events); outside Guard it stops yaml-cpp, caught in ParseYaml.
struct SAbortParse
{};

[[nodiscard]] uint32 ToLine (const YAML::Mark& Mark) noexcept
{
	return Mark.line >= 0 ? static_cast<uint32>(Mark.line) + 1u : 0u;
}

[[nodiscard]] uint32 ToColumn (const YAML::Mark& Mark) noexcept
{
	return Mark.column >= 0 ? static_cast<uint32>(Mark.column) + 1u : 0u;
}

[[nodiscard]] char GetByteAt (FStringView Source, int Position) noexcept
{
	return Position >= 0 && static_cast<uint32>(Position) < Source.Len() ? Source[static_cast<uint32>(Position)] : '\0';
}

[[nodiscard]] bool CannotStartAPlainValue (char Ch) noexcept
{
	return Ch == '*' || Ch == '&' || Ch == '!' || Ch == '%' || Ch == '@' || Ch == '`';
}

// An unquoted value that starts with one of YAML's indicators * & ! % @ ` is an alias, an anchor, a tag, a directive or
// a reserved character, never text: a glob such as **/README.md fails as an alias to an undefined anchor. Message plus
// a hint to quote the value when the byte at the mark is such an indicator (or, for yaml-cpp's "alias not found after
// *" and "anchor not found after &", whose mark is past the indicator, the byte before it is '*' or '&').
[[nodiscard]] FString WithQuoteHint (FStringView Message, FStringView Source, const YAML::Mark& Mark)
{
	char indicator = GetByteAt(Source, Mark.pos);
	if (!CannotStartAPlainValue(indicator))
	{
		const char before = Mark.pos > 0 ? GetByteAt(Source, Mark.pos - 1) : '\0';
		indicator = before == '*' || before == '&' ? before : '\0';
	}
	if (indicator == '\0')
	{
		return FString(Message);
	}
	if (indicator == '*')
	{
		return FString::Format("{}; quote a value that starts with '*' (a glob: \"**/README.md\")", Message);
	}
	return FString::Format("{}; quote a value that starts with '{}'", Message, indicator);
}

// The null spelling (`null`, `Null`, `NULL` or `~`, followed by a delimiter) in Source at Mark, or empty.
[[nodiscard]] FStringView GetNullSpellingAt (FStringView Source, const YAML::Mark& Mark) noexcept
{
	static constexpr FStringLiteral Spellings[] = { "null", "Null", "NULL", "~" };
	if (Mark.pos < 0 || static_cast<uint32>(Mark.pos) >= Source.Len())
	{
		return {};
	}
	const FStringView rest = Source.RightChop(static_cast<uint32>(Mark.pos));
	for (const FStringLiteral spelling : Spellings)
	{
		if (!rest.StartsWith(spelling))
		{
			continue;
		}
		const char next = rest.Len() > spelling.Len() ? rest[spelling.Len()] : '\0';
		if (next == '\0' || next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == ':' || next == ','
			|| next == ']' || next == '}')
		{
			return rest.Left(spelling.Len());
		}
	}
	return {};
}

/**
 * yaml-cpp's events -> CDataTreeBuilder. The builder tracks where keys are due (IsExpectingKey), so a scalar event
 * becomes a Key or a value without a second state machine here. Every rule of FRT's subset that yaml-cpp does not
 * enforce itself (one document, no anchors/aliases/tags, scalar keys) is checked here, at the event's mark.
 * The overrides' std::string parameters are yaml-cpp's EventHandler interface, the file's only std text: each is
 * viewed as FStringView at once.
 *
 * After the first error the handler drains: it ignores the rest of the document's events while yaml-cpp reads on, so
 * that ParseYaml can tell a text outside FRT's subset (yaml-cpp reads it to the end) from one that is not YAML.
 */
class CTreeEventHandler final : public YAML::EventHandler
{
public:
	CTreeEventHandler (FStringView InSource, CDataTreeBuilder& InBuilder) noexcept
		: Source(InSource)
		, Builder(InBuilder)
	{}

	void OnDocumentStart (const YAML::Mark& Mark) override
	{
		if (bDocumentSeen)
		{
			// Stops yaml-cpp (outside Guard): YAML::Load reads the first document only, so the rest is not checked.
			if (!bDraining)
			{
				Builder.Fail("a second YAML document in one file ('---' starts a new document)", ToLine(Mark), ToColumn(Mark));
			}
			throw SAbortParse();
		}
		bDocumentSeen = true;
	}

	void OnDocumentEnd () override
	{}

	void OnNull (const YAML::Mark& Mark, YAML::anchor_t Anchor) override
	{
		Guard([&] { AddNull(Mark, Anchor); });
	}

	void OnAlias (const YAML::Mark& Mark, YAML::anchor_t /*Anchor*/) override
	{
		Guard([&] { Abort("aliases (*name) are not supported", Mark); });
	}

	void OnScalar (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, const std::string& Value) override
	{
		Guard([&] { AddScalar(Mark, FStringView(Tag), Anchor, FStringView(Value)); });
	}

	void OnSequenceStart (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, YAML::EmitterStyle::value Style) override
	{
		Guard([&] {
			CheckCollection(Mark, FStringView(Tag), Anchor, "a sequence");
			Check(Builder.BeginSequence(ToLine(Mark), ToColumn(Mark), Style == YAML::EmitterStyle::Flow ? ECollectionStyle::Flow : ECollectionStyle::Block));
		});
	}

	void OnSequenceEnd () override
	{
		Guard([&] { Check(Builder.End()); });
	}

	void OnMapStart (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, YAML::EmitterStyle::value Style) override
	{
		Guard([&] {
			CheckCollection(Mark, FStringView(Tag), Anchor, "a map");
			Check(Builder.BeginMap(ToLine(Mark), ToColumn(Mark), Style == YAML::EmitterStyle::Flow ? ECollectionStyle::Flow : ECollectionStyle::Block));
		});
	}

	void OnMapEnd () override
	{
		Guard([&] { Check(Builder.End()); });
	}

	void OnAnchor (const YAML::Mark& Mark, const std::string& /*Name*/) override
	{
		Guard([&] { Abort("anchors (&name) are not supported", Mark); });
	}

private:
	FStringView Source;
	CDataTreeBuilder& Builder;
	bool bDocumentSeen = false;
	bool bDraining = false;
	int LastMarkPosition = -1;   // Mark.pos of the last scalar, null or collection start

	// Runs one event's work unless the handler drains; an SAbortParse from it (the builder failed) starts draining.
	template <typename TBody>
	void Guard (TBody&& Body)
	{
		if (bDraining)
		{
			return;
		}
		try
		{
			Body();
		}
		catch (const SAbortParse&)
		{
			bDraining = true;
		}
	}

	[[noreturn]] void Abort (FStringView Message, const YAML::Mark& Mark)
	{
		Builder.Fail(WithQuoteHint(Message, Source, Mark).View(), ToLine(Mark), ToColumn(Mark));
		throw SAbortParse();
	}

	void Check (bool bOk)
	{
		if (!bOk)
		{
			throw SAbortParse();   // the builder holds the error and its position
		}
	}

	void CheckAnchor (YAML::anchor_t Anchor, const YAML::Mark& Mark)
	{
		if (Anchor != YAML::NullAnchor)
		{
			Abort("anchors (&name) are not supported", Mark);
		}
	}

	void AddScalar (const YAML::Mark& Mark, FStringView Tag, YAML::anchor_t Anchor, FStringView Value)
	{
		LastMarkPosition = Mark.pos;
		CheckAnchor(Anchor, Mark);
		const EScalarStyle style = GetStyle(Mark, Tag);
		if (style == EScalarStyle::SingleQuoted || style == EScalarStyle::DoubleQuoted)
		{
			CheckQuoteClosed(Mark, style == EScalarStyle::SingleQuoted ? '\'' : '"');
		}
		if (Builder.IsExpectingKey())
		{
			Check(Builder.Key(Value, ToLine(Mark), ToColumn(Mark)));
		}
		else
		{
			Check(Builder.Scalar(Value, style, ToLine(Mark), ToColumn(Mark)));
		}
	}

	// FRT's null value is empty, `~` or `null`, as HDataNode::IsNull reads any tree; yaml-cpp's YAML 1.2 core spellings
	// `Null` and `NULL` stay plain text. yaml-cpp also reports an empty value as a null event at the next token's mark,
	// which may be a key spelled `Null`: the entry's own key (`{NULL}`: the key's mark, just read) or the next entry's
	// key (`a:`, then `NULL: 1` on the next line: a ':' follows the spelling). Neither is the value's text.
	void AddNull (const YAML::Mark& Mark, YAML::anchor_t Anchor)
	{
		const int previousMarkPosition = LastMarkPosition;
		LastMarkPosition = Mark.pos;
		CheckAnchor(Anchor, Mark);
		if (Builder.IsExpectingKey())
		{
			AddNullKey(Mark);
			return;
		}
		const FStringView spelling = GetNullSpellingAt(Source, Mark);
		if ((spelling == "Null" || spelling == "NULL") && Mark.pos != previousMarkPosition
			&& !IsColonNext(static_cast<uint32>(Mark.pos) + spelling.Len()))
		{
			Check(Builder.Scalar(spelling, EScalarStyle::Plain, ToLine(Mark), ToColumn(Mark)));
			return;
		}
		Check(Builder.Null(ToLine(Mark), ToColumn(Mark)));
	}

	// True when the first byte from Position on that is not a space or a tab is ':'.
	[[nodiscard]] bool IsColonNext (uint32 Position) const noexcept
	{
		while (Position < Source.Len() && (Source[Position] == ' ' || Source[Position] == '\t'))
		{
			++Position;
		}
		return Position < Source.Len() && Source[Position] == ':';
	}

	void CheckCollection (const YAML::Mark& Mark, FStringView Tag, YAML::anchor_t Anchor, FStringLiteral What)
	{
		LastMarkPosition = Mark.pos;
		CheckAnchor(Anchor, Mark);
		if (Tag != "?" && !Tag.IsEmpty())
		{
			Abort("tags (!name, !!type) are not supported", Mark);
		}
		if (Builder.IsExpectingKey())
		{
			Abort(FString::Format("a map key must be a scalar, not {}", What), Mark);
		}
	}

	// '?' is yaml-cpp's non-specific tag of a plain scalar, '!' that of a quoted or block one; which of those is in
	// the source at the scalar's mark (its first byte). An explicit tag also arrives here: refused.
	EScalarStyle GetStyle (const YAML::Mark& Mark, FStringView Tag)
	{
		if (Tag == "?")
		{
			return EScalarStyle::Plain;
		}
		if (Tag == "!")
		{
			switch (GetByteAt(Source, Mark.pos))
			{
			case '\'': return EScalarStyle::SingleQuoted;
			case '"':  return EScalarStyle::DoubleQuoted;
			case '|':  return EScalarStyle::Literal;
			case '>':  return EScalarStyle::Folded;
			case '!':  break;   // "! text": an explicit non-specific tag
			default:
				Abort("cannot tell how this scalar is quoted", Mark);
			}
		}
		Abort("tags (!name, !!type) are not supported", Mark);
	}

	// yaml-cpp 0.8.0 ends a quoted scalar silently at the end of the text when a line break comes first ("a: 'open\n"
	// parses), so check that the quote opened at the mark closes: the next unescaped quote of its kind ('' is an
	// escaped single quote, a backslash escapes in double quotes).
	void CheckQuoteClosed (const YAML::Mark& Mark, char Quote)
	{
		uint32 i = Mark.pos >= 0 ? static_cast<uint32>(Mark.pos) + 1u : Source.Len();
		while (i < Source.Len())
		{
			const char ch = Source[i];
			if (Quote == '"' && ch == '\\')
			{
				i += 2u;
				continue;
			}
			if (ch == Quote)
			{
				if (Quote == '\'' && i + 1u < Source.Len() && Source[i + 1u] == '\'')
				{
					i += 2u;
					continue;
				}
				return;
			}
			++i;
		}
		Abort("a quoted scalar is not closed", Mark);
	}

	// yaml-cpp reports a plain key spelled like null (`null: 1`, `~: 1`, `NULL: 1`) as a null event, at the key's own
	// mark: keep the text it spells. Anything else there is a key that is really empty (`: 1`, `? `), which FRT refuses.
	void AddNullKey (const YAML::Mark& Mark)
	{
		const FStringView spelling = GetNullSpellingAt(Source, Mark);
		if (spelling.IsEmpty())
		{
			Abort("a map key is empty", Mark);
		}
		Check(Builder.Key(spelling, ToLine(Mark), ToColumn(Mark)));
	}
};
}


bool ParseYaml (FStringView Text, CDataTree& Out, SDiagnostic* OutError, EYamlFailure* OutFailure)
{
	Out = CDataTree();

	// yaml-cpp skips a byte order mark before it starts counting positions; skipping it here keeps the marks
	// indices into Source.
	FStringView source = Text;
	if (source.StartsWith("\xEF\xBB\xBF"))
	{
		source = source.RightChop(3u);
	}

	CDataTreeBuilder builder;
	// yaml-cpp's own error (YAML::Load would refuse the text too). It is reported even when the handler drained past
	// an earlier subset violation: the file has to become YAML first.
	bool bSyntaxError = false;
	FString syntaxMessage;
	uint32 syntaxLine = 0u;
	uint32 syntaxColumn = 0u;
	if (!source.IsEmpty())
	{
		CMemoryStreamBuffer buffer(source.GetData(), source.Len());
		std::istream stream(&buffer);
		CTreeEventHandler handler(source, builder);
		try
		{
			YAML::Parser parser(stream);
			if (parser.HandleNextDocument(handler))
			{
				// A second document stops in OnDocumentStart; trailing comments or "..." end quietly.
				parser.HandleNextDocument(handler);
			}
		}
		catch (const SAbortParse&)
		{
			// The builder has the error.
		}
		catch (const YAML::Exception& exception)
		{
			bSyntaxError = true;
			syntaxMessage = WithQuoteHint(FString::FromUtf8Lossy(FStringView(exception.msg)).View(), source, exception.mark);
			syntaxLine = ToLine(exception.mark);
			syntaxColumn = ToColumn(exception.mark);
		}
		catch (const std::exception& exception)
		{
			bSyntaxError = true;
			syntaxMessage = FString::Format("the YAML parser failed: {}", FString::FromUtf8Lossy(exception.what()));
		}
	}
	if (bSyntaxError)
	{
		builder.Fail(syntaxMessage.View(), syntaxLine, syntaxColumn);   // ignored when a subset violation came first
	}
	const bool bOk = builder.Finish(Out, OutError);
	if (bSyntaxError && OutError != nullptr)
	{
		OutError->Line = syntaxLine;
		OutError->Column = syntaxColumn;
		OutError->Message = std::move(syntaxMessage);
	}
	if (OutFailure != nullptr)
	{
		*OutFailure = bOk ? EYamlFailure::None : bSyntaxError ? EYamlFailure::Syntax : EYamlFailure::Subset;
	}
	return bOk;
}
}
