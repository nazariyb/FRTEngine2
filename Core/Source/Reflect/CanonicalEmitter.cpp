#include "Reflect/CanonicalEmitter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>
#include <vector>

#include "Diagnostics/Diagnostics.h"
#include "Reflect/DataTreeYaml.h"
#include "Unicode/Unicode.h"


namespace frt::reflect
{
namespace
{
constexpr FStringLiteral DocumentLeadKeys[] = { "$type", "$version", "$rowType", "$rowVersion", "$note" };
constexpr FStringLiteral ReferenceLeadKeys[] = { "$asset", "sub", "row", "id", "path" };
constexpr FStringLiteral ReferenceKey = "$asset";
constexpr uint32 IndentWidth = 2u;

[[nodiscard]] bool IsReference (const HDataNode& Node) noexcept
{
	return Node.FindKeyIndex(ReferenceKey).has_value();
}

// Keys that hold a name or free text whatever it spells: `$note: 1.50` is a note, not a number to shorten.
[[nodiscard]] bool IsTextKey (FStringView Key) noexcept
{
	return Key == "$type" || Key == "$rowType" || Key == "$note";
}

[[nodiscard]] bool IsDigit (char Ch) noexcept
{
	return Ch >= '0' && Ch <= '9';
}

// [-+]?[0-9]+: what AsInt64 reads, in or out of its range.
[[nodiscard]] bool HasIntegerSyntax (FStringView Text) noexcept
{
	const FStringView digits = Text.StartsWith('-') || Text.StartsWith('+') ? Text.RightChop(1u) : Text;
	return !digits.IsEmpty() && std::all_of(digits.begin(), digits.end(), IsDigit);
}

// Digits with YAML 1.1's '_' separators and at least one digit from Set: the tail of 0x, 0o and 0b integers.
template <typename TIsSetDigit>
[[nodiscard]] bool IsRadixTail (FStringView Tail, TIsSetDigit&& IsSetDigit) noexcept
{
	bool bDigit = false;
	for (const char ch : Tail)
	{
		if (ch == '_')
		{
			continue;
		}
		if (!IsSetDigit(ch))
		{
			return false;
		}
		bDigit = true;
	}
	return bDigit;
}

/**
 * Whether some YAML reader takes this text for a number: YAML 1.2's core forms and 1.1's (`_` separators, 0x, 0o, 0b,
 * .Inf, .NaN). Over-approximates on purpose, like SGuid::HasNumericText: quoting a string that needed none costs nothing,
 * and leaving one unquoted changes its type in the first tool that reads the file with other rules.
 */
[[nodiscard]] bool SpellsNumber (FStringView Text) noexcept
{
	const FStringView body = Text.StartsWith('-') || Text.StartsWith('+') ? Text.RightChop(1u) : Text;
	if (body == ".inf" || body == ".Inf" || body == ".INF" || Text == ".nan" || Text == ".NaN" || Text == ".NAN")
	{
		return true;
	}
	if (body.Len() > 2u && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
	{
		return IsRadixTail(body.RightChop(2u), [] (char Ch) { return IsDigit(Ch) || (Ch >= 'a' && Ch <= 'f') || (Ch >= 'A' && Ch <= 'F'); });
	}
	if (body.Len() > 2u && body[0] == '0' && (body[1] == 'o' || body[1] == 'O'))
	{
		return IsRadixTail(body.RightChop(2u), [] (char Ch) { return Ch >= '0' && Ch <= '7'; });
	}
	if (body.Len() > 2u && body[0] == '0' && (body[1] == 'b' || body[1] == 'B'))
	{
		return IsRadixTail(body.RightChop(2u), [] (char Ch) { return Ch == '0' || Ch == '1'; });
	}

	uint32 i = 0u;
	uint32 digits = 0u;
	const uint32 length = body.Len();
	const auto skipDigits = [&] ()
	{
		while (i < length && (IsDigit(body[i]) || body[i] == '_'))
		{
			digits += body[i] == '_' ? 0u : 1u;
			++i;
		}
	};
	skipDigits();
	if (i < length && body[i] == '.')
	{
		++i;
		skipDigits();
	}
	if (digits == 0u)
	{
		return false;
	}
	if (i < length && (body[i] == 'e' || body[i] == 'E'))
	{
		++i;
		if (i < length && (body[i] == '+' || body[i] == '-'))
		{
			++i;
		}
		const uint32 exponent = i;
		while (i < length && IsDigit(body[i]))
		{
			++i;
		}
		if (i == exponent)
		{
			return false;
		}
	}
	return i == length;
}

// What a YAML 1.1 or 1.2 reader, yaml-cpp's convert<bool> among them, takes for a boolean or a null.
[[nodiscard]] bool SpellsBoolOrNull (FStringView Text) noexcept
{
	static constexpr FStringLiteral Words[] = {
		"y", "Y", "n", "N",
		"yes", "Yes", "YES", "no", "No", "NO",
		"on", "On", "ON", "off", "Off", "OFF",
		"true", "True", "TRUE", "false", "False", "FALSE",
		"null", "Null", "NULL", "~",
	};
	return std::find(std::begin(Words), std::end(Words), Text) != std::end(Words);
}

// A code point that a YAML text may not hold as it is (c-printable), or that a reader may break a line at: escaped.
[[nodiscard]] bool MustEscape (char32_t CodePoint) noexcept
{
	return CodePoint < 0x20u || (CodePoint >= 0x7Fu && CodePoint <= 0x9Fu) || CodePoint == 0x2028u || CodePoint == 0x2029u
		|| CodePoint == 0xFEFFu || unicode::IsNoncharacter(CodePoint);
}

[[nodiscard]] bool IsLeadingIndicator (char Ch) noexcept
{
	switch (Ch)
	{
	case '-': case '?': case ':': case ',': case '[': case ']': case '{': case '}': case '#': case '&': case '*':
	case '!': case '|': case '>': case '\'': case '"': case '%': case '@': case '`': case ' ':
		return true;
	default:
		return false;
	}
}

// Valid UTF-8 only: the caller refused anything else.
[[nodiscard]] bool NeedsQuotes (FStringView Text, bool bInFlow) noexcept
{
	if (Text.IsEmpty() || IsLeadingIndicator(Text[0]) || Text.EndsWith(' ') || SpellsBoolOrNull(Text) || SpellsNumber(Text)
		|| Text == "<<" || Text == "=" || Text == "...")
	{
		return true;
	}
	const char* const data = Text.GetData();
	const uint32 length = Text.Len();
	for (uint32 i = 0u; i < length;)
	{
		const char ch = data[i];
		if (static_cast<unsigned char>(ch) < 0x80u)
		{
			if (ch == ':' || ch == '#' || MustEscape(static_cast<char32_t>(ch)))
			{
				return true;
			}
			// yaml-cpp's flow scalars also end at any '?', which YAML itself would not ask for.
			if (bInFlow && (ch == ',' || ch == '?' || ch == '[' || ch == ']' || ch == '{' || ch == '}'))
			{
				return true;
			}
			++i;
			continue;
		}
		const unicode::SDecodedCodePoint decoded = unicode::DecodeUtf8(data + i, length - i);
		if (MustEscape(decoded.CodePoint))
		{
			return true;
		}
		i += decoded.Length;
	}
	return false;
}

void AppendHex (FString& Out, uint32 Value, uint32 Digits) noexcept
{
	for (uint32 digit = Digits; digit > 0u; --digit)
	{
		Out.AppendChar("0123456789ABCDEF"[(Value >> ((digit - 1u) * 4u)) & 0xFu]);
	}
}

// A double-quoted scalar on one line: every escape is one that YAML defines and ParseYaml decodes back to these bytes.
void AppendQuoted (FString& Out, FStringView Text) noexcept
{
	Out.AppendChar('"');
	const char* const data = Text.GetData();
	const uint32 length = Text.Len();
	for (uint32 i = 0u; i < length;)
	{
		const char ch = data[i];
		if (static_cast<unsigned char>(ch) >= 0x80u)
		{
			const unicode::SDecodedCodePoint decoded = unicode::DecodeUtf8(data + i, length - i);
			if (!MustEscape(decoded.CodePoint))
			{
				Out.Append(FStringView(data + i, decoded.Length));
			}
			else if (decoded.CodePoint <= 0xFFFFu)
			{
				Out.Append("\\u");
				AppendHex(Out, static_cast<uint32>(decoded.CodePoint), 4u);
			}
			else
			{
				Out.Append("\\U");
				AppendHex(Out, static_cast<uint32>(decoded.CodePoint), 8u);
			}
			i += decoded.Length;
			continue;
		}
		switch (ch)
		{
		case '"':  Out.Append("\\\""); break;
		case '\\': Out.Append("\\\\"); break;
		case '\n': Out.Append("\\n"); break;
		case '\r': Out.Append("\\r"); break;
		case '\t': Out.Append("\\t"); break;
		default:
			if (MustEscape(static_cast<char32_t>(ch)))
			{
				Out.Append("\\x");
				AppendHex(Out, static_cast<uint32>(ch), 2u);
			}
			else
			{
				Out.AppendChar(ch);
			}
			break;
		}
		++i;
	}
	Out.AppendChar('"');
}

template <typename T>
void AppendShortest (FString& Out, T Value) noexcept
{
	if (std::isnan(Value))
	{
		Out.Append(".nan");
		return;
	}
	if (std::isinf(Value))
	{
		Out.Append(Value < T(0) ? "-.inf" : ".inf");
		return;
	}
	char buffer[64];
	const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), Value);
	Out.Append(FStringView(buffer, static_cast<size_t>(result.ptr - buffer)));
}

// One scalar as it will be written, and what it must read back as.
struct SCanonicalScalar
{
	FString Text;
	bool bQuoted = false;
};

// A plain scalar that says a typed value, in that value's one spelling. False: it is text like any other.
[[nodiscard]] bool TryNormalizeTyped (const HDataNode& Node, FString& OutText)
{
	const FStringView text = Node.GetText();
	if (text == "true" || text == "false")
	{
		OutText = FString(text);
		return true;
	}
	if (HasIntegerSyntax(text))
	{
		// Outside int64 it is verbatim: a double, the only other reading, would not keep its digits.
		const std::optional<int64> integer = Node.AsInt64();
		if (!integer)
		{
			OutText = FString(text);
		}
		else if (*integer == 0 && text.StartsWith('-'))
		{
			OutText = FString("-0");   // AsDouble reads a negative zero, and a float's -0.0 is written so
		}
		else
		{
			OutText.Clear();
			OutText.AppendNumber(*integer);
		}
		return true;
	}
	if (const std::optional<double> number = Node.AsDouble())
	{
		OutText.Clear();
		AppendCanonicalNumber(OutText, *number);
		return true;
	}
	return false;
}

[[nodiscard]] SCanonicalScalar MakeCanonicalScalar (const HDataNode& Node, bool bText, bool bInFlow)
{
	SCanonicalScalar scalar;
	if (!bText && Node.GetScalarStyle() == EScalarStyle::Plain && TryNormalizeTyped(Node, scalar.Text))
	{
		return scalar;
	}
	scalar.Text = FString(Node.GetText());
	scalar.bQuoted = NeedsQuotes(scalar.Text.View(), bInFlow);
	return scalar;
}

// The tree -> bytes, and the same walk's check that bytes read back as what this tree says. Recursion is bounded by the
// depth limit every CDataTreeBuilder holds a tree to.
class CCanonicalWriter
{
public:
	CCanonicalWriter (const SEmitPolicy& InPolicy, FString& InOut, SDiagnostic* InError) noexcept
		: Policy(InPolicy)
		, Out(InOut)
		, Error(InError)
	{}

	[[nodiscard]] bool Write (const CDataTree& Tree)
	{
		const HDataNode root = Tree.GetRoot();
		if (!root.IsValid())
		{
			return true;
		}
		if (IsBlock(root))
		{
			WriteBlock(root, 0u, false, false);
		}
		else
		{
			WriteInline(root, false, false);
			Out.AppendChar('\n');
		}
		return !bFailed;
	}

	// Whether Read, the output parsed again, holds exactly what Write wrote for Tree.
	[[nodiscard]] bool Check (const CDataTree& Tree, const CDataTree& Read)
	{
		if (Tree.IsEmpty() || Read.IsEmpty())
		{
			return Tree.IsEmpty() == Read.IsEmpty() || Fail(Tree.GetRoot(), "the text reads back empty");
		}
		return CheckNode(Tree.GetRoot(), Read.GetRoot(), false, false);
	}

private:
	const SEmitPolicy& Policy;
	FString& Out;
	SDiagnostic* Error;
	std::vector<FStringView> Path;   // the keys to here; a sequence item is an empty view, which only "*" matches
	bool bFailed = false;

	bool Fail (const HDataNode& Node, FStringView Message)
	{
		if (!bFailed)
		{
			bFailed = true;
			if (Error != nullptr)
			{
				Error->Severity = ESeverity::Error;
				Error->Line = Node.GetLine();
				Error->Column = Node.GetColumn();
				Error->Message = FString(Message);
			}
		}
		return false;
	}

	[[nodiscard]] const SEmitRule* FindRule () const noexcept
	{
		for (const SEmitRule& rule : Policy.Rules)
		{
			if (MatchesPath(rule.Path))
			{
				return &rule;
			}
		}
		return nullptr;
	}

	[[nodiscard]] bool MatchesPath (FStringView RulePath) const noexcept
	{
		if (RulePath.IsEmpty())
		{
			return Path.empty();
		}
		FStringView rest = RulePath;
		for (size_t i = 0u; i < Path.size(); ++i)
		{
			const SStringSplit split = rest.SplitFirst('.');
			if (split.Left != "*" && (Path[i].IsEmpty() || split.Left != Path[i]))
			{
				return false;
			}
			if (!split.bFound)
			{
				return i + 1u == Path.size();
			}
			rest = split.Right;
		}
		return false;
	}

	// Flow by itself, not by what is above it. A node that is no collection, or an empty one, has no block form.
	[[nodiscard]] bool IsFlowNode (const HDataNode& Node) const noexcept
	{
		if (IsReference(Node))
		{
			return true;
		}
		const SEmitRule* const rule = FindRule();
		return rule != nullptr && rule->bFlow;
	}

	[[nodiscard]] bool IsBlock (const HDataNode& Node) const noexcept
	{
		return (Node.IsMap() || Node.IsSequence()) && Node.GetCount() > 0u && !IsFlowNode(Node);
	}

	// A map's entries in the order they are written.
	[[nodiscard]] std::vector<uint32> GetKeyOrder (const HDataNode& Map) const
	{
		std::vector<uint32> order;
		const uint32 count = Map.GetCount();
		order.reserve(count);
		std::vector<bool> placed(count, false);
		const auto place = [&] (std::span<const FStringLiteral> Keys)
		{
			for (const FStringLiteral key : Keys)
			{
				const std::optional<uint32> index = Map.FindKeyIndex(key);
				if (index && !placed[*index])
				{
					placed[*index] = true;
					order.push_back(*index);
				}
			}
		};
		const SEmitRule* const rule = FindRule();
		if (IsReference(Map))
		{
			place(ReferenceLeadKeys);
		}
		else
		{
			place(DocumentLeadKeys);
		}
		if (rule != nullptr)
		{
			place(rule->KeyOrder);
		}
		const size_t firstRemaining = order.size();
		for (uint32 i = 0u; i < count; ++i)
		{
			if (!placed[i])
			{
				order.push_back(i);
			}
		}
		if (rule != nullptr && rule->bSortKeys)
		{
			std::sort(order.begin() + static_cast<std::ptrdiff_t>(firstRemaining), order.end(), [&Map] (uint32 A, uint32 B)
			{
				return Map.GetKey(A) < Map.GetKey(B);
			});
		}
		return order;
	}

	void WriteKey (const SDataChild& Child, bool bInFlow)
	{
		if (!Child.Key.IsValidUtf8())
		{
			Fail(Child.Value, "a map key is not valid UTF-8");
			return;
		}
		if (NeedsQuotes(Child.Key, bInFlow))
		{
			AppendQuoted(Out, Child.Key);
		}
		else
		{
			Out.Append(Child.Key);
		}
		Out.AppendChar(':');
	}

	void WriteScalar (const HDataNode& Node, bool bText, bool bInFlow)
	{
		if (!Node.GetText().IsValidUtf8())
		{
			Fail(Node, "a scalar is not valid UTF-8");
			return;
		}
		const SCanonicalScalar scalar = MakeCanonicalScalar(Node, bText, bInFlow);
		if (scalar.bQuoted)
		{
			AppendQuoted(Out, scalar.Text.View());
		}
		else
		{
			Out.Append(scalar.Text.View());
		}
	}

	// Null, a scalar, or a collection in flow style, with no line break around it.
	void WriteInline (const HDataNode& Node, bool bText, bool bInFlow)
	{
		if (Node.IsNull())
		{
			Out.Append("null");
		}
		else if (Node.IsRaw())
		{
			Fail(Node, "a raw subtree has no canonical form");
		}
		else if (Node.IsScalar())
		{
			WriteScalar(Node, bText, bInFlow);
		}
		else if (Node.IsMap())
		{
			if (Node.GetCount() == 0u)
			{
				Out.Append("{}");
				return;
			}
			const bool bReference = IsReference(Node);
			Out.Append("{ ");
			bool bFirst = true;
			for (const uint32 index : GetKeyOrder(Node))
			{
				if (!bFirst)
				{
					Out.Append(", ");
				}
				bFirst = false;
				const SDataChild child = Node.GetChild(index);
				WriteKey(child, true);
				Out.AppendChar(' ');
				Path.push_back(child.Key);
				WriteInline(child.Value, bText || bReference || IsTextKey(child.Key), true);
				Path.pop_back();
			}
			Out.Append(" }");
		}
		else
		{
			if (Node.GetCount() == 0u)
			{
				Out.Append("[]");
				return;
			}
			Out.Append("[ ");
			for (uint32 i = 0u; i < Node.GetCount(); ++i)
			{
				if (i > 0u)
				{
					Out.Append(", ");
				}
				Path.push_back(FStringView());
				WriteInline(Node.GetValue(i), bText, true);
				Path.pop_back();
			}
			Out.Append(" ]");
		}
	}

	// A map or sequence in block style. bHeadPlaced: the cursor already stands where its first line goes (after "- ").
	void WriteBlock (const HDataNode& Node, uint32 Indent, bool bHeadPlaced, bool bText)
	{
		if (Node.IsMap())
		{
			bool bFirst = true;
			for (const uint32 index : GetKeyOrder(Node))
			{
				if (!bFirst || !bHeadPlaced)
				{
					Out.AppendChars(' ', Indent);
				}
				bFirst = false;
				const SDataChild child = Node.GetChild(index);
				WriteKey(child, false);
				const bool bChildText = bText || IsTextKey(child.Key);
				Path.push_back(child.Key);
				if (IsBlock(child.Value))
				{
					Out.AppendChar('\n');
					WriteBlock(child.Value, Indent + IndentWidth, false, bChildText);
				}
				else
				{
					Out.AppendChar(' ');
					WriteInline(child.Value, bChildText, false);
					Out.AppendChar('\n');
				}
				Path.pop_back();
			}
			return;
		}
		for (uint32 i = 0u; i < Node.GetCount(); ++i)
		{
			if (i > 0u || !bHeadPlaced)
			{
				Out.AppendChars(' ', Indent);
			}
			Out.Append("- ");
			const HDataNode item = Node.GetValue(i);
			Path.push_back(FStringView());
			if (IsBlock(item))
			{
				WriteBlock(item, Indent + IndentWidth, true, bText);
			}
			else
			{
				WriteInline(item, bText, false);
				Out.AppendChar('\n');
			}
			Path.pop_back();
		}
	}

	[[nodiscard]] bool CheckNode (const HDataNode& Node, const HDataNode& Read, bool bText, bool bInFlow)
	{
		if (Node.IsNull())
		{
			return Read.GetKind() == EDataNodeKind::Null || Fail(Node, "a null would not read back as a null");
		}
		if (Node.IsScalar())
		{
			const SCanonicalScalar scalar = MakeCanonicalScalar(Node, bText, bInFlow);
			const EScalarStyle written = scalar.bQuoted ? EScalarStyle::DoubleQuoted : EScalarStyle::Plain;
			if (Read.GetKind() != EDataNodeKind::Scalar || Read.GetText() != scalar.Text.View() || Read.GetScalarStyle() != written)
			{
				return Fail(Node, FString::Format("the text {} would not read back as written", scalar.Text.View().Truncate(40u)));
			}
			return true;
		}
		if (Node.GetKind() != Read.GetKind() || Node.GetCount() != Read.GetCount())
		{
			return Fail(Node, "a map or sequence would not read back with the same children");
		}
		const bool bFlow = bInFlow || (Node.GetCount() > 0u && IsFlowNode(Node));
		if (Node.IsSequence())
		{
			for (uint32 i = 0u; i < Node.GetCount(); ++i)
			{
				Path.push_back(FStringView());
				const bool bOk = CheckNode(Node.GetValue(i), Read.GetValue(i), bText, bFlow);
				Path.pop_back();
				if (!bOk)
				{
					return false;
				}
			}
			return true;
		}

		const bool bReference = IsReference(Node);
		const std::vector<uint32> order = GetKeyOrder(Node);
		for (uint32 i = 0u; i < order.size(); ++i)
		{
			const SDataChild child = Node.GetChild(order[i]);
			const SDataChild read = Read.GetChild(i);
			if (child.Key != read.Key)
			{
				return Fail(child.Value, FString::Format("the key {} would not read back as written", child.Key.Truncate(40u)));
			}
			Path.push_back(child.Key);
			const bool bOk = CheckNode(child.Value, read.Value, bText || bReference || IsTextKey(child.Key), bFlow);
			Path.pop_back();
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}
};
}


bool EmitCanonical (const CDataTree& Tree, const SEmitPolicy& Policy, FString& Out, SDiagnostic* OutError)
{
	Out.Clear();
	CCanonicalWriter writer(Policy, Out, OutError);
	if (!writer.Write(Tree))
	{
		Out.Clear();
		return false;
	}
	if (!Policy.bVerifyRoundTrip)
	{
		return true;
	}

	// The output says what the tree says, and is a fixed point: its own canonical form, byte for byte.
	CDataTree read;
	SDiagnostic parseError;
	if (!ParseYaml(Out.View(), read, &parseError))
	{
		if (OutError != nullptr)
		{
			OutError->Severity = ESeverity::Error;
			OutError->Line = 0u;
			OutError->Column = 0u;
			OutError->Message = FString::Format("the canonical text would not parse back: line {}: {}", parseError.Line, parseError.Message);
		}
		Out.Clear();
		return false;
	}
	if (!writer.Check(Tree, read))
	{
		Out.Clear();
		return false;
	}
	FString again;
	SEmitPolicy unverified = Policy;
	unverified.bVerifyRoundTrip = false;
	if (!EmitCanonical(read, unverified, again) || again != Out)
	{
		if (OutError != nullptr)
		{
			OutError->Severity = ESeverity::Error;
			OutError->Line = 0u;
			OutError->Column = 0u;
			OutError->Message = FString("the canonical text is not its own canonical form");
		}
		Out.Clear();
		return false;
	}
	return true;
}

void AppendCanonicalNumber (FString& Out, double Value) noexcept
{
	AppendShortest(Out, Value);
}

void AppendCanonicalNumber (FString& Out, float Value) noexcept
{
	AppendShortest(Out, Value);
}
}
