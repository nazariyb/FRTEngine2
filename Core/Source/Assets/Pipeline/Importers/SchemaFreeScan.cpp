#include "Assets/Pipeline/Importers/ImporterUtils.h"

#include <unordered_map>

#include "Containers/StringMap.h"
#include "Reflect/DataTree.h"
#include "Unicode/Ascii.h"


namespace frt::assets
{
namespace
{
using reflect::HDataNode;

constexpr FStringLiteral AssetKey = "$asset";
constexpr FStringLiteral NativeIdKey = "$id";
constexpr uint32 NativeIdTextLength = 17u;   // '$' + 16 hex digits
constexpr uint32 MaxRowKeyLength = 63u;

// "$3f9c0a7e5d21b864" -> the id; nullopt for anything else.
std::optional<uint64> ParseNativeId (FStringView Text) noexcept
{
	if (Text.Len() != NativeIdTextLength || Text[0] != '$')
	{
		return std::nullopt;
	}
	const FStringView digits = Text.RightChop(1u);
	for (uint32 i = 0u; i < digits.Len(); ++i)
	{
		if (!ascii::IsHexDigit(digits[i]))
		{
			return std::nullopt;
		}
	}
	return digits.ToUInt64(16u);
}

// §3.6: [A-Za-z_][A-Za-z0-9_.-]{0,62}.
bool IsValidRowKey (FStringView Key) noexcept
{
	if (Key.IsEmpty() || Key.Len() > MaxRowKeyLength || !(ascii::IsAlpha(Key[0]) || Key[0] == '_'))
	{
		return false;
	}
	for (uint32 i = 1u; i < Key.Len(); ++i)
	{
		const char ch = Key[i];
		if (!(ascii::IsAlnum(ch) || ch == '_' || ch == '.' || ch == '-'))
		{
			return false;
		}
	}
	return true;
}

FString JoinFieldPath (FStringView Parent, FStringView Child)
{
	if (Parent.IsEmpty())
	{
		return FString(Child);
	}
	FString path(Parent);
	path += '.';
	path += Child;
	return path;
}

class CSchemaFreeScan
{
public:
	CSchemaFreeScan (const SAnalyzeInput& InInput, SAnalyzeResult& InResult)
		: In(InInput)
		, Out(InResult)
	{}

	void Run (const reflect::CDataTree& Tree)
	{
		const HDataNode root = Tree.GetRoot();
		if (!root)
		{
			return;
		}
		const HDataNode type = root.FindChild("$type");
		const HDataNode rows = root.FindChild("Rows");
		if (!(type.IsScalar() && type.GetText() == "frt.DataTable" && rows.IsMap()))
		{
			Walk(root, FStringView(), 0u);
			return;
		}

		for (const reflect::SDataChild& child : root.GetChildren())
		{
			if (child.Value != rows)
			{
				Walk(child.Value, child.Key, 0u);
			}
		}
		for (const reflect::SDataChild& row : rows.GetChildren())
		{
			const FString fieldPath = JoinFieldPath("Rows", row.Key);
			Walk(row.Value, fieldPath, AddRow(row, fieldPath));
		}
	}

private:
	void Report (ESeverity Severity, const HDataNode& Node, FStringView FieldPath, FString Message, uint32 Line = 0u, uint32 Column = 0u)
	{
		SDiagnostic diagnostic = importers::MakeFileDiagnostic(In, Severity, std::move(Message));
		diagnostic.Line = Line != 0u ? Line : Node.GetLine();
		diagnostic.Column = Line != 0u ? Column : Node.GetColumn();
		diagnostic.FieldPath = FString(FieldPath);
		Out.Diagnostics.push_back(std::move(diagnostic));
	}

	// The row's sub-entry; its SubId, or 0 when the row gets none (the references inside it then come from the
	// record).
	uint64 AddRow (const reflect::SDataChild& Row, FStringView FieldPath)
	{
		if (!IsValidRowKey(Row.Key))
		{
			Report(ESeverity::Error, Row.Value, FieldPath, FString::Format("the row key '{}' is not [A-Za-z_][A-Za-z0-9_.-]{{0,62}}: the row is not registered",
				Row.Key), Row.KeyLine, Row.KeyColumn);
			return 0u;
		}
		const auto [caseTwin, bNewKey] = RowKeys.try_emplace(FString(Row.Key), FString(Row.Key));
		if (!bNewKey)
		{
			Report(ESeverity::Error, Row.Value, FieldPath, FString::Format("the row keys '{}' and '{}' differ only in case: the second is not registered",
				caseTwin->second, Row.Key), Row.KeyLine, Row.KeyColumn);
			return 0u;
		}

		uint64 subId = MakeSubId(FString::Format("row:{}", Row.Key));
		ERecordFlags flags = ERecordFlags::Schemaless;
		const HDataNode pin = Row.Value.FindChild(NativeIdKey);
		if (pin && !pin.IsNull())
		{
			const std::optional<uint64> pinned = pin.IsScalar() ? ParseNativeId(pin.GetText()) : std::nullopt;
			if (!pinned)
			{
				Report(ESeverity::Error, pin, JoinFieldPath(FieldPath, NativeIdKey), FString::Format("the row '{}' has a malformed $id pin (expected $ and 16 hex digits): the row is not registered",
					Row.Key));
				return 0u;
			}
			subId = *pinned != 0u ? *pinned : 1u;
			flags |= ERecordFlags::PinnedId;
		}

		const auto [existing, bNewId] = RowIds.try_emplace(subId, FString(Row.Key));
		if (!bNewId)
		{
			Report(ESeverity::Error, Row.Value, FieldPath, FString::Format("the rows '{}' and '{}' have the same id {:016x}: the second is not registered",
				existing->second, Row.Key, subId), Row.KeyLine, Row.KeyColumn);
			return 0u;
		}
		SSubEntryDecl& sub = Out.SubEntries.emplace_back();
		sub.Label = FString(Row.Key);
		sub.SubId = subId;
		sub.Type = asset_types::Unknown;
		sub.Flags = flags;
		return subId;
	}

	void Walk (const HDataNode& Node, FStringView FieldPath, uint64 FromSub)
	{
		if (Node.IsMap())
		{
			if (Node.FindChild(AssetKey))
			{
				AddReference(Node, FieldPath, FromSub);
				return;
			}
			for (const reflect::SDataChild& child : Node.GetChildren())
			{
				Walk(child.Value, JoinFieldPath(FieldPath, child.Key), FromSub);
			}
		}
		else if (Node.IsSequence())
		{
			for (uint32 i = 0u; i < Node.GetCount(); ++i)
			{
				Walk(Node.GetValue(i), JoinFieldPath(FieldPath, FString::Format("{}", i)), FromSub);
			}
		}
	}

	// A text value of the reference map, or nullopt (absent or null). A value that is not a scalar is reported.
	std::optional<FStringView> GetText (const HDataNode& Map, FStringView Key, FStringView FieldPath, bool& bMalformed)
	{
		const HDataNode node = Map.FindChild(Key);
		if (!node || node.IsNull())
		{
			return std::nullopt;
		}
		if (!node.IsScalar())
		{
			Report(ESeverity::Warning, node, FieldPath, FString::Format("the reference's '{}' is not text: the reference is not recorded", Key));
			bMalformed = true;
			return std::nullopt;
		}
		return node.GetText();
	}

	void AddReference (const HDataNode& Map, FStringView FieldPath, uint64 FromSub)
	{
		bool bMalformed = false;
		const std::optional<FStringView> guidText = GetText(Map, AssetKey, FieldPath, bMalformed);
		if (!guidText)
		{
			return;   // `$asset: null`: no reference (or reported)
		}
		const std::optional<SGuid> guid = SGuid::Parse(*guidText);
		const HDataNode guidNode = Map.FindChild(AssetKey);
		if (!guid || !guid->IsValid())
		{
			Report(ESeverity::Warning, guidNode, FieldPath, FString::Format("'{}' is not an asset GUID: the reference is not recorded", *guidText));
			return;
		}

		const std::optional<FStringView> sub = GetText(Map, "sub", FieldPath, bMalformed);
		const std::optional<FStringView> row = GetText(Map, "row", FieldPath, bMalformed);
		const std::optional<FStringView> rowId = GetText(Map, "id", FieldPath, bMalformed);
		const std::optional<FStringView> hint = GetText(Map, "path", FieldPath, bMalformed);
		if (bMalformed)
		{
			return;
		}

		SEdgeDecl edge;
		edge.FromSub = FromSub;
		edge.Target.Guid = *guid;
		edge.Flags = EDepFlags::Schemaless;
		edge.FieldPath = FString(FieldPath);
		if (sub && row)
		{
			Report(ESeverity::Warning, Map, FieldPath, "the reference has both 'sub' and 'row': it is not recorded");
			return;
		}
		if (sub)
		{
			std::optional<uint64> subId;
			if (sub->StartsWith('$'))
			{
				subId = ParseNativeId(*sub);
			}
			else if (sub->Contains(':'))
			{
				subId = MakeSubId(*sub);
			}
			if (!subId)
			{
				Report(ESeverity::Warning, Map.FindChild("sub"), FieldPath,
					FString::Format("the sub-asset '{}' is neither a label (kind:name) nor $ and 16 hex digits: the reference is not recorded", *sub));
				return;
			}
			edge.Target.SubId = *subId != 0u ? *subId : 1u;
		}
		else if (row)
		{
			std::optional<uint64> subId;
			if (rowId)
			{
				subId = ParseNativeId(*rowId);
				if (!subId)
				{
					Report(ESeverity::Warning, Map.FindChild("id"), FieldPath,
						FString::Format("the row id '{}' is not $ and 16 hex digits: the reference is not recorded", *rowId));
					return;
				}
			}
			else
			{
				subId = MakeSubId(FString::Format("row:{}", *row));
			}
			edge.Target.SubId = *subId != 0u ? *subId : 1u;
			edge.Flags |= EDepFlags::Row;
		}
		if (hint)
		{
			if (IsWellFormedMountPath(*hint))
			{
				edge.TargetPath = FString(*hint);
			}
			else
			{
				Report(ESeverity::Warning, Map.FindChild("path"), FieldPath, FString::Format("the path hint '{}' is not a mount path: it is ignored", *hint));
			}
		}
		Out.Edges.push_back(std::move(edge));
	}

	const SAnalyzeInput& In;
	SAnalyzeResult& Out;
	TStringMapIgnoreCase<FString> RowKeys;
	std::unordered_map<uint64, FString> RowIds;
};
}


void GatherSchemaFree (const reflect::CDataTree& Tree, const SAnalyzeInput& In, SAnalyzeResult& Out)
{
	CSchemaFreeScan(In, Out).Run(Tree);
}
}
