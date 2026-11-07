#include "Reflect/DataTree.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>

#include "Assets/ByteStream.h"
#include "Diagnostics/Diagnostics.h"


namespace frt::reflect
{
namespace
{
// Byte order of two keys: the order of SortedEntries and of its binary searches.
[[nodiscard]] int32 CompareKeyBytes (FStringView A, FStringView B) noexcept
{
	return assets::binary::CompareBytes(A, B);
}

[[nodiscard]] FStringLiteral GetKindName (EDataNodeKind Kind) noexcept
{
	switch (Kind)
	{
	case EDataNodeKind::Null:     return "a null value";
	case EDataNodeKind::Scalar:   return "a scalar";
	case EDataNodeKind::Map:      return "a map";
	case EDataNodeKind::Sequence: return "a sequence";
	case EDataNodeKind::Raw:      return "a raw subtree";
	}
	return "a node";
}

// Quotes text for a message, cut at 40 bytes (on a code point boundary) so a long scalar keeps the message short.
[[nodiscard]] FString QuoteForMessage (FStringView Text)
{
	constexpr uint32 MaxBytes = 40u;
	FString quoted("'");
	if (Text.Len() > MaxBytes)
	{
		quoted += Text.Truncate(MaxBytes);
		quoted += "...";
	}
	else
	{
		quoted += Text;
	}
	quoted += "'";
	return quoted;
}

void Report (SDiagnostic* OutError, const HDataNode& Node, FStringView Message)
{
	if (OutError != nullptr)
	{
		OutError->Severity = ESeverity::Error;
		OutError->Line = Node.GetLine();
		OutError->Column = Node.GetColumn();
		OutError->Message = FString(Message);
	}
}

// The plain scalar a typed accessor reads, or nullopt with the reason reported.
[[nodiscard]] std::optional<FStringView> GetPlainScalar (const HDataNode& Node, FStringLiteral Expected, SDiagnostic* OutError)
{
	if (!Node.IsValid())
	{
		Report(OutError, Node, FString::Format("expected {}, found nothing", Expected));
		return std::nullopt;
	}
	if (Node.GetKind() != EDataNodeKind::Scalar)
	{
		Report(OutError, Node, FString::Format("expected {}, found {}", Expected, GetKindName(Node.GetKind())));
		return std::nullopt;
	}
	if (Node.GetScalarStyle() != EScalarStyle::Plain)
	{
		// A quoted '1' is text by YAML's rules and by intent: never read it as the value it spells.
		Report(OutError, Node, FString::Format("expected {}, found the quoted or block text {}", Expected, QuoteForMessage(Node.GetText())));
		return std::nullopt;
	}
	return Node.GetText();
}

[[nodiscard]] bool IsDigit (char Ch) noexcept
{
	return Ch >= '0' && Ch <= '9';
}

// YAML 1.2 core float syntax, finite part: [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?
[[nodiscard]] bool IsDecimalFloatSyntax (FStringView Text) noexcept
{
	uint32 i = 0u;
	const uint32 length = Text.Len();
	if (i < length && (Text[i] == '+' || Text[i] == '-'))
	{
		++i;
	}
	uint32 integerDigits = 0u;
	while (i < length && IsDigit(Text[i]))
	{
		++i;
		++integerDigits;
	}
	uint32 fractionDigits = 0u;
	if (i < length && Text[i] == '.')
	{
		++i;
		while (i < length && IsDigit(Text[i]))
		{
			++i;
			++fractionDigits;
		}
		// "1." is a float; "." and ".e5" are not.
		if (integerDigits == 0u && fractionDigits == 0u)
		{
			return false;
		}
	}
	else if (integerDigits == 0u)
	{
		return false;
	}
	if (i < length && (Text[i] == 'e' || Text[i] == 'E'))
	{
		++i;
		if (i < length && (Text[i] == '+' || Text[i] == '-'))
		{
			++i;
		}
		uint32 exponentDigits = 0u;
		while (i < length && IsDigit(Text[i]))
		{
			++i;
			++exponentDigits;
		}
		if (exponentDigits == 0u)
		{
			return false;
		}
	}
	return i == length;
}
}


// ---------------- HDataNode ----------------

EDataNodeKind HDataNode::GetKind () const noexcept
{
	return Tree != nullptr ? Tree->Nodes[Index].Kind : EDataNodeKind::Null;
}

bool HDataNode::IsMap () const noexcept
{
	return Tree != nullptr && Tree->Nodes[Index].Kind == EDataNodeKind::Map;
}

bool HDataNode::IsSequence () const noexcept
{
	return Tree != nullptr && Tree->Nodes[Index].Kind == EDataNodeKind::Sequence;
}

bool HDataNode::IsScalar () const noexcept
{
	return Tree != nullptr && Tree->Nodes[Index].Kind == EDataNodeKind::Scalar;
}

bool HDataNode::IsRaw () const noexcept
{
	return Tree != nullptr && Tree->Nodes[Index].Kind == EDataNodeKind::Raw;
}

bool HDataNode::IsNull () const noexcept
{
	if (Tree == nullptr)
	{
		return false;
	}
	const CDataTree::SNode& node = Tree->Nodes[Index];
	if (node.Kind == EDataNodeKind::Null)
	{
		return true;
	}
	if (node.Kind != EDataNodeKind::Scalar || node.Style != static_cast<uint8>(EScalarStyle::Plain))
	{
		return false;
	}
	const FStringView text = Tree->GetTextAt(node.First, node.Count);
	return text.IsEmpty() || text == "~" || text == "null";
}

uint32 HDataNode::GetLine () const noexcept
{
	return Tree != nullptr ? Tree->Nodes[Index].Line : 0u;
}

uint32 HDataNode::GetColumn () const noexcept
{
	return Tree != nullptr ? Tree->Nodes[Index].Column : 0u;
}

EScalarStyle HDataNode::GetScalarStyle () const noexcept
{
	if (Tree == nullptr || Tree->Nodes[Index].Kind != EDataNodeKind::Scalar)
	{
		return EScalarStyle::Plain;
	}
	return static_cast<EScalarStyle>(Tree->Nodes[Index].Style);
}

ECollectionStyle HDataNode::GetCollectionStyle () const noexcept
{
	if (Tree == nullptr)
	{
		return ECollectionStyle::Block;
	}
	const CDataTree::SNode& node = Tree->Nodes[Index];
	if (node.Kind != EDataNodeKind::Map && node.Kind != EDataNodeKind::Sequence)
	{
		return ECollectionStyle::Block;
	}
	return static_cast<ECollectionStyle>(node.Style);
}

FStringView HDataNode::GetText () const noexcept
{
	if (Tree == nullptr)
	{
		return FStringView();
	}
	const CDataTree::SNode& node = Tree->Nodes[Index];
	if (node.Kind != EDataNodeKind::Scalar && node.Kind != EDataNodeKind::Raw)
	{
		return FStringView();
	}
	return Tree->GetTextAt(node.First, node.Count);
}

std::optional<bool> HDataNode::AsBool (SDiagnostic* OutError) const
{
	const std::optional<FStringView> text = GetPlainScalar(*this, "a boolean (true or false)", OutError);
	if (!text)
	{
		return std::nullopt;
	}
	if (*text == "true")
	{
		return true;
	}
	if (*text == "false")
	{
		return false;
	}
	Report(OutError, *this, FString::Format("expected a boolean (true or false), found {}", QuoteForMessage(*text)));
	return std::nullopt;
}

std::optional<int64> HDataNode::AsInt64 (SDiagnostic* OutError) const
{
	const std::optional<FStringView> text = GetPlainScalar(*this, "an integer", OutError);
	if (!text)
	{
		return std::nullopt;
	}

	// [-+]?[0-9]+, accumulated as a negative number so that INT64_MIN fits.
	const FStringView digits = (!text->IsEmpty() && ((*text)[0] == '-' || (*text)[0] == '+')) ? text->RightChop(1u) : *text;
	const bool bNegative = !text->IsEmpty() && (*text)[0] == '-';
	if (digits.IsEmpty() || !std::all_of(digits.begin(), digits.end(), IsDigit))
	{
		Report(OutError, *this, FString::Format("expected an integer, found {}", QuoteForMessage(*text)));
		return std::nullopt;
	}
	constexpr int64 minimum = std::numeric_limits<int64>::min();
	int64 value = 0;
	for (const char ch : digits)
	{
		const int64 digit = ch - '0';
		if (value < (minimum + digit) / 10)
		{
			Report(OutError, *this, FString::Format("the integer {} is outside the 64-bit range", QuoteForMessage(*text)));
			return std::nullopt;
		}
		value = value * 10 - digit;
	}
	if (!bNegative)
	{
		if (value == minimum)
		{
			Report(OutError, *this, FString::Format("the integer {} is outside the 64-bit range", QuoteForMessage(*text)));
			return std::nullopt;
		}
		value = -value;
	}
	return value;
}

std::optional<double> HDataNode::AsDouble (SDiagnostic* OutError) const
{
	const std::optional<FStringView> text = GetPlainScalar(*this, "a number", OutError);
	if (!text)
	{
		return std::nullopt;
	}
	if (*text == ".inf" || *text == "+.inf")
	{
		return std::numeric_limits<double>::infinity();
	}
	if (*text == "-.inf")
	{
		return -std::numeric_limits<double>::infinity();
	}
	if (*text == ".nan")
	{
		return std::numeric_limits<double>::quiet_NaN();
	}
	if (!IsDecimalFloatSyntax(*text))
	{
		Report(OutError, *this, FString::Format("expected a number, found {}", QuoteForMessage(*text)));
		return std::nullopt;
	}

	// from_chars: locale-independent, exact rounding; it takes no leading '+'.
	const FStringView number = (*text)[0] == '+' ? text->RightChop(1u) : *text;
	double value = 0.0;
	const std::from_chars_result result = std::from_chars(number.GetData(), number.GetData() + number.Len(), value, std::chars_format::general);
	if (result.ec == std::errc::result_out_of_range)
	{
		Report(OutError, *this, FString::Format("the number {} is outside the range of a double", QuoteForMessage(*text)));
		return std::nullopt;
	}
	if (result.ec != std::errc() || result.ptr != number.GetData() + number.Len())
	{
		Report(OutError, *this, FString::Format("expected a number, found {}", QuoteForMessage(*text)));
		return std::nullopt;
	}
	return value;
}

std::optional<FStringView> HDataNode::AsText (SDiagnostic* OutError) const
{
	if (!IsValid())
	{
		Report(OutError, *this, "expected text, found nothing");
		return std::nullopt;
	}
	if (GetKind() != EDataNodeKind::Scalar)
	{
		Report(OutError, *this, FString::Format("expected text, found {}", GetKindName(GetKind())));
		return std::nullopt;
	}
	return GetText();
}

uint32 HDataNode::GetCount () const noexcept
{
	if (Tree == nullptr)
	{
		return 0u;
	}
	const CDataTree::SNode& node = Tree->Nodes[Index];
	return (node.Kind == EDataNodeKind::Map || node.Kind == EDataNodeKind::Sequence) ? node.Count : 0u;
}

SDataChild HDataNode::GetChild (uint32 ChildIndex) const noexcept
{
	SDataChild child;
	if (Tree == nullptr)
	{
		return child;
	}
	const CDataTree::SNode& node = Tree->Nodes[Index];
	if (ChildIndex >= node.Count)
	{
		return child;
	}
	if (node.Kind == EDataNodeKind::Map)
	{
		const CDataTree::SEntry& entry = Tree->Entries[node.First + ChildIndex];
		child.Key = Tree->GetTextAt(entry.KeyOffset, entry.KeyLength);
		child.KeyLine = entry.KeyLine;
		child.KeyColumn = entry.KeyColumn;
		child.Value = HDataNode(Tree, entry.Value);
	}
	else if (node.Kind == EDataNodeKind::Sequence)
	{
		child.Value = HDataNode(Tree, Tree->Items[node.First + ChildIndex]);
	}
	return child;
}

HDataNode HDataNode::GetValue (uint32 ChildIndex) const noexcept
{
	return GetChild(ChildIndex).Value;
}

FStringView HDataNode::GetKey (uint32 ChildIndex) const noexcept
{
	return GetChild(ChildIndex).Key;
}

std::optional<uint32> HDataNode::FindKeyIndex (FStringView Key) const noexcept
{
	if (Tree == nullptr || Tree->Nodes[Index].Kind != EDataNodeKind::Map)
	{
		return std::nullopt;
	}
	return Tree->FindEntry(Tree->Nodes[Index], Key);
}

HDataNode HDataNode::FindChild (FStringView Key) const noexcept
{
	const std::optional<uint32> entry = FindKeyIndex(Key);
	return entry ? GetValue(*entry) : HDataNode();
}

HDataNode HDataNode::Find (FStringView Path) const noexcept
{
	HDataNode node = *this;
	if (Path.IsEmpty())
	{
		return node;
	}
	FStringView rest = Path;
	for (;;)
	{
		if (!node.IsValid())
		{
			return HDataNode();
		}
		const SStringSplit split = rest.SplitFirst('.');
		const FStringView segment = split.Left;
		if (node.IsMap())
		{
			node = node.FindChild(segment);
		}
		else if (node.IsSequence())
		{
			if (segment.IsEmpty() || !std::all_of(segment.begin(), segment.end(), IsDigit))
			{
				return HDataNode();
			}
			const std::optional<uint32> index = segment.ToUInt32();
			if (!index || *index >= node.GetCount())
			{
				return HDataNode();
			}
			node = node.GetValue(*index);
		}
		else
		{
			return HDataNode();
		}
		if (!split.bFound)
		{
			return node;
		}
		rest = split.Right;
	}
}


// ---------------- CDataTree ----------------

CDataTree::CDataTree () noexcept = default;
CDataTree::~CDataTree () = default;
CDataTree::CDataTree (const CDataTree& Other) = default;
CDataTree::CDataTree (CDataTree&& Other) noexcept = default;
CDataTree& CDataTree::operator= (const CDataTree& Other) = default;
CDataTree& CDataTree::operator= (CDataTree&& Other) noexcept = default;

bool CDataTree::IsEmpty () const noexcept
{
	return Nodes.empty();
}

HDataNode CDataTree::GetRoot () const noexcept
{
	return Nodes.empty() ? HDataNode() : HDataNode(this, 0u);
}

HDataNode CDataTree::Find (FStringView Path) const noexcept
{
	return GetRoot().Find(Path);
}

uint32 CDataTree::GetNodeCount () const noexcept
{
	return static_cast<uint32>(Nodes.size());
}

FStringView CDataTree::GetTextAt (uint32 Offset, uint32 Length) const noexcept
{
	return Length == 0u ? FStringView() : FStringView(Text.data() + Offset, Length);
}

std::optional<uint32> CDataTree::FindEntry (const SNode& Map, FStringView Key) const noexcept
{
	// Small maps (most of a rules file, a material): a linear scan beats the indirection.
	constexpr uint32 LinearScanLimit = 8u;
	if (Map.Count <= LinearScanLimit)
	{
		for (uint32 i = 0u; i < Map.Count; ++i)
		{
			const SEntry& entry = Entries[Map.First + i];
			if (GetTextAt(entry.KeyOffset, entry.KeyLength) == Key)
			{
				return i;
			}
		}
		return std::nullopt;
	}

	const uint32* const begin = SortedEntries.data() + Map.First;
	const uint32* const end = begin + Map.Count;
	const uint32* const found = std::lower_bound(begin, end, Key, [this] (uint32 EntryIndex, FStringView Wanted)
	{
		const SEntry& entry = Entries[EntryIndex];
		return CompareKeyBytes(GetTextAt(entry.KeyOffset, entry.KeyLength), Wanted) < 0;
	});
	if (found == end)
	{
		return std::nullopt;
	}
	const SEntry& entry = Entries[*found];
	if (GetTextAt(entry.KeyOffset, entry.KeyLength) != Key)
	{
		return std::nullopt;
	}
	return *found - Map.First;
}

bool CDataTree::operator== (const CDataTree& Other) const noexcept
{
	if (Nodes.size() != Other.Nodes.size() || Entries.size() != Other.Entries.size() || Items.size() != Other.Items.size())
	{
		return false;
	}
	// Builders number nodes in document order and lay children out when their parent closes, so equal trees have
	// equal node, entry and item arrays: compare them element by element, texts by content (pools may differ).
	for (size_t i = 0u; i < Nodes.size(); ++i)
	{
		const SNode& a = Nodes[i];
		const SNode& b = Other.Nodes[i];
		if (a.Kind != b.Kind || a.Style != b.Style || a.Line != b.Line || a.Column != b.Column || a.Count != b.Count)
		{
			return false;
		}
		switch (a.Kind)
		{
		case EDataNodeKind::Scalar:
		case EDataNodeKind::Raw:
			if (GetTextAt(a.First, a.Count) != Other.GetTextAt(b.First, b.Count))
			{
				return false;
			}
			break;
		case EDataNodeKind::Map:
		case EDataNodeKind::Sequence:
			if (a.First != b.First)
			{
				return false;
			}
			break;
		case EDataNodeKind::Null:
			break;
		}
	}
	for (size_t i = 0u; i < Entries.size(); ++i)
	{
		const SEntry& a = Entries[i];
		const SEntry& b = Other.Entries[i];
		if (a.Value != b.Value || a.KeyLine != b.KeyLine || a.KeyColumn != b.KeyColumn
			|| GetTextAt(a.KeyOffset, a.KeyLength) != Other.GetTextAt(b.KeyOffset, b.KeyLength))
		{
			return false;
		}
	}
	return Items == Other.Items;
}


// ---------------- Binary form ----------------
//
//	"FRTDTREE" magic, varint BinaryVersion, varint node count (0: an empty tree), then the root in pre-order:
//	node     = u8 (Kind | Style << 4), varint Line, varint Column, then by kind:
//	           Scalar, Raw: text (varint length + bytes)
//	           Map: varint count, then per entry: key text, varint KeyLine, varint KeyColumn, node
//	           Sequence: varint count, then the item nodes
//	           Null: nothing
//	Nothing may follow the root.

namespace
{
constexpr uint8 TreeMagic[8] = { 'F', 'R', 'T', 'D', 'T', 'R', 'E', 'E' };
}

void CDataTree::Serialize (std::vector<uint8>& Out) const
{
	using namespace assets::binary;
	AppendRaw(Out, TreeMagic);
	AppendVarint(Out, BinaryVersion);
	AppendVarint(Out, Nodes.size());
	if (Nodes.empty())
	{
		return;
	}

	// Pre-order with an explicit stack (node index, next child), so depth costs heap, not stack.
	struct SFrame
	{
		uint32 Node;
		uint32 NextChild;
	};
	std::vector<SFrame> stack;
	const auto writeNode = [&] (uint32 NodeIndex)
	{
		const SNode& node = Nodes[NodeIndex];
		Out.push_back(static_cast<uint8>(static_cast<uint8>(node.Kind) | (node.Style << 4u)));
		AppendVarint(Out, node.Line);
		AppendVarint(Out, node.Column);
		switch (node.Kind)
		{
		case EDataNodeKind::Scalar:
		case EDataNodeKind::Raw:
			AppendText(Out, GetTextAt(node.First, node.Count));
			break;
		case EDataNodeKind::Map:
		case EDataNodeKind::Sequence:
			AppendVarint(Out, node.Count);
			stack.push_back({ NodeIndex, 0u });
			break;
		case EDataNodeKind::Null:
			break;
		}
	};

	writeNode(0u);
	while (!stack.empty())
	{
		SFrame& frame = stack.back();
		const SNode& node = Nodes[frame.Node];
		if (frame.NextChild == node.Count)
		{
			stack.pop_back();
			continue;
		}
		const uint32 child = frame.NextChild++;
		if (node.Kind == EDataNodeKind::Map)
		{
			const SEntry& entry = Entries[node.First + child];
			AppendText(Out, GetTextAt(entry.KeyOffset, entry.KeyLength));
			AppendVarint(Out, entry.KeyLine);
			AppendVarint(Out, entry.KeyColumn);
			writeNode(entry.Value);   // may push: the reference `frame` is not used after this
		}
		else
		{
			writeNode(Items[node.First + child]);
		}
	}
}

bool CDataTree::Deserialize (std::span<const uint8> Bytes, SDiagnostic* OutError)
{
	*this = CDataTree();
	const auto fail = [OutError] (FStringView Message)
	{
		if (OutError != nullptr)
		{
			OutError->Severity = ESeverity::Error;
			OutError->Line = 0u;
			OutError->Column = 0u;
			OutError->Message = FString(Message);
		}
		return false;
	};

	assets::binary::CByteReader reader(Bytes);
	uint64 version = 0u;
	uint64 nodeCount = 0u;
	if (!reader.Expect(TreeMagic))
	{
		return fail("not a binary data tree (wrong magic)");
	}
	if (!reader.ReadVarint(version) || version != BinaryVersion)
	{
		return fail(FString::Format("binary data tree version {} is not the supported {}", version, BinaryVersion));
	}
	// Every node takes at least 3 bytes: a count past that cannot be honest.
	if (!reader.ReadVarint(nodeCount) || nodeCount > reader.GetRemaining() / 3u)
	{
		return fail("binary data tree: bad node count");
	}
	if (nodeCount == 0u)
	{
		return reader.IsAtEnd() ? true : fail("binary data tree: bytes after the end");
	}

	// Rebuilt through the builder, so a decoded tree obeys every rule a parsed one does (unique keys, depth). The
	// open collections live on a heap stack: hostile nesting fails at the builder's depth limit, never on the stack.
	struct SOpenCollection
	{
		uint64 Remaining;   // children still to read
		bool bMap;
	};
	CDataTreeBuilder builder;
	std::vector<SOpenCollection> open;
	uint64 nodesRead = 0u;

	const auto readPosition = [&reader] (uint32& OutLine, uint32& OutColumn)
	{
		uint64 line = 0u;
		uint64 column = 0u;
		if (!reader.ReadVarint(line) || !reader.ReadVarint(column) || line > 0xFFFFFFFFu || column > 0xFFFFFFFFu)
		{
			return false;
		}
		OutLine = static_cast<uint32>(line);
		OutColumn = static_cast<uint32>(column);
		return true;
	};

	// One node: its builder event, and for a map or sequence a new open collection. False with a message.
	FStringLiteral nodeError;
	const auto readNode = [&] ()
	{
		uint8 header = 0u;
		uint32 line = 0u;
		uint32 column = 0u;
		if (!reader.ReadU8(header) || !readPosition(line, column))
		{
			nodeError = "binary data tree: truncated node";
			return false;
		}
		++nodesRead;
		const uint8 kind = header & 0x0Fu;
		const uint8 style = static_cast<uint8>(header >> 4u);
		switch (kind)
		{
		case static_cast<uint8>(EDataNodeKind::Null):
			if (style != 0u)
			{
				break;
			}
			builder.Null(line, column);
			return true;
		case static_cast<uint8>(EDataNodeKind::Scalar):
		case static_cast<uint8>(EDataNodeKind::Raw):
		{
			const bool bRaw = kind == static_cast<uint8>(EDataNodeKind::Raw);
			if (bRaw ? style != 0u : style > static_cast<uint8>(EScalarStyle::Folded))
			{
				break;
			}
			FString text;
			if (!reader.ReadText(text))
			{
				nodeError = "binary data tree: truncated text";
				return false;
			}
			if (bRaw)
			{
				builder.Raw(text.View(), line, column);
			}
			else
			{
				builder.Scalar(text.View(), static_cast<EScalarStyle>(style), line, column);
			}
			return true;
		}
		case static_cast<uint8>(EDataNodeKind::Map):
		case static_cast<uint8>(EDataNodeKind::Sequence):
		{
			if (style > static_cast<uint8>(ECollectionStyle::Flow))
			{
				break;
			}
			uint64 count = 0u;
			if (!reader.ReadVarint(count) || count > reader.GetRemaining())
			{
				nodeError = "binary data tree: bad collection size";
				return false;
			}
			const bool bMap = kind == static_cast<uint8>(EDataNodeKind::Map);
			if (bMap)
			{
				builder.BeginMap(line, column, static_cast<ECollectionStyle>(style));
			}
			else
			{
				builder.BeginSequence(line, column, static_cast<ECollectionStyle>(style));
			}
			open.push_back({ count, bMap });
			return true;
		}
		default:
			break;
		}
		nodeError = "binary data tree: bad node header";
		return false;
	};

	if (!readNode())
	{
		return fail(nodeError);
	}
	while (!open.empty() && !builder.HasFailed())
	{
		SOpenCollection& top = open.back();
		if (top.Remaining == 0u)
		{
			open.pop_back();
			builder.End();
			continue;
		}
		--top.Remaining;
		if (top.bMap)
		{
			FString key;
			uint32 keyLine = 0u;
			uint32 keyColumn = 0u;
			if (!reader.ReadText(key) || !readPosition(keyLine, keyColumn))
			{
				return fail("binary data tree: truncated key");
			}
			builder.Key(key.View(), keyLine, keyColumn);
		}
		if (!readNode())   // may push: `top` is not used after this
		{
			return fail(nodeError);
		}
	}

	SDiagnostic error;
	CDataTree tree;
	if (!builder.Finish(tree, &error))
	{
		return fail(FString::Format("binary data tree: {}", error.Message));
	}
	if (nodesRead != nodeCount)
	{
		return fail("binary data tree: node count does not match");
	}
	if (!reader.IsAtEnd())
	{
		return fail("binary data tree: bytes after the end");
	}
	*this = std::move(tree);
	return true;
}


// ---------------- CDataTreeBuilder ----------------

CDataTreeBuilder::CDataTreeBuilder (uint32 InMaxDepth) noexcept
	: MaxDepth(InMaxDepth)
{}

CDataTreeBuilder::~CDataTreeBuilder () = default;

bool CDataTreeBuilder::IsExpectingKey () const noexcept
{
	return !Open.empty() && Open.back().bMap && !Open.back().bHasKey;
}

bool CDataTreeBuilder::SetError (FStringView Message, uint32 Line, uint32 Column)
{
	if (!bFailed)
	{
		bFailed = true;
		ErrorMessage = FString(Message);
		ErrorLine = Line;
		ErrorColumn = Column;
	}
	return false;
}

void CDataTreeBuilder::Fail (FStringView Message, uint32 Line, uint32 Column)
{
	SetError(Message, Line, Column);
}

uint32 CDataTreeBuilder::AddText (FStringView Text)
{
	const uint32 offset = static_cast<uint32>(Tree.Text.size());
	Tree.Text.insert(Tree.Text.end(), Text.begin(), Text.end());
	return offset;
}

uint32 CDataTreeBuilder::AddNode (EDataNodeKind Kind, uint8 Style, uint32 Line, uint32 Column)
{
	CDataTree::SNode node;
	node.Kind = Kind;
	node.Style = Style;
	node.Line = Line;
	node.Column = Column;
	Tree.Nodes.push_back(node);
	return static_cast<uint32>(Tree.Nodes.size() - 1u);
}

// Checks that a value may come here; the value itself is added by the caller, then FinishValue files it.
bool CDataTreeBuilder::BeginValue (uint32 Line, uint32 Column)
{
	if (bFailed)
	{
		return false;
	}
	if (Open.empty())
	{
		return bHasRoot ? SetError("a second value after the document's root", Line, Column) : true;
	}
	if (Open.back().bMap && !Open.back().bHasKey)
	{
		return SetError("a value where a map key is expected", Line, Column);
	}
	return true;
}

void CDataTreeBuilder::FinishValue (uint32 Node)
{
	if (Open.empty())
	{
		bHasRoot = true;
		return;
	}
	SOpen& parent = Open.back();
	if (parent.bMap)
	{
		CDataTree::SEntry entry = parent.Key;
		entry.Value = Node;
		PendingEntries.push_back(entry);
		parent.bHasKey = false;
	}
	else
	{
		PendingItems.push_back(Node);
	}
}

bool CDataTreeBuilder::BeginMap (uint32 Line, uint32 Column, ECollectionStyle Style)
{
	if (!BeginValue(Line, Column))
	{
		return false;
	}
	if (Open.size() >= MaxDepth)
	{
		return SetError(FString::Format("maps and sequences nested deeper than {}", MaxDepth), Line, Column);
	}
	const uint32 node = AddNode(EDataNodeKind::Map, static_cast<uint8>(Style), Line, Column);
	SOpen open;
	open.Node = node;
	open.PendingStart = static_cast<uint32>(PendingEntries.size());
	open.bMap = true;
	Open.push_back(open);
	return true;
}

bool CDataTreeBuilder::BeginSequence (uint32 Line, uint32 Column, ECollectionStyle Style)
{
	if (!BeginValue(Line, Column))
	{
		return false;
	}
	if (Open.size() >= MaxDepth)
	{
		return SetError(FString::Format("maps and sequences nested deeper than {}", MaxDepth), Line, Column);
	}
	const uint32 node = AddNode(EDataNodeKind::Sequence, static_cast<uint8>(Style), Line, Column);
	SOpen open;
	open.Node = node;
	open.PendingStart = static_cast<uint32>(PendingItems.size());
	open.bMap = false;
	Open.push_back(open);
	return true;
}

// Lays the map's entries out contiguously, orders its SortedEntries range and rejects a duplicate key.
bool CDataTreeBuilder::CloseMap (const SOpen& Map)
{
	const uint32 first = static_cast<uint32>(Tree.Entries.size());
	const uint32 count = static_cast<uint32>(PendingEntries.size() - Map.PendingStart);
	Tree.Entries.insert(Tree.Entries.end(), PendingEntries.begin() + Map.PendingStart, PendingEntries.end());
	PendingEntries.resize(Map.PendingStart);
	CDataTree::SNode& node = Tree.Nodes[Map.Node];
	node.First = first;
	node.Count = count;

	// Stable by key bytes: equal keys stay in source order, so the second of a run is the duplicate.
	SortScratch.resize(count);
	for (uint32 i = 0u; i < count; ++i)
	{
		SortScratch[i] = first + i;
	}
	const auto keyOf = [this] (uint32 EntryIndex)
	{
		const CDataTree::SEntry& entry = Tree.Entries[EntryIndex];
		return Tree.GetTextAt(entry.KeyOffset, entry.KeyLength);
	};
	std::stable_sort(SortScratch.begin(), SortScratch.end(), [&keyOf] (uint32 A, uint32 B)
	{
		return CompareKeyBytes(keyOf(A), keyOf(B)) < 0;
	});

	// In each run of equal keys the first is the original and the second the first repetition; report the
	// repetition that comes first in the source, whatever its key's order.
	uint32 duplicate = ~0u;
	uint32 original = ~0u;
	for (uint32 runStart = 0u; runStart < count;)
	{
		uint32 runEnd = runStart + 1u;
		while (runEnd < count && keyOf(SortScratch[runEnd]) == keyOf(SortScratch[runStart]))
		{
			++runEnd;
		}
		if (runEnd - runStart >= 2u && SortScratch[runStart + 1u] < duplicate)
		{
			duplicate = SortScratch[runStart + 1u];
			original = SortScratch[runStart];
		}
		runStart = runEnd;
	}
	if (duplicate != ~0u)
	{
		const CDataTree::SEntry& entry = Tree.Entries[duplicate];
		const CDataTree::SEntry& firstEntry = Tree.Entries[original];
		return SetError(FString::Format("duplicate key {} (first at line {}, column {})", QuoteForMessage(keyOf(duplicate)),
			firstEntry.KeyLine, firstEntry.KeyColumn), entry.KeyLine, entry.KeyColumn);
	}

	Tree.SortedEntries.resize(Tree.Entries.size());
	std::copy(SortScratch.begin(), SortScratch.end(), Tree.SortedEntries.begin() + first);
	return true;
}

bool CDataTreeBuilder::End ()
{
	if (bFailed)
	{
		return false;
	}
	if (Open.empty())
	{
		return SetError("End without an open map or sequence", 0u, 0u);
	}
	const SOpen open = Open.back();
	if (open.bMap && open.bHasKey)
	{
		return SetError(FString::Format("the key {} has no value", QuoteForMessage(Tree.GetTextAt(open.Key.KeyOffset, open.Key.KeyLength))),
			open.Key.KeyLine, open.Key.KeyColumn);
	}
	Open.pop_back();
	if (open.bMap)
	{
		if (!CloseMap(open))
		{
			return false;
		}
	}
	else
	{
		const uint32 first = static_cast<uint32>(Tree.Items.size());
		const uint32 count = static_cast<uint32>(PendingItems.size() - open.PendingStart);
		Tree.Items.insert(Tree.Items.end(), PendingItems.begin() + open.PendingStart, PendingItems.end());
		PendingItems.resize(open.PendingStart);
		CDataTree::SNode& node = Tree.Nodes[open.Node];
		node.First = first;
		node.Count = count;
	}
	FinishValue(open.Node);
	return true;
}

bool CDataTreeBuilder::Key (FStringView Key, uint32 Line, uint32 Column)
{
	if (bFailed)
	{
		return false;
	}
	if (!IsExpectingKey())
	{
		return SetError(FString::Format("the key {} is not inside a map that expects one", QuoteForMessage(Key)), Line, Column);
	}
	SOpen& map = Open.back();
	map.Key.KeyOffset = AddText(Key);
	map.Key.KeyLength = Key.Len();
	map.Key.KeyLine = Line;
	map.Key.KeyColumn = Column;
	map.bHasKey = true;
	return true;
}

bool CDataTreeBuilder::Scalar (FStringView Text, EScalarStyle Style, uint32 Line, uint32 Column)
{
	if (!BeginValue(Line, Column))
	{
		return false;
	}
	const uint32 node = AddNode(EDataNodeKind::Scalar, static_cast<uint8>(Style), Line, Column);
	Tree.Nodes[node].First = AddText(Text);
	Tree.Nodes[node].Count = Text.Len();
	FinishValue(node);
	return true;
}

bool CDataTreeBuilder::Null (uint32 Line, uint32 Column)
{
	if (!BeginValue(Line, Column))
	{
		return false;
	}
	FinishValue(AddNode(EDataNodeKind::Null, 0u, Line, Column));
	return true;
}

bool CDataTreeBuilder::Raw (FStringView Text, uint32 Line, uint32 Column)
{
	if (!BeginValue(Line, Column))
	{
		return false;
	}
	const uint32 node = AddNode(EDataNodeKind::Raw, 0u, Line, Column);
	Tree.Nodes[node].First = AddText(Text);
	Tree.Nodes[node].Count = Text.Len();
	FinishValue(node);
	return true;
}

bool CDataTreeBuilder::Subtree (HDataNode Node)
{
	if (!Node.IsValid())
	{
		return SetError("a subtree of no node", 0u, 0u);
	}
	const uint32 line = Node.GetLine();
	const uint32 column = Node.GetColumn();
	switch (Node.GetKind())
	{
	case EDataNodeKind::Null:
		return this->Null(line, column);
	case EDataNodeKind::Scalar:
		return Scalar(Node.GetText(), Node.GetScalarStyle(), line, column);
	case EDataNodeKind::Raw:
		return Raw(Node.GetText(), line, column);
	case EDataNodeKind::Map:
		BeginMap(line, column, Node.GetCollectionStyle());
		for (const SDataChild child : Node.GetChildren())
		{
			Key(child.Key, child.KeyLine, child.KeyColumn);
			Subtree(child.Value);
		}
		return End();
	case EDataNodeKind::Sequence:
		BeginSequence(line, column, Node.GetCollectionStyle());
		for (const SDataChild child : Node.GetChildren())
		{
			Subtree(child.Value);
		}
		return End();
	}
	return false;
}

void CDataTreeBuilder::Reset ()
{
	Tree = CDataTree();
	Open.clear();
	PendingEntries.clear();
	PendingItems.clear();
	bFailed = false;
	bHasRoot = false;
	ErrorLine = 0u;
	ErrorColumn = 0u;
	ErrorMessage.Clear();
}

bool CDataTreeBuilder::Finish (CDataTree& Out, SDiagnostic* OutError)
{
	if (!bFailed && !Open.empty())
	{
		const CDataTree::SNode& node = Tree.Nodes[Open.back().Node];
		SetError(Open.back().bMap ? "a map is not closed" : "a sequence is not closed", node.Line, node.Column);
	}
	if (bFailed)
	{
		if (OutError != nullptr)
		{
			OutError->Severity = ESeverity::Error;
			OutError->Line = ErrorLine;
			OutError->Column = ErrorColumn;
			OutError->Message = ErrorMessage;
		}
		Out = CDataTree();
		Reset();
		return false;
	}
	Tree.Text.shrink_to_fit();
	Out = std::move(Tree);
	Reset();
	return true;
}


// ---------------- IsSameContent ----------------

namespace
{
[[nodiscard]] bool IsSameNodeContent (const HDataNode& A, const HDataNode& B) noexcept
{
	if (A.IsNull() || B.IsNull())
	{
		return A.IsNull() && B.IsNull();
	}
	if (A.GetKind() != B.GetKind())
	{
		return false;
	}
	switch (A.GetKind())
	{
	case EDataNodeKind::Scalar:
		return A.GetText() == B.GetText()
			&& (A.GetScalarStyle() == EScalarStyle::Plain) == (B.GetScalarStyle() == EScalarStyle::Plain);
	case EDataNodeKind::Raw:
		return A.GetText() == B.GetText();
	case EDataNodeKind::Map:
	case EDataNodeKind::Sequence:
		if (A.GetCount() != B.GetCount())
		{
			return false;
		}
		for (uint32 i = 0u; i < A.GetCount(); ++i)
		{
			const SDataChild childA = A.GetChild(i);
			const SDataChild childB = B.GetChild(i);
			if (childA.Key != childB.Key || !IsSameNodeContent(childA.Value, childB.Value))
			{
				return false;
			}
		}
		return true;
	case EDataNodeKind::Null:
		break;
	}
	return true;
}
}

bool IsSameContent (const CDataTree& A, const CDataTree& B)
{
	if (A.IsEmpty() || B.IsEmpty())
	{
		return A.IsEmpty() && B.IsEmpty();
	}
	return IsSameNodeContent(A.GetRoot(), B.GetRoot());
}
}
