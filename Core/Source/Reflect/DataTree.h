#pragma once

#include <iterator>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt
{
struct SDiagnostic;
}


/**
 * A parsed text document as plain data (design §5.5, §15 I15): what the importers' Analyze, the rules engine and,
 * from M5, every reader of a data document walk instead of a parser's own node type.
 *
 * Why a tree of our own instead of yaml-cpp's YAML::Node:
 * - It keeps what a format needs and YAML::Node drops: each scalar's style (a quoted '1' is text, a plain 1 is a
 *   number), each node's source line and column (diagnostics point at file:line:column), the order of map keys (the
 *   canonical emitter, §3.7, writes them back in that order), and duplicate keys are an error, not silently merged.
 * - It is std containers and FString only (the CRT heap), so workers build and read it; YAML::Node and the TLSF
 *   containers are not for workers (§8.8).
 * - It isolates the parser: ParseYaml (DataTreeYaml.h) is one producer, the legacy line readers and the binary form
 *   (Serialize / Deserialize, the tree cache of §5.5) are others, and rapidyaml or an in-house parser could replace
 *   yaml-cpp behind it (§18 D7).
 *
 * The tree is immutable once built (CDataTreeBuilder::Finish), so any number of threads may read one tree at once.
 * Positions: Line and Column count from 1; 0 means unknown (a tree built without positions). **Column counts bytes
 * of the UTF-8 text, not characters**: in "ключ: 1" the value's column is 11 (2 bytes per Cyrillic letter + ": ").
 *
 * Module ABI from M5 (§6.3.1): the class is exported and its members are private; public signatures use only
 * FStringView, std::span, std::optional and the node handle, so the layout of the private std containers may change
 * without breaking callers that go through the accessors.
 */
namespace frt::reflect
{
class CDataTree;

// What a node is. Raw holds unparsed source text kept verbatim for a later writer (an unknown subtree it must
// preserve), never interpreted.
enum class EDataNodeKind : uint8
{
	Null,
	Scalar,
	Map,
	Sequence,
	Raw,
};

// How a scalar was written. The typed accessors read Plain scalars only: a quoted or block scalar is always text.
enum class EScalarStyle : uint8
{
	Plain,
	SingleQuoted,   // 'text'
	DoubleQuoted,   // "text"
	Literal,        // |
	Folded,         // >
};

// How a map or sequence was written: indented lines or { } / [ ].
enum class ECollectionStyle : uint8
{
	Block,
	Flow,
};

// One child of a map (Key and its position set) or a sequence (Key empty, KeyLine and KeyColumn 0).
struct SDataChild;

/**
 * A node of a CDataTree: the tree and an index into it, copied freely. Valid while its tree lives and is not
 * reassigned. A default-constructed handle names no node; every accessor of it returns the empty answer (kind Null,
 * no text, no children, every As* fails), so lookups chain without checks: Tree.Find("albedo.texture").AsText().
 */
class FRT_CORE_API HDataNode
{
public:
	constexpr HDataNode () noexcept = default;

	[[nodiscard]] constexpr bool IsValid () const noexcept { return Tree != nullptr; }
	[[nodiscard]] constexpr explicit operator bool () const noexcept { return Tree != nullptr; }

	[[nodiscard]] EDataNodeKind GetKind () const noexcept;
	[[nodiscard]] bool IsMap () const noexcept;
	[[nodiscard]] bool IsSequence () const noexcept;
	[[nodiscard]] bool IsScalar () const noexcept;   // any style
	[[nodiscard]] bool IsRaw () const noexcept;
	// A Null node (YAML: an empty value, ~ or null), or a Plain scalar that spells one ("", "~", "null") when a
	// builder other than ParseYaml wrote it as a scalar. False for an invalid handle.
	[[nodiscard]] bool IsNull () const noexcept;

	[[nodiscard]] uint32 GetLine () const noexcept;
	[[nodiscard]] uint32 GetColumn () const noexcept;   // bytes, from 1
	[[nodiscard]] EScalarStyle GetScalarStyle () const noexcept;           // Plain for anything but a scalar
	[[nodiscard]] ECollectionStyle GetCollectionStyle () const noexcept;   // Block for anything but a map or sequence
	// A scalar's text (after YAML's unescaping and folding) or a Raw node's source text; empty for anything else.
	[[nodiscard]] FStringView GetText () const noexcept;

	// Typed reads (YAML 1.2 core schema, strictly). Each fails - nullopt, with OutError's Line, Column and Message
	// filled and its Severity Error; the caller fills Code, File and Subject - instead of guessing:
	// AsBool: a Plain scalar, exactly "true" or "false" (no yes/no/on/off, no True).
	// AsInt64: a Plain scalar, an optional sign and decimal digits; a value outside int64 fails.
	// AsDouble: a Plain scalar in decimal or exponent form ("1", "-0.5", "2.5e-3", ".5"), or ".inf", "+.inf",
	//           "-.inf", ".nan"; a finite value outside double's range fails.
	// AsText: any scalar, whatever its style (a Null node is not text).
	[[nodiscard]] std::optional<bool> AsBool (SDiagnostic* OutError = nullptr) const;
	[[nodiscard]] std::optional<int64> AsInt64 (SDiagnostic* OutError = nullptr) const;
	[[nodiscard]] std::optional<double> AsDouble (SDiagnostic* OutError = nullptr) const;
	[[nodiscard]] std::optional<FStringView> AsText (SDiagnostic* OutError = nullptr) const;

	// Children: a map's entries or a sequence's items, in source order; 0 and invalid handles for anything else.
	[[nodiscard]] uint32 GetCount () const noexcept;
	[[nodiscard]] SDataChild GetChild (uint32 Index) const noexcept;
	[[nodiscard]] HDataNode GetValue (uint32 Index) const noexcept;   // a map value or a sequence item
	[[nodiscard]] FStringView GetKey (uint32 Index) const noexcept;   // empty for a sequence

	// The value of a map's key, matched byte for byte (keys are case-sensitive); invalid if absent or not a map.
	[[nodiscard]] HDataNode FindChild (FStringView Key) const noexcept;
	// The entry index of the key in a map, nullopt if absent or not a map.
	[[nodiscard]] std::optional<uint32> FindKeyIndex (FStringView Key) const noexcept;
	/**
	 * A path of '.'-separated segments below this node: a segment names a map key, or, on a sequence, an item index
	 * in decimal. "albedo.texture" is the "texture" key of the "albedo" map; "Rules.0.Match" the "Match" key of the
	 * first item of "Rules". An empty path is this node. A key that itself contains '.' is reached with FindChild.
	 */
	[[nodiscard]] HDataNode Find (FStringView Path) const noexcept;

	class CChildIterator;
	class CChildRange;
	// for (const SDataChild& child : Node.GetChildren()) - in source order.
	[[nodiscard]] CChildRange GetChildren () const noexcept;

	// The same node of the same tree.
	[[nodiscard]] constexpr bool operator== (const HDataNode& Other) const noexcept = default;

private:
	friend class CDataTree;

	constexpr HDataNode (const CDataTree* InTree, uint32 InIndex) noexcept
		: Tree(InTree)
		, Index(InIndex)
	{}

	const CDataTree* Tree = nullptr;
	uint32 Index = 0u;
};

struct SDataChild
{
	FStringView Key;
	uint32 KeyLine = 0u;
	uint32 KeyColumn = 0u;   // bytes, from 1
	HDataNode Value;
};

class HDataNode::CChildIterator
{
public:
	using iterator_category = std::forward_iterator_tag;
	using value_type = SDataChild;
	using difference_type = std::ptrdiff_t;
	using pointer = void;
	using reference = SDataChild;

	CChildIterator () noexcept = default;
	CChildIterator (HDataNode InParent, uint32 InIndex) noexcept
		: Parent(InParent)
		, Index(InIndex)
	{}

	[[nodiscard]] SDataChild operator* () const noexcept { return Parent.GetChild(Index); }
	CChildIterator& operator++ () noexcept { ++Index; return *this; }
	CChildIterator operator++ (int) noexcept { CChildIterator previous = *this; ++Index; return previous; }
	[[nodiscard]] bool operator== (const CChildIterator& Other) const noexcept { return Index == Other.Index && Parent == Other.Parent; }

private:
	HDataNode Parent;
	uint32 Index = 0u;
};

class HDataNode::CChildRange
{
public:
	explicit CChildRange (HDataNode InParent) noexcept
		: Parent(InParent)
	{}

	[[nodiscard]] CChildIterator begin () const noexcept { return CChildIterator(Parent, 0u); }
	[[nodiscard]] CChildIterator end () const noexcept { return CChildIterator(Parent, Parent.GetCount()); }
	[[nodiscard]] uint32 size () const noexcept { return Parent.GetCount(); }
	[[nodiscard]] bool empty () const noexcept { return Parent.GetCount() == 0u; }

private:
	HDataNode Parent;
};

inline HDataNode::CChildRange HDataNode::GetChildren () const noexcept
{
	return CChildRange(*this);
}


/**
 * The tree. Empty (no root) when default-constructed, when a YAML text holds no document, or after a failed
 * Deserialize. Built by CDataTreeBuilder; copyable and movable; never modified in place.
 *
 * Binary form (the DDC tree cache of §5.5, keyed by content hash and ParserVersion): Serialize appends a versioned,
 * self-delimiting encoding of every node, style, position, key and text (assets::binary varints, so it is the same
 * bytes on every machine); Deserialize rebuilds an equal tree (operator==) and rejects anything malformed - a wrong
 * magic or BinaryVersion, a truncated or trailing byte, a duplicate key, nesting past the builder's depth limit -
 * without reading out of bounds or allocating from an unchecked count.
 */
class FRT_CORE_API CDataTree
{
public:
	// The binary encoding's version: bumped whenever Serialize's bytes change.
	static constexpr uint32 BinaryVersion = 1u;
	// What ParseYaml makes of a text: bumped whenever the same text would give a different tree (a new rule, a
	// changed position or style). Part of the tree cache's key, with the content hash.
	static constexpr uint32 ParserVersion = 1u;

	CDataTree () noexcept;
	~CDataTree ();
	CDataTree (const CDataTree& Other);
	CDataTree (CDataTree&& Other) noexcept;
	CDataTree& operator= (const CDataTree& Other);
	CDataTree& operator= (CDataTree&& Other) noexcept;

	[[nodiscard]] bool IsEmpty () const noexcept;
	// The document's root; an invalid handle for an empty tree.
	[[nodiscard]] HDataNode GetRoot () const noexcept;
	// GetRoot().Find(Path).
	[[nodiscard]] HDataNode Find (FStringView Path) const noexcept;
	[[nodiscard]] uint32 GetNodeCount () const noexcept;

	// Appends the binary form to Out.
	void Serialize (std::vector<uint8>& Out) const;
	// Replaces this tree with the one Bytes encode. False (this tree left empty, OutError's Message set) when Bytes
	// are not exactly one well-formed encoding of BinaryVersion.
	bool Deserialize (std::span<const uint8> Bytes, SDiagnostic* OutError = nullptr);

	// Same nodes, kinds, styles, texts, keys and positions, in the same order.
	[[nodiscard]] bool operator== (const CDataTree& Other) const noexcept;

private:
	friend class HDataNode;
	friend class CDataTreeBuilder;

	// Scalar and Raw: First/Count = the text's offset and length in Text. Map: First/Count = its entries in Entries
	// (and the same range of SortedEntries). Sequence: First/Count = its items in Items. Null: unused.
	struct SNode
	{
		uint32 Line = 0u;
		uint32 Column = 0u;
		uint32 First = 0u;
		uint32 Count = 0u;
		EDataNodeKind Kind = EDataNodeKind::Null;
		uint8 Style = 0u;   // EScalarStyle or ECollectionStyle
	};

	struct SEntry
	{
		uint32 KeyOffset = 0u;
		uint32 KeyLength = 0u;
		uint32 KeyLine = 0u;
		uint32 KeyColumn = 0u;
		uint32 Value = 0u;   // node index
	};

	[[nodiscard]] FStringView GetTextAt (uint32 Offset, uint32 Length) const noexcept;
	[[nodiscard]] std::optional<uint32> FindEntry (const SNode& Map, FStringView Key) const noexcept;

	std::vector<SNode> Nodes;              // Nodes[0] is the root; a parent's index is below its children's
	std::vector<SEntry> Entries;           // each map's entries contiguous, in source order
	std::vector<uint32> SortedEntries;     // per map, the same range of Entries indices ordered by key bytes
	std::vector<uint32> Items;             // each sequence's items contiguous (node indices)
	std::vector<char> Text;                // every key and scalar's bytes
};


/**
 * Builds a CDataTree from a stream of events, in document order: what ParseYaml, the legacy line readers of the
 * importers (they are not YAML) and Deserialize produce trees with, so every tree obeys the same rules.
 *
 * A value is the root, a sequence item, or the value after a Key inside a map. Inside a map, Key comes before each
 * value. End closes the innermost map or sequence. The first misuse or rule violation fails the builder: every
 * later call returns false and changes nothing, and Finish reports that first error. Rules:
 * - one root value; a map's keys are unique (byte for byte; reported at the second occurrence, when the map ends);
 * - nesting is at most MaxDepth maps and sequences deep (hostile input: "[[[[..." fails here, not on the stack);
 * - Key only where a map expects one; a value never where a key is due.
 */
class FRT_CORE_API CDataTreeBuilder
{
public:
	static constexpr uint32 DefaultMaxDepth = 128u;

	explicit CDataTreeBuilder (uint32 MaxDepth = DefaultMaxDepth) noexcept;
	~CDataTreeBuilder ();
	CDataTreeBuilder (const CDataTreeBuilder&) = delete;
	CDataTreeBuilder& operator= (const CDataTreeBuilder&) = delete;

	bool BeginMap (uint32 Line, uint32 Column, ECollectionStyle Style = ECollectionStyle::Block);
	bool BeginSequence (uint32 Line, uint32 Column, ECollectionStyle Style = ECollectionStyle::Block);
	bool End ();
	bool Key (FStringView Key, uint32 Line, uint32 Column);
	bool Scalar (FStringView Text, EScalarStyle Style, uint32 Line, uint32 Column);
	bool Null (uint32 Line, uint32 Column);
	bool Raw (FStringView Text, uint32 Line, uint32 Column);
	// Node and everything below it as one value: styles and positions as they are in Node's tree, so a tree is cut and
	// reassembled (a meta keeps its Settings subtree) without a second walk in each caller. An invalid handle fails.
	bool Subtree (HDataNode Node);

	// Fails the builder from outside (a producer's own error), at that position. Ignored once failed.
	void Fail (FStringView Message, uint32 Line, uint32 Column);

	[[nodiscard]] bool HasFailed () const noexcept { return bFailed; }
	// Open maps and sequences.
	[[nodiscard]] uint32 GetDepth () const noexcept { return static_cast<uint32>(Open.size()); }
	// True inside a map whose next event must be a Key.
	[[nodiscard]] bool IsExpectingKey () const noexcept;

	/**
	 * Moves the tree into Out and resets the builder for another tree. False (Out left empty, OutError filled with
	 * the first error's Line, Column and Message) when the builder failed or a map or sequence is still open. No
	 * event at all gives an empty tree.
	 */
	bool Finish (CDataTree& Out, SDiagnostic* OutError = nullptr);

private:
	struct SOpen
	{
		uint32 Node = 0u;
		uint32 PendingStart = 0u;   // first of its entries in PendingEntries, or of its items in PendingItems
		bool bMap = false;
		bool bHasKey = false;       // a map whose Key awaits its value
		CDataTree::SEntry Key;
	};

	bool BeginValue (uint32 Line, uint32 Column);
	uint32 AddNode (EDataNodeKind Kind, uint8 Style, uint32 Line, uint32 Column);
	uint32 AddText (FStringView Text);
	void FinishValue (uint32 Node);
	bool CloseMap (const SOpen& Map);
	bool SetError (FStringView Message, uint32 Line, uint32 Column);
	void Reset ();

	CDataTree Tree;
	std::vector<SOpen> Open;
	std::vector<CDataTree::SEntry> PendingEntries;
	std::vector<uint32> PendingItems;
	std::vector<uint32> SortScratch;
	uint32 MaxDepth = DefaultMaxDepth;
	bool bFailed = false;
	bool bHasRoot = false;
	uint32 ErrorLine = 0u;
	uint32 ErrorColumn = 0u;
	FString ErrorMessage;
};


/**
 * Whether two trees say the same thing, wherever and however it was written: the same kinds, keys, key order and texts,
 * and each scalar plain in both or not plain in both, since that is what decides its type (a quoted '1' is text). Unlike
 * CDataTree::operator==, it ignores positions, flow or block, and which kind of quote: what the canonical emitter's tests
 * ask of a re-parse (design §3.7), which never sits on the lines the original did. Any spelling of null equals any other
 * (HDataNode::IsNull). Two empty trees are the same; a Raw node equals a Raw node of the same text.
 */
[[nodiscard]] FRT_CORE_API bool IsSameContent (const CDataTree& A, const CDataTree& B);
}
