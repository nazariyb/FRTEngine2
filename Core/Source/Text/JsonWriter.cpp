#include "Text/JsonWriter.h"

#include "Unicode/Unicode.h"


namespace frt::json
{
void AppendJsonString (FString& Out, FStringView Text)
{
	constexpr FStringLiteral Hex = "0123456789abcdef";
	const auto appendEscape = [&Out, &Hex] (uint32 Unit)
	{
		Out.Append("\\u");
		for (int32 shift = 12; shift >= 0; shift -= 4)
		{
			Out.AppendChar(Hex[(Unit >> static_cast<uint32>(shift)) & 0xFu]);
		}
	};

	Out.AppendChar('"');
	const char* data = Text.GetData();
	uint32 offset = 0u;
	while (offset < Text.Len())
	{
		const char c = data[offset];
		const uint32 byte = static_cast<unsigned char>(c);
		if (byte < 0x80u)
		{
			switch (c)
			{
			case '"': Out.Append("\\\""); break;
			case '\\': Out.Append("\\\\"); break;
			case '\b': Out.Append("\\b"); break;
			case '\f': Out.Append("\\f"); break;
			case '\n': Out.Append("\\n"); break;
			case '\r': Out.Append("\\r"); break;
			case '\t': Out.Append("\\t"); break;
			default:
				if (byte < 0x20u)
				{
					appendEscape(byte);
				}
				else
				{
					Out.AppendChar(c);
				}
				break;
			}
			++offset;
			continue;
		}

		// DecodeWtf8 reads a WTF-8 surrogate as well-formed: its three bytes are no UTF-8, so it is
		// escaped, never copied.
		const unicode::SDecodedCodePoint decoded = unicode::DecodeWtf8(data + offset, Text.Len() - offset);
		if (!decoded.bWellFormed)
		{
			appendEscape(static_cast<uint32>(unicode::ReplacementCharacter));
		}
		else if (unicode::IsSurrogate(decoded.CodePoint))
		{
			appendEscape(static_cast<uint32>(decoded.CodePoint));
		}
		else
		{
			Out.Append(FStringView(data + offset, decoded.Length));
		}
		offset += decoded.Length;
	}
	Out.AppendChar('"');
}


CJsonObject& CJsonObject::AddString (FStringView Name, FStringView Value)
{
	AddName(Name);
	AppendJsonString(Fields, Value);
	return *this;
}

CJsonObject& CJsonObject::AddInt (FStringView Name, int64 Value)
{
	AddName(Name);
	Fields.AppendFormat("{}", Value);
	return *this;
}

CJsonObject& CJsonObject::AddUInt (FStringView Name, uint64 Value)
{
	AddName(Name);
	Fields.AppendFormat("{}", Value);
	return *this;
}

CJsonObject& CJsonObject::AddBool (FStringView Name, bool Value)
{
	AddName(Name);
	Fields.Append(Value ? "true" : "false");
	return *this;
}

CJsonObject& CJsonObject::AddStringArray (FStringView Name, std::span<const FString> Values)
{
	AddName(Name);
	Fields.AppendChar('[');
	for (size_t i = 0u; i < Values.size(); ++i)
	{
		if (i != 0u)
		{
			Fields.AppendChar(',');
		}
		AppendJsonString(Fields, Values[i]);
	}
	Fields.AppendChar(']');
	return *this;
}

CJsonObject& CJsonObject::AddObjectArray (FStringView Name, std::span<const CJsonObject> Values)
{
	AddName(Name);
	Fields.AppendChar('[');
	for (size_t i = 0u; i < Values.size(); ++i)
	{
		if (i != 0u)
		{
			Fields.AppendChar(',');
		}
		Fields.Append(Values[i].ToString());
	}
	Fields.AppendChar(']');
	return *this;
}

CJsonObject& CJsonObject::AddObject (FStringView Name, const CJsonObject& Value)
{
	AddName(Name);
	Fields.Append(Value.ToString());
	return *this;
}

CJsonObject& CJsonObject::AddFields (const CJsonObject& Other)
{
	if (!Other.Fields.IsEmpty())
	{
		if (!Fields.IsEmpty())
		{
			Fields.AppendChar(',');
		}
		Fields.Append(Other.Fields);
	}
	return *this;
}

FString CJsonObject::ToString () const
{
	FString text("{");
	text.Append(Fields).AppendChar('}');
	return text;
}

void CJsonObject::AddName (FStringView Name)
{
	if (!Fields.IsEmpty())
	{
		Fields.AppendChar(',');
	}
	AppendJsonString(Fields, Name);
	Fields.AppendChar(':');
}
}
