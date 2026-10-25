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

// Thrown by the event handler after it failed the builder, to stop yaml-cpp at the first error; caught in ParseYaml.
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

[[nodiscard]] FStringView ToView (const std::string& Text) noexcept
{
	return FStringView(Text.data(), Text.size());
}

/**
 * yaml-cpp's events -> CDataTreeBuilder. The builder tracks where keys are due (IsExpectingKey), so a scalar event
 * becomes a Key or a value without a second state machine here. Every rule of FRT's subset that yaml-cpp does not
 * enforce itself (one document, no anchors/aliases/tags, scalar keys) is checked here, at the event's mark.
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
			Abort("a second YAML document in one file ('---' starts a new document)", Mark);
		}
		bDocumentSeen = true;
	}

	void OnDocumentEnd () override
	{}

	void OnNull (const YAML::Mark& Mark, YAML::anchor_t Anchor) override
	{
		CheckAnchor(Anchor, Mark);
		if (Builder.IsExpectingKey())
		{
			AddNullKey(Mark);
			return;
		}
		Check(Builder.Null(ToLine(Mark), ToColumn(Mark)));
	}

	void OnAlias (const YAML::Mark& Mark, YAML::anchor_t /*Anchor*/) override
	{
		Abort("aliases (*name) are not supported", Mark);
	}

	void OnScalar (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, const std::string& Value) override
	{
		CheckAnchor(Anchor, Mark);
		const EScalarStyle style = GetStyle(Mark, Tag);
		if (style == EScalarStyle::SingleQuoted || style == EScalarStyle::DoubleQuoted)
		{
			CheckQuoteClosed(Mark, style == EScalarStyle::SingleQuoted ? '\'' : '"');
		}
		if (Builder.IsExpectingKey())
		{
			Check(Builder.Key(ToView(Value), ToLine(Mark), ToColumn(Mark)));
		}
		else
		{
			Check(Builder.Scalar(ToView(Value), style, ToLine(Mark), ToColumn(Mark)));
		}
	}

	void OnSequenceStart (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, YAML::EmitterStyle::value Style) override
	{
		CheckCollection(Mark, Tag, Anchor, "a sequence");
		Check(Builder.BeginSequence(ToLine(Mark), ToColumn(Mark), Style == YAML::EmitterStyle::Flow ? ECollectionStyle::Flow : ECollectionStyle::Block));
	}

	void OnSequenceEnd () override
	{
		Check(Builder.End());
	}

	void OnMapStart (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, YAML::EmitterStyle::value Style) override
	{
		CheckCollection(Mark, Tag, Anchor, "a map");
		Check(Builder.BeginMap(ToLine(Mark), ToColumn(Mark), Style == YAML::EmitterStyle::Flow ? ECollectionStyle::Flow : ECollectionStyle::Block));
	}

	void OnMapEnd () override
	{
		Check(Builder.End());
	}

	void OnAnchor (const YAML::Mark& Mark, const std::string& /*Name*/) override
	{
		Abort("anchors (&name) are not supported", Mark);
	}

private:
	FStringView Source;
	CDataTreeBuilder& Builder;
	bool bDocumentSeen = false;

	[[noreturn]] void Abort (FStringView Message, const YAML::Mark& Mark)
	{
		Builder.Fail(Message, ToLine(Mark), ToColumn(Mark));
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

	[[nodiscard]] char GetSourceByte (const YAML::Mark& Mark) const noexcept
	{
		return Mark.pos >= 0 && static_cast<uint32>(Mark.pos) < Source.Len() ? Source[static_cast<uint32>(Mark.pos)] : '\0';
	}

	void CheckCollection (const YAML::Mark& Mark, const std::string& Tag, YAML::anchor_t Anchor, FStringLiteral What)
	{
		CheckAnchor(Anchor, Mark);
		if (Tag != "?" && !Tag.empty())
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
	EScalarStyle GetStyle (const YAML::Mark& Mark, const std::string& Tag)
	{
		if (Tag == "?")
		{
			return EScalarStyle::Plain;
		}
		if (Tag == "!")
		{
			switch (GetSourceByte(Mark))
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

	// yaml-cpp reports a plain key spelled like null (`null: 1`, `~: 1`) as a null event, at the key's own mark: keep
	// the text it spells. Anything else there is a key that is really empty (`: 1`, `? `), which FRT refuses.
	void AddNullKey (const YAML::Mark& Mark)
	{
		static constexpr FStringLiteral Spellings[] = { "null", "Null", "NULL", "~" };
		if (Mark.pos >= 0 && static_cast<uint32>(Mark.pos) < Source.Len())
		{
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
					Check(Builder.Key(spelling, ToLine(Mark), ToColumn(Mark)));
					return;
				}
			}
		}
		Abort("a map key is empty", Mark);
	}
};
}


bool ParseYaml (FStringView Text, CDataTree& Out, SDiagnostic* OutError)
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
				// A second document fails in OnDocumentStart; trailing comments or "..." end quietly.
				parser.HandleNextDocument(handler);
			}
		}
		catch (const SAbortParse&)
		{
			// The builder has the error.
		}
		catch (const YAML::Exception& exception)
		{
			builder.Fail(FString::FromUtf8Lossy(ToView(exception.msg)), ToLine(exception.mark), ToColumn(exception.mark));
		}
		catch (const std::exception& exception)
		{
			builder.Fail(FString::Format("the YAML parser failed: {}", FString::FromUtf8Lossy(exception.what())), 0u, 0u);
		}
	}
	return builder.Finish(Out, OutError);
}
}
