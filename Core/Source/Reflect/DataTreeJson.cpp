#include "Reflect/DataTreeJson.h"

#include "Diagnostics/Diagnostics.h"
#include "Unicode/Unicode.h"


namespace frt::reflect
{
namespace
{
// A recursive-descent reader over the bytes of Text that feeds CDataTreeBuilder; the builder bounds the depth, so the
// recursion is bounded too (each level opens one map or sequence first).
class CJsonReader
{
public:
	explicit CJsonReader (FStringView InText) noexcept
		: Text(InText)
	{}

	bool Run (CDataTree& Out, SDiagnostic* OutError)
	{
		if (Text.Len() >= 3u && Text.StartsWith("\xEF\xBB\xBF"))
		{
			Offset = 3u;   // a UTF-8 byte order mark, not counted, as ParseYaml skips it
			LineStart = 3u;
		}
		const uint32 invalid = unicode::FindInvalidUtf8(Text.GetData(), Text.Len());
		if (invalid < Text.Len())
		{
			Offset = invalid;
			Fail("the text is not UTF-8");
		}
		else
		{
			SkipSpace();
			if (AtEnd())
			{
				Fail("no JSON value");
			}
			else if (ReadValue())
			{
				SkipSpace();
				if (!AtEnd())
				{
					Fail("text after the JSON value");
				}
			}
		}
		return Builder.Finish(Out, OutError);
	}

private:
	[[nodiscard]] bool AtEnd () const noexcept { return Offset >= Text.Len(); }
	[[nodiscard]] char Peek () const noexcept { return Text.GetData()[Offset]; }
	[[nodiscard]] uint32 GetColumn () const noexcept { return Offset - LineStart + 1u; }

	bool Fail (FStringView Message)
	{
		Builder.Fail(Message, Line, GetColumn());
		return false;
	}

	void SkipSpace () noexcept
	{
		while (!AtEnd())
		{
			const char c = Peek();
			if (c == '\n')
			{
				++Line;
				LineStart = Offset + 1u;
			}
			else if (c != ' ' && c != '\t' && c != '\r')
			{
				return;
			}
			++Offset;
		}
	}

	bool ReadValue ()
	{
		const uint32 line = Line;
		const uint32 column = GetColumn();
		switch (Peek())
		{
		case '{': return ReadObject();
		case '[': return ReadArray();
		case '"':
		{
			FString value;
			return ReadString(value) && Builder.Scalar(value.View(), EScalarStyle::DoubleQuoted, line, column);
		}
		case 't': return ReadWord("true") && Builder.Scalar("true", EScalarStyle::Plain, line, column);
		case 'f': return ReadWord("false") && Builder.Scalar("false", EScalarStyle::Plain, line, column);
		case 'n': return ReadWord("null") && Builder.Null(line, column);
		default: return ReadNumber(line, column);
		}
	}

	bool ReadWord (FStringView Word)
	{
		if (!Text.RightChop(Offset).StartsWith(Word))
		{
			return Fail("not a JSON value");
		}
		Offset += Word.Len();
		return true;
	}

	// RFC 8259 number grammar: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?, kept as its text.
	bool ReadNumber (uint32 NumberLine, uint32 NumberColumn)
	{
		const uint32 start = Offset;
		const auto digits = [this] ()
		{
			const uint32 first = Offset;
			while (!AtEnd() && Peek() >= '0' && Peek() <= '9')
			{
				++Offset;
			}
			return Offset - first;
		};
		if (!AtEnd() && Peek() == '-')
		{
			++Offset;
		}
		if (AtEnd() || Peek() < '0' || Peek() > '9')
		{
			return Fail("not a JSON value");
		}
		if (Peek() == '0')
		{
			++Offset;
		}
		else
		{
			digits();
		}
		if (!AtEnd() && Peek() == '.')
		{
			++Offset;
			if (digits() == 0u)
			{
				return Fail("a number without digits after its '.'");
			}
		}
		if (!AtEnd() && (Peek() == 'e' || Peek() == 'E'))
		{
			++Offset;
			if (!AtEnd() && (Peek() == '+' || Peek() == '-'))
			{
				++Offset;
			}
			if (digits() == 0u)
			{
				return Fail("a number without digits in its exponent");
			}
		}
		if (!AtEnd() && Peek() >= '0' && Peek() <= '9')
		{
			return Fail("a number with a leading zero");
		}
		return Builder.Scalar(Text.Mid(start, Offset - start), EScalarStyle::Plain, NumberLine, NumberColumn);
	}

	// Four hex digits after "\u".
	std::optional<char32_t> ReadHex4 ()
	{
		if (Text.Len() - Offset < 4u)
		{
			return std::nullopt;
		}
		char32_t value = 0u;
		for (uint32 i = 0u; i < 4u; ++i)
		{
			const char c = Peek();
			uint32 digit = 0u;
			if (c >= '0' && c <= '9')
			{
				digit = static_cast<uint32>(c - '0');
			}
			else if (c >= 'a' && c <= 'f')
			{
				digit = static_cast<uint32>(c - 'a' + 10);
			}
			else if (c >= 'A' && c <= 'F')
			{
				digit = static_cast<uint32>(c - 'A' + 10);
			}
			else
			{
				return std::nullopt;
			}
			value = value << 4u | digit;
			++Offset;
		}
		return value;
	}

	// A string at Offset (on its '"'). An escaped surrogate pair is one code point; an escaped lone surrogate becomes
	// its WTF-8 bytes, which is how the engine's text holds it.
	bool ReadString (FString& Out)
	{
		++Offset;
		while (true)
		{
			if (AtEnd())
			{
				return Fail("a string without its closing quote");
			}
			const char c = Peek();
			if (c == '"')
			{
				++Offset;
				return true;
			}
			if (static_cast<unsigned char>(c) < 0x20u)
			{
				return Fail("a control character inside a string (escape it)");
			}
			if (c != '\\')
			{
				Out.AppendChar(c);
				++Offset;
				continue;
			}
			++Offset;
			if (AtEnd())
			{
				return Fail("a string without its closing quote");
			}
			const char escape = Peek();
			++Offset;
			switch (escape)
			{
			case '"': Out.AppendChar('"'); break;
			case '\\': Out.AppendChar('\\'); break;
			case '/': Out.AppendChar('/'); break;
			case 'b': Out.AppendChar('\b'); break;
			case 'f': Out.AppendChar('\f'); break;
			case 'n': Out.AppendChar('\n'); break;
			case 'r': Out.AppendChar('\r'); break;
			case 't': Out.AppendChar('\t'); break;
			case 'u':
			{
				std::optional<char32_t> unit = ReadHex4();
				if (!unit.has_value())
				{
					return Fail("a \\u escape without four hex digits");
				}
				if (unicode::IsHighSurrogate(*unit) && Text.RightChop(Offset).StartsWith("\\u"))
				{
					const uint32 saved = Offset;
					Offset += 2u;
					const std::optional<char32_t> low = ReadHex4();
					if (low.has_value() && unicode::IsLowSurrogate(*low))
					{
						unit = 0x10000u + ((*unit - 0xD800u) << 10u) + (*low - 0xDC00u);
					}
					else
					{
						Offset = saved;   // the next escape is read on its own
					}
				}
				char bytes[4];
				Out.Append(FStringView(bytes, unicode::EncodeWtf8(*unit, bytes)));
				break;
			}
			default:
				--Offset;
				return Fail("an unknown escape in a string");
			}
		}
	}

	bool ReadObject ()
	{
		if (!Builder.BeginMap(Line, GetColumn(), ECollectionStyle::Flow))
		{
			return false;
		}
		++Offset;
		SkipSpace();
		if (!AtEnd() && Peek() == '}')
		{
			++Offset;
			return Builder.End();
		}
		while (true)
		{
			SkipSpace();
			if (AtEnd() || Peek() != '"')
			{
				return Fail("an object key must be a string");
			}
			const uint32 keyLine = Line;
			const uint32 keyColumn = GetColumn();
			FString key;
			if (!ReadString(key) || !Builder.Key(key.View(), keyLine, keyColumn))
			{
				return false;
			}
			SkipSpace();
			if (AtEnd() || Peek() != ':')
			{
				return Fail("a ':' after an object key");
			}
			++Offset;
			SkipSpace();
			if (AtEnd())
			{
				return Fail("an object key without its value");
			}
			if (!ReadValue())
			{
				return false;
			}
			SkipSpace();
			if (AtEnd())
			{
				return Fail("an object without its closing '}'");
			}
			if (Peek() == '}')
			{
				++Offset;
				return Builder.End();
			}
			if (Peek() != ',')
			{
				return Fail("a ',' or '}' after an object member");
			}
			++Offset;
		}
	}

	bool ReadArray ()
	{
		if (!Builder.BeginSequence(Line, GetColumn(), ECollectionStyle::Flow))
		{
			return false;
		}
		++Offset;
		SkipSpace();
		if (!AtEnd() && Peek() == ']')
		{
			++Offset;
			return Builder.End();
		}
		while (true)
		{
			SkipSpace();
			if (AtEnd())
			{
				return Fail("an array without its closing ']'");
			}
			if (!ReadValue())
			{
				return false;
			}
			SkipSpace();
			if (AtEnd())
			{
				return Fail("an array without its closing ']'");
			}
			if (Peek() == ']')
			{
				++Offset;
				return Builder.End();
			}
			if (Peek() != ',')
			{
				return Fail("a ',' or ']' after an array item");
			}
			++Offset;
		}
	}

	FStringView Text;
	uint32 Offset = 0u;
	uint32 Line = 1u;
	uint32 LineStart = 0u;
	CDataTreeBuilder Builder;
};
}

bool ParseJson (FStringView Text, CDataTree& Out, SDiagnostic* OutError)
{
	CJsonReader reader(Text);
	return reader.Run(Out, OutError);
}
}
