#include "Assets/Registry/QueryEngine.h"

#include <algorithm>
#include <bit>
#include <iterator>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetTypes.h"
#include "Assets/Registry/RegistryTables.h"
#include "Assets/Registry/RegistryView.h"
#include "Platform/Timestamp.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral SystemColumnNames[] = {
	"$Type", "$Name", "$Path", "$Folder", "$Labels", "$Size", "$ImportStatus", "$Problems", "$Referencers", "$Dependencies",
	"$CookRule", "$Chunk", "$PrimaryName",
};
static_assert(std::size(SystemColumnNames) == SystemColumnCount);
}


FStringLiteral GetSystemColumnName (ESystemColumn Column) noexcept
{
	static constexpr FStringLiteral Unknown = "$Unknown";
	const uint32 index = static_cast<uint32>(Column);
	return index < SystemColumnCount ? SystemColumnNames[index] : Unknown;
}

std::optional<ESystemColumn> FindSystemColumn (FStringView Name) noexcept
{
	for (uint32 i = 0u; i < SystemColumnCount; ++i)
	{
		if (Name.Equals<ESearchCase::IgnoreCaseAscii>(SystemColumnNames[i]))
		{
			return static_cast<ESystemColumn>(i);
		}
	}
	return std::nullopt;
}


// ---------------- The plan ----------------

namespace
{
/**
 * How a compiled comparison reads a value. Int and Real compare numbers (an Int64 key against a fractional literal
 * compares as Real), Bool and Bits compare for equality (Bits: enums, references and the enum system columns as
 * ordinals), TypeId compares $Type by id, Text compares folded text bytes (FString::FoldCase on both sides).
 */
enum class EValueKind : uint8 { Int, Real, Bool, Bits, TypeId, Text };

// ECompare without Match (lowered by type at compile).
enum class EOp : uint8 { Eq, Ne, Lt, Le, Gt, Ge, In, Prefix, Contains, Exists, Missing };

struct SCompiledValues
{
	std::vector<int64> Ints;
	std::vector<double> Reals;
	std::vector<uint64> Bits;       // Bool, Bits, TypeId
	std::vector<FString> Texts;     // folded
};

// A tag predicate bound to one type's key.
struct SColumnTest
{
	uint32 Key = 0u;
	ETagType KeyType = ETagType::None;
	bool bMulti = false;
	EOp Op = EOp::Eq;
	EValueKind Kind = EValueKind::Int;
	SCompiledValues Values;
};

struct SSystemTest
{
	ESystemColumn Column = ESystemColumn::Type;
	EOp Op = EOp::Eq;
	EValueKind Kind = EValueKind::Int;
	SCompiledValues Values;
	bool bCheap = false;            // one value read straight from a column: tested in the scan loop (IsCheapColumn)
};

// The system columns that hold exactly one value read from one or two columns.
[[nodiscard]] bool IsCheapColumn (ESystemColumn Column) noexcept
{
	switch (Column)
	{
	case ESystemColumn::Type:
	case ESystemColumn::Name:
	case ESystemColumn::Size:
	case ESystemColumn::ImportStatus:
	case ESystemColumn::CookRule:
	case ESystemColumn::Chunk:
		return true;
	default:
		return false;
	}
}

// A folder of a dir: term: its subtree (the mount's pre-order interval) or the folder alone.
struct SFolderRange
{
	MountId Mount = InvalidMountId;
	uint32 Begin = 0u;
	uint32 End = 0u;
	uint32 Row = InvalidRow;
	bool bRecursive = true;
};

struct SLabelTest
{
	uint32 ExactId = InvalidRow;    // NAMES id of the label as written, when interned
	FString Folded;
	bool bExclude = false;
};

struct SGraphSpec
{
	EQueryGraphDirection Direction = EQueryGraphDirection::References;
	bool bTransitive = false;
	EDepFlags Mask {};
	uint32 TargetAsset = InvalidRow;    // ASSETS row of the target's file
	uint64 TargetSub = 0u;              // the target sub-entry's SubId; 0 = the asset record
};

/**
 * One SAssetQuery (the root or an AnyOf alternative). AllowedTypes folds the type terms of this node and its
 * ancestors and the keys of its tag predicates: a record of another type fails the node before anything else is
 * read. TypeTests[t] holds the node's tag predicates bound to type t's keys.
 */
struct SQueryNode
{
	std::vector<uint8> AllowedTypes;
	std::vector<std::vector<SColumnTest>> TypeTests;
	std::vector<SSystemTest> SystemTests;
	std::vector<SFolderRange> Folders;
	std::vector<SLabelTest> Labels;
	std::vector<uint32> Guids;          // sorted record values
	std::vector<uint32> TableOwners;    // sorted ASSETS rows
	std::vector<FString> Texts;         // folded
	std::vector<uint32> Graph;          // indices into SQueryPlan::Graph
	std::vector<ESystemPredicate> Is;
	std::vector<std::vector<SQueryNode>> AnyOf;
	bool bHasGuids = false;
	bool bHasTables = false;
	bool bMatchesNothing = false;
	bool bHasRowTests = false;          // anything but types and tag predicates
	bool bOnlyCheapRowTests = false;    // every row test is a cheap system test (PassesCheapTests is all of them)
};

enum class EOrderKind : uint8 { None, Tag, System };

struct SOrderSpec
{
	EOrderKind Kind = EOrderKind::None;
	ESystemColumn Column = ESystemColumn::Type;
	std::vector<uint32> KeyPerType;     // Tag: the key index per TypeId, InvalidRow when the type has none
	bool bAscending = true;
};
}

struct SQueryPlan
{
	SQueryNode Root;
	ERecordScope Records = ERecordScope::Assets;
	std::vector<uint8> AdmittedSubAssets;   // Records Assets: sub-assets of these types are in scope (named types)
	std::vector<SGraphSpec> Graph;
	std::vector<FString> FoldedTypeNames;   // per TypeId, for $Type prefix / contains
	SOrderSpec Order;
	uint32 Limit = 0u;
	uint32 Offset = 0u;
	FString Description;
};


namespace
{
[[nodiscard]] uint32 GetTypeCount () noexcept
{
	return static_cast<uint32>(CAssetTypeRegistry::GetTypes().size());
}

[[nodiscard]] FString Fold (FStringView Text)
{
	return FString(Text).FoldCase();
}

// A sub-entry is a table row when its label holds no ':' (row keys never do, §3.6); importer labels are "kind:name".
[[nodiscard]] bool IsRowLabel (FStringView Label) noexcept
{
	return !Label.Contains(':');
}

[[nodiscard]] FStringLiteral GetOpName (EOp Op) noexcept
{
	switch (Op)
	{
	case EOp::Eq: return "Eq";
	case EOp::Ne: return "Ne";
	case EOp::Lt: return "Lt";
	case EOp::Le: return "Le";
	case EOp::Gt: return "Gt";
	case EOp::Ge: return "Ge";
	case EOp::In: return "In";
	case EOp::Prefix: return "Prefix";
	case EOp::Contains: return "Contains";
	case EOp::Exists: return "Exists";
	case EOp::Missing: return "Missing";
	}
	return "?";
}

void AppendValues (FString& Out, EValueKind Kind, const SCompiledValues& Values)
{
	const auto separator = [&] (size_t Index) { Out += Index == 0u ? " " : ","; };
	switch (Kind)
	{
	case EValueKind::Int:
		for (size_t i = 0u; i < Values.Ints.size(); ++i)
		{
			separator(i);
			Out.AppendNumber(Values.Ints[i]);
		}
		return;
	case EValueKind::Real:
		for (size_t i = 0u; i < Values.Reals.size(); ++i)
		{
			separator(i);
			Out += "real:";
			Out.AppendNumber(Values.Reals[i]);
		}
		return;
	case EValueKind::Bool:
		for (size_t i = 0u; i < Values.Bits.size(); ++i)
		{
			separator(i);
			Out += Values.Bits[i] != 0u ? "true" : "false";
		}
		return;
	case EValueKind::Bits:
		for (size_t i = 0u; i < Values.Bits.size(); ++i)
		{
			separator(i);
			Out.AppendFormat("bits:{}", Values.Bits[i]);
		}
		return;
	case EValueKind::TypeId:
		for (size_t i = 0u; i < Values.Bits.size(); ++i)
		{
			separator(i);
			Out += CAssetTypeRegistry::Get(static_cast<reflect::TypeId>(Values.Bits[i])).Name;
		}
		return;
	case EValueKind::Text:
		for (size_t i = 0u; i < Values.Texts.size(); ++i)
		{
			separator(i);
			Out.AppendFormat("\"{}\"", Values.Texts[i]);
		}
		return;
	}
}

void AppendSeparator (FString& Out)
{
	if (!Out.IsEmpty())
	{
		Out += ' ';
	}
}


// ---------------- Compiler ----------------

// The types a node's terms resolve against: its own type terms and its ancestors' (all types when none names one).
struct STypeScope
{
	std::vector<uint8> Types;
	bool bExplicit = false;
};

class CQueryCompiler
{
public:
	CQueryCompiler (const CRegistryView& InView, SQueryPlan& InPlan, std::vector<SDiagnostic>& InNotes) noexcept
		: View(InView)
		, Tables(GetTables(InView))
		, Plan(InPlan)
		, Notes(InNotes)
	{
	}

	bool Compile (const SAssetQuery& Query)
	{
		const uint32 typeCount = GetTypeCount();
		Plan.Records = Query.Records;
		Plan.AdmittedSubAssets.assign(typeCount, 0u);
		Plan.FoldedTypeNames.reserve(typeCount);
		for (const SAssetTypeInfo& type : CAssetTypeRegistry::GetTypes())
		{
			Plan.FoldedTypeNames.push_back(Fold(type.Name));
		}

		Plan.Description.AppendFormat("records:{}", enum_::ToString(Query.Records));
		STypeScope all;
		all.Types.assign(typeCount, 1u);
		STypeScope rootScope;
		if (!CompileNode(Query, all, Plan.Root, Plan.Description, rootScope))
		{
			return false;
		}
		if (Query.Order && !CompileOrder(*Query.Order, rootScope))
		{
			return false;
		}
		Plan.Limit = Query.LimitCount;
		Plan.Offset = Query.OffsetCount;
		if (Plan.Limit != 0u || Plan.Offset != 0u)
		{
			Plan.Description.AppendFormat(" limit:{}", Plan.Limit);
			if (Plan.Offset != 0u)
			{
				Plan.Description.AppendFormat(" offset:{}", Plan.Offset);
			}
		}
		return true;
	}

	void FillError (SDiagnostic& Out) const
	{
		Out = SDiagnostic {};
		Out.Severity = ESeverity::Error;
		Out.Line = 1u;
		Out.Column = ErrorColumn;
		Out.Message = ErrorMessage;
	}

private:
	const CRegistryView& View;
	const SRegistryTables& Tables;
	SQueryPlan& Plan;
	std::vector<SDiagnostic>& Notes;
	uint32 ErrorColumn = 0u;
	FString ErrorMessage;

	bool Fail (uint32 Column, FString Message)
	{
		ErrorColumn = Column;
		ErrorMessage = std::move(Message);
		return false;
	}

	void Note (uint32 Column, FString Message)
	{
		SDiagnostic note;
		note.Severity = ESeverity::Info;
		note.Line = 1u;
		note.Column = Column;
		note.Message = std::move(Message);
		Notes.push_back(std::move(note));
	}

	bool CompileNode (const SAssetQuery& Query, const STypeScope& Parent, SQueryNode& Out, FString& Describe, STypeScope& OutScope)
	{
		const uint32 typeCount = GetTypeCount();

		// Types: a node's own terms OR together and AND with its ancestors'.
		STypeScope scope = Parent;
		if (!Query.Types.empty())
		{
			std::vector<uint8> own(typeCount, 0u);
			AppendSeparator(Describe);
			Describe += "types:";
			for (size_t i = 0u; i < Query.Types.size(); ++i)
			{
				const SQueryType& term = Query.Types[i];
				reflect::TypeId type = 0u;
				if (!ResolveType(term, type))
				{
					return false;
				}
				own[type] = 1u;
				Plan.AdmittedSubAssets[type] = 1u;
				Describe += i == 0u ? "" : ",";
				Describe += CAssetTypeRegistry::Get(type).Name;
			}
			for (uint32 t = 0u; t < typeCount; ++t)
			{
				scope.Types[t] = scope.Types[t] != 0u && own[t] != 0u ? 1u : 0u;
			}
			scope.bExplicit = true;
		}
		OutScope = scope;
		Out.AllowedTypes = scope.Types;
		Out.TypeTests.resize(typeCount);

		for (const SQueryPredicate& predicate : Query.Predicates)
		{
			if (!CompilePredicate(predicate, scope, Out, Describe))
			{
				return false;
			}
		}

		for (const SQueryFolder& folder : Query.Folders)
		{
			if (!CompileFolder(folder, Out, Describe))
			{
				return false;
			}
		}

		for (const SQueryLabel& label : Query.Labels)
		{
			SLabelTest test;
			const std::optional<uint32> id = Tables.Names.Find(label.Label.View());
			test.ExactId = id.value_or(InvalidRow);
			test.Folded = Fold(label.Label.View());
			test.bExclude = label.bExclude;
			AppendSeparator(Describe);
			Describe.AppendFormat("{}label:{}", label.bExclude ? "-" : "", test.Folded);
			Out.Labels.push_back(std::move(test));
		}

		if (!Query.Guids.empty())
		{
			Out.bHasGuids = true;
			for (const SAssetId& id : Query.Guids)
			{
				// An id that names no record (removed since the caller kept it) matches nothing.
				if (const std::optional<SRecordId> record = View.Find(id))
				{
					Out.Guids.push_back(record->Value);
				}
			}
			std::sort(Out.Guids.begin(), Out.Guids.end());
			Out.Guids.erase(std::unique(Out.Guids.begin(), Out.Guids.end()), Out.Guids.end());
			AppendSeparator(Describe);
			Describe.AppendFormat("ids:{}", Out.Guids.size());
		}

		if (!Query.Tables.empty())
		{
			Out.bHasTables = true;
			for (const SQueryTable& table : Query.Tables)
			{
				uint32 value = InvalidRow;
				if (!ResolveTarget(table.Name.View(), table.Table, table.Column, "table", value))
				{
					return false;
				}
				if ((value & SubEntryBit) != 0u)
				{
					return Fail(table.Column, FString::Format("table:{} names a sub-entry; a table is a file", table.Name));
				}
				Out.TableOwners.push_back(value);
			}
			std::sort(Out.TableOwners.begin(), Out.TableOwners.end());
			AppendSeparator(Describe);
			Describe.AppendFormat("tables:{}", Out.TableOwners.size());
		}

		for (const SQueryText& text : Query.TextTerms)
		{
			Out.Texts.push_back(Fold(text.Text.View()));
			AppendSeparator(Describe);
			Describe.AppendFormat("text:\"{}\"", Out.Texts.back());
		}

		for (const SQueryGraph& graph : Query.Graph)
		{
			if (!CompileGraph(graph, Out, Describe))
			{
				return false;
			}
		}

		for (const SQuerySystemPredicate& system : Query.SystemPredicates)
		{
			AppendSeparator(Describe);
			Describe.AppendFormat("is:{}", GetSystemPredicateName(system.Predicate));
			if (!HasSystemPredicateData(system.Predicate))
			{
				Out.bMatchesNothing = true;
				Note(system.Column, FString::Format("is:{} matches nothing until its milestone", GetSystemPredicateName(system.Predicate)));
				continue;
			}
			Out.Is.push_back(system.Predicate);
		}

		for (const SQueryAnyOf& group : Query.AnyOfGroups)
		{
			std::vector<SQueryNode>& alternatives = Out.AnyOf.emplace_back();
			AppendSeparator(Describe);
			Describe += "any(";
			for (size_t i = 0u; i < group.Alternatives.size(); ++i)
			{
				Describe += i == 0u ? "" : " | ";
				FString inner;
				STypeScope innerScope;
				if (!CompileNode(group.Alternatives[i], scope, alternatives.emplace_back(), inner, innerScope))
				{
					return false;
				}
				Describe += inner;
			}
			Describe += ')';
		}

		const bool bOtherRowTests = !Out.Folders.empty() || !Out.Labels.empty() || Out.bHasGuids || Out.bHasTables || !Out.Texts.empty()
			|| !Out.Graph.empty() || !Out.Is.empty() || !Out.AnyOf.empty();
		const bool bAllCheap = std::all_of(Out.SystemTests.begin(), Out.SystemTests.end(), [] (const SSystemTest& Test) { return Test.bCheap; });
		Out.bHasRowTests = !Out.SystemTests.empty() || bOtherRowTests;
		Out.bOnlyCheapRowTests = Out.bHasRowTests && !bOtherRowTests && bAllCheap;
		return true;
	}

	bool ResolveType (const SQueryType& Term, reflect::TypeId& OutType)
	{
		if (const std::optional<reflect::TypeId> type = CAssetTypeRegistry::Find(Term.Name.View()))
		{
			OutType = *type;
			return true;
		}
		FString shortNames;
		uint32 matches = 0u;
		for (const SAssetTypeInfo& type : CAssetTypeRegistry::GetTypes())
		{
			if (SStringEqualIgnoreCase {}(Term.Name.View(), type.ShortName))
			{
				shortNames += matches == 0u ? "" : ", ";
				shortNames += type.Name;
				++matches;
			}
		}
		if (matches > 1u)
		{
			return Fail(Term.Column, FString::Format("type '{}' names several types ({}): write the dotted name", Term.Name, shortNames));
		}
		return Fail(Term.Column, FString::Format("unknown type '{}'", Term.Name));
	}

	// ---- Predicates ----

	bool CompilePredicate (const SQueryPredicate& Predicate, const STypeScope& Scope, SQueryNode& Out, FString& Describe)
	{
		const uint32 typeCount = GetTypeCount();
		if (Predicate.Key.View().StartsWith('$'))
		{
			const std::optional<ESystemColumn> column = FindSystemColumn(Predicate.Key.View());
			if (!column)
			{
				FString names;
				for (uint32 i = 0u; i < SystemColumnCount; ++i)
				{
					names += i == 0u ? "" : ", ";
					names += SystemColumnNames[i];
				}
				return Fail(Predicate.Column, FString::Format("unknown column {} (known: {})", Predicate.Key, names));
			}
			SSystemTest test;
			test.Column = *column;
			if (!CompileSystemValues(Predicate, *column, test.Op, test.Kind, test.Values))
			{
				return false;
			}
			AppendSeparator(Describe);
			Describe.AppendFormat("where:{} {}", GetSystemColumnName(*column), GetOpName(test.Op));
			AppendValues(Describe, test.Kind, test.Values);
			test.bCheap = IsCheapColumn(*column);
			Out.SystemTests.push_back(std::move(test));
			return true;
		}

		std::vector<uint8> declares(typeCount, 0u);
		FString typed;
		uint32 declaring = 0u;
		EOp op = EOp::Eq;
		EValueKind kind = EValueKind::Int;
		SCompiledValues values;
		for (uint32 t = 0u; t < typeCount; ++t)
		{
			if (Scope.Types[t] == 0u)
			{
				continue;
			}
			const std::optional<uint32> key = CAssetTypeRegistry::FindKey(static_cast<reflect::TypeId>(t), Predicate.Key.View());
			if (!key)
			{
				continue;
			}
			const SAssetTypeInfo& info = CAssetTypeRegistry::Get(static_cast<reflect::TypeId>(t));
			const SStaticTagKey& desc = info.Keys[*key];
			SColumnTest test;
			test.Key = *key;
			test.KeyType = desc.Type;
			test.bMulti = HasAnyFlags(desc.Flags, ETagFlags::MultiValue);
			if (!CompileTagValues(Predicate, FString::Format("{}.{}", info.Name, desc.Name).View(), desc.Type, test.Op, test.Kind, test.Values))
			{
				return false;
			}
			op = test.Op;
			kind = test.Kind;
			values = test.Values;
			declares[t] = 1u;
			typed += declaring == 0u ? "" : ",";
			typed.AppendFormat("{}.{}", info.Name, desc.Name);
			++declaring;
			Out.TypeTests[t].push_back(std::move(test));
		}
		if (declaring == 0u)
		{
			if (Scope.bExplicit)
			{
				FString inScope;
				for (uint32 t = 0u; t < typeCount; ++t)
				{
					if (Scope.Types[t] != 0u)
					{
						inScope += inScope.IsEmpty() ? "" : ", ";
						inScope += CAssetTypeRegistry::Get(static_cast<reflect::TypeId>(t)).Name;
					}
				}
				return Fail(Predicate.Column, FString::Format("no type in scope ({}) has a tag '{}'", inScope, Predicate.Key));
			}
			return Fail(Predicate.Column, FString::Format("no type has a tag '{}'", Predicate.Key));
		}
		for (uint32 t = 0u; t < typeCount; ++t)
		{
			Out.AllowedTypes[t] = Out.AllowedTypes[t] != 0u && declares[t] != 0u ? 1u : 0u;
		}
		AppendSeparator(Describe);
		Describe.AppendFormat("where:{} {}", typed, GetOpName(op));
		AppendValues(Describe, kind, values);
		return true;
	}

	[[nodiscard]] static EOp LowerOp (ECompare Op, EValueKind Kind) noexcept
	{
		switch (Op)
		{
		case ECompare::Eq: return EOp::Eq;
		case ECompare::Ne: return EOp::Ne;
		case ECompare::Lt: return EOp::Lt;
		case ECompare::Le: return EOp::Le;
		case ECompare::Gt: return EOp::Gt;
		case ECompare::Ge: return EOp::Ge;
		case ECompare::In: return EOp::In;
		case ECompare::Prefix: return EOp::Prefix;
		case ECompare::Contains: return EOp::Contains;
		case ECompare::Exists: return EOp::Exists;
		case ECompare::Missing: return EOp::Missing;
		case ECompare::Match: return Kind == EValueKind::Text ? EOp::Prefix : EOp::Eq;
		}
		return EOp::Eq;
	}

	// The operator suits the value kind and the predicate holds the right number of values.
	bool CheckShape (const SQueryPredicate& Predicate, FStringView Key, EOp Op, EValueKind Kind)
	{
		const bool bOrdering = Op == EOp::Lt || Op == EOp::Le || Op == EOp::Gt || Op == EOp::Ge;
		const bool bText = Op == EOp::Prefix || Op == EOp::Contains;
		if (bText && Kind != EValueKind::Text)
		{
			return Fail(Predicate.Column, FString::Format("{} is not text: {} needs a Name or String key", Key, enum_::ToString(Predicate.Op)));
		}
		if (bOrdering && (Kind == EValueKind::Bool || Kind == EValueKind::Bits || Kind == EValueKind::TypeId))
		{
			return Fail(Predicate.Column, FString::Format("{} has no order: it takes =, !=, ':' or in", Key));
		}
		const size_t count = Predicate.Values.size();
		if (Op == EOp::Exists || Op == EOp::Missing)
		{
			if (count != 0u)
			{
				return Fail(Predicate.Column, FString::Format("{} {} takes no value", Key, enum_::ToString(Predicate.Op)));
			}
		}
		else if (Op == EOp::In ? count == 0u : count != 1u)
		{
			return Fail(Predicate.Column, FString::Format("{} {} takes {} value", Key, enum_::ToString(Predicate.Op), Op == EOp::In ? "at least one" : "one"));
		}
		return true;
	}

	// Literal and typed values for a tag key of type KeyType.
	bool CompileTagValues (const SQueryPredicate& Predicate, FStringView Key, ETagType KeyType, EOp& OutOp, EValueKind& OutKind, SCompiledValues& Out)
	{
		EValueKind kind = EValueKind::Int;
		switch (KeyType)
		{
		case ETagType::Int64: kind = EValueKind::Int; break;
		case ETagType::Double: kind = EValueKind::Real; break;
		case ETagType::Bool: kind = EValueKind::Bool; break;
		case ETagType::Name:
		case ETagType::String: kind = EValueKind::Text; break;
		case ETagType::Enum:
		case ETagType::AssetRef:
		case ETagType::RowRef: kind = EValueKind::Bits; break;
		case ETagType::None: return Fail(Predicate.Column, FString::Format("{} has no value type", Key));
		}
		OutOp = LowerOp(Predicate.Op, kind);
		if (!CheckShape(Predicate, Key, OutOp, kind))
		{
			return false;
		}
		for (const STagValue& value : Predicate.Values)
		{
			if (!ConvertValue(Predicate, Key, KeyType, value, kind, Out))
			{
				return false;
			}
		}
		FinishNumbers(kind, Out);
		OutKind = kind;
		return true;
	}

	bool CompileSystemValues (const SQueryPredicate& Predicate, ESystemColumn Column, EOp& OutOp, EValueKind& OutKind, SCompiledValues& Out)
	{
		const FStringView key = GetSystemColumnName(Column);
		EValueKind kind = EValueKind::Int;
		switch (Column)
		{
		case ESystemColumn::Type:
			// By id, except prefix and substring, which read the dotted name.
			kind = Predicate.Op == ECompare::Prefix || Predicate.Op == ECompare::Contains ? EValueKind::Text : EValueKind::TypeId;
			break;
		case ESystemColumn::Name:
		case ESystemColumn::Path:
		case ESystemColumn::Folder:
		case ESystemColumn::Labels:
		case ESystemColumn::PrimaryName:
			kind = EValueKind::Text;
			break;
		case ESystemColumn::ImportStatus:
		case ESystemColumn::CookRule:
			kind = EValueKind::Bits;
			break;
		case ESystemColumn::Size:
		case ESystemColumn::Problems:
		case ESystemColumn::Referencers:
		case ESystemColumn::Dependencies:
		case ESystemColumn::Chunk:
			kind = EValueKind::Int;
			break;
		}
		OutOp = LowerOp(Predicate.Op, kind);
		if (!CheckShape(Predicate, key, OutOp, kind))
		{
			return false;
		}
		for (const STagValue& value : Predicate.Values)
		{
			if (kind == EValueKind::TypeId)
			{
				if (value.Type != ETagType::None && value.Type != ETagType::Name)
				{
					return Fail(Predicate.Column, FString::Format("$Type takes a type name, not a {} value", enum_::ToString(value.Type)));
				}
				SQueryType term { value.Text, true, Predicate.Column };
				reflect::TypeId type = 0u;
				if (!ResolveType(term, type))
				{
					return false;
				}
				Out.Bits.push_back(type);
				continue;
			}
			if (Column == ESystemColumn::ImportStatus || Column == ESystemColumn::CookRule)
			{
				if (!ConvertEnumName(Predicate, Column, value, Out))
				{
					return false;
				}
				continue;
			}
			if (!ConvertValue(Predicate, key, kind == EValueKind::Text ? ETagType::Name : ETagType::Int64, value, kind, Out))
			{
				return false;
			}
		}
		FinishNumbers(kind, Out);
		OutKind = kind;
		return true;
	}

	bool ConvertEnumName (const SQueryPredicate& Predicate, ESystemColumn Column, const STagValue& Value, SCompiledValues& Out)
	{
		if (Value.Type == ETagType::Int64 || Value.Type == ETagType::Enum)
		{
			Out.Bits.push_back(Value.Bits);
			return true;
		}
		const FStringView text = Value.Text.View();
		if (Column == ESystemColumn::CookRule)
		{
			ECookRule rule = ECookRule::Default;
			if (text.Equals<ESearchCase::IgnoreCaseAscii>("Auto") || enum_::TryParse(text, &rule))
			{
				Out.Bits.push_back(static_cast<uint64>(rule));
				return true;
			}
			return Fail(Predicate.Column, FString::Format("$CookRule takes Default (Auto), Always, Never, EditorOnly or DevelopmentOnly, not '{}'", text));
		}
		EImportStatus status = EImportStatus::NotAnalyzed;
		if (enum_::TryParse(text, &status))
		{
			Out.Bits.push_back(static_cast<uint64>(status));
			return true;
		}
		return Fail(Predicate.Column, FString::Format("$ImportStatus takes NotAnalyzed, Analyzed, AnalyzeFailed or ImporterUnavailable, not '{}'", text));
	}

	// An In list that mixes whole and fractional numbers compares as Real.
	static void FinishNumbers (EValueKind& Kind, SCompiledValues& Values)
	{
		if (Kind == EValueKind::Int && !Values.Reals.empty())
		{
			for (const int64 number : Values.Ints)
			{
				Values.Reals.push_back(static_cast<double>(number));
			}
			Values.Ints.clear();
			Kind = EValueKind::Real;
		}
	}

	bool ConvertValue (const SQueryPredicate& Predicate, FStringView Key, ETagType KeyType, const STagValue& Value, EValueKind Kind, SCompiledValues& Out)
	{
		const uint32 column = Predicate.Column;
		const bool bLiteral = Value.Type == ETagType::None;
		const FStringView text = Value.Text.View();
		const auto mismatch = [&] ()
		{
			return Fail(column, FString::Format("{} is {}, the value is {}", Key, enum_::ToString(KeyType), enum_::ToString(Value.Type)));
		};
		switch (Kind)
		{
		case EValueKind::Int:
		case EValueKind::Real:
			if (bLiteral)
			{
				if (Value.IsQuotedLiteral())
				{
					return Fail(column, FString::Format("'{}' is quoted text and {} is a number: write the number unquoted", text, Key));
				}
				if (Kind == EValueKind::Int)
				{
					if (const std::optional<int64> number = text.ToInt64())
					{
						Out.Ints.push_back(*number);
						return true;
					}
				}
				if (const std::optional<double> number = text.ToDouble())
				{
					Out.Reals.push_back(*number);
					return true;
				}
				return Fail(column, FString::Format("{} compares numbers: '{}' is not one", Key, text));
			}
			if (Value.Type == ETagType::Int64)
			{
				if (Kind == EValueKind::Int)
				{
					Out.Ints.push_back(Value.AsInt64());
				}
				else
				{
					Out.Reals.push_back(static_cast<double>(Value.AsInt64()));
				}
				return true;
			}
			if (Value.Type == ETagType::Double)
			{
				Out.Reals.push_back(Value.AsDouble());
				return true;
			}
			return mismatch();
		case EValueKind::Bool:
			if (bLiteral)
			{
				if (!Value.IsQuotedLiteral() && text.Equals<ESearchCase::IgnoreCaseAscii>("true"))
				{
					Out.Bits.push_back(1u);
					return true;
				}
				if (!Value.IsQuotedLiteral() && text.Equals<ESearchCase::IgnoreCaseAscii>("false"))
				{
					Out.Bits.push_back(0u);
					return true;
				}
				return Fail(column, FString::Format("{} is true or false, not '{}'", Key, text));
			}
			if (Value.Type == ETagType::Bool)
			{
				Out.Bits.push_back(Value.AsBool() ? 1u : 0u);
				return true;
			}
			return mismatch();
		case EValueKind::Text:
			if (bLiteral || Value.Type == ETagType::Name || Value.Type == ETagType::String)
			{
				Out.Texts.push_back(Fold(text));
				return true;
			}
			return mismatch();
		case EValueKind::Bits:
			if (!bLiteral)
			{
				if (Value.Type != KeyType && Value.Type != ETagType::Int64)
				{
					return mismatch();
				}
				Out.Bits.push_back(Value.Bits);
				return true;
			}
			if (KeyType == ETagType::AssetRef || KeyType == ETagType::RowRef)
			{
				// A reference literal names its target like ref: does.
				uint32 record = InvalidRow;
				if (!ResolveTarget(text, SAssetId {}, column, Key, record))
				{
					return false;
				}
				Out.Bits.push_back(GetId64(record));
				return true;
			}
			if (const std::optional<uint64> number = Value.IsQuotedLiteral() ? std::nullopt : text.ToUInt64())
			{
				Out.Bits.push_back(*number);
				return true;
			}
			return Fail(column, FString::Format("{} is an enum: compare it by number (enum names arrive with reflection, M5)", Key));
		case EValueKind::TypeId:
			break;
		}
		return mismatch();
	}

	[[nodiscard]] uint64 GetId64 (uint32 RecordValue) const noexcept
	{
		return (RecordValue & SubEntryBit) != 0u ? Tables.SubEntries.AssetId64[RecordValue & ~SubEntryBit] : Tables.Assets.AssetId64[RecordValue];
	}

	// ---- Folders ----

	bool CompileFolder (const SQueryFolder& Folder, SQueryNode& Out, FString& Describe)
	{
		const FStringView path = Folder.MountPath.View();
		std::vector<uint32> rows;
		Tables.Indices.FoldersByPortablePath.ForEach(HashPortable(path), [&] (uint32 Row)
		{
			if (Tables.Folders.Mount[Row] != InvalidMountId && EqualPortable(Tables.Paths.Get(Tables.Folders.Path[Row]), path))
			{
				rows.push_back(Row);
			}
			return true;
		});
		if (rows.empty())
		{
			return Fail(Folder.Column, FString::Format("no folder '{}' in the registry", path));
		}
		std::sort(rows.begin(), rows.end());
		for (const uint32 row : rows)
		{
			SFolderRange range;
			range.Mount = Tables.Folders.Mount[row];
			range.Begin = Tables.Folders.PreorderBegin[row];
			range.End = Tables.Folders.PreorderEnd[row];
			range.Row = row;
			range.bRecursive = Folder.bRecursive;
			AppendSeparator(Describe);
			Describe.AppendFormat("dir{}{}[{}:{}..{})", Folder.bRecursive ? ":" : "=", Tables.Paths.Get(Tables.Folders.Path[row]), range.Mount,
				range.Begin, range.bRecursive ? range.End : range.Begin + 1u);
			Out.Folders.push_back(range);
		}
		return true;
	}

	// ---- Graph ----

	bool CompileGraph (const SQueryGraph& Graph, SQueryNode& Out, FString& Describe)
	{
		uint32 value = InvalidRow;
		if (!ResolveTarget(Graph.TargetText.View(), Graph.Target, Graph.Column, Graph.Direction == EQueryGraphDirection::References ? "ref" : "dep", value))
		{
			return false;
		}
		SGraphSpec spec;
		spec.Direction = Graph.Direction;
		spec.bTransitive = Graph.bTransitive;
		spec.Mask = Graph.Mask;
		if ((value & SubEntryBit) != 0u)
		{
			const uint32 sub = value & ~SubEntryBit;
			spec.TargetAsset = Tables.SubEntries.Owner[sub];
			spec.TargetSub = Tables.SubEntries.SubId[sub];
		}
		else
		{
			spec.TargetAsset = value;
		}
		Out.Graph.push_back(static_cast<uint32>(Plan.Graph.size()));
		Plan.Graph.push_back(spec);

		SAssetData data;
		View.GetRecord(SRecordId { value, static_cast<uint32>(Tables.Revision) }, data);
		AppendSeparator(Describe);
		Describe.AppendFormat("{}:{}{}", Graph.Direction == EQueryGraphDirection::References ? "ref" : "dep", Graph.bTransitive ? "~" : "", data.MountPath);
		if (static_cast<uint16>(Graph.Mask) != 0u)
		{
			Describe.AppendFormat("[mask {:#x}]", static_cast<uint16>(Graph.Mask));
		}
		return true;
	}

	/**
	 * A ref:/dep:/table: target or a reference literal: an id (code), a mount path with an optional "#label"
	 * (FindByPath: the portable fold), else a unique $Name - a file's stem or a sub-entry's label, the exact
	 * spelling first, else under the portable fold. OutValue: an SRecordId value of this view.
	 */
	bool ResolveTarget (FStringView Text, const SAssetId& Id, uint32 Column, FStringView What, uint32& OutValue)
	{
		if (Id.IsValid())
		{
			if (const std::optional<SRecordId> record = View.Find(Id))
			{
				OutValue = record->Value;
				return true;
			}
			return Fail(Column, FString::Format("{}: no record has the id {}", What, Id.Guid.ToString()));
		}
		if (Text.IsEmpty())
		{
			return Fail(Column, FString::Format("{}: no target", What));
		}
		if (Text[0] == '/')
		{
			SDiagnostic error;
			if (const std::optional<SRecordId> record = View.FindByPath(Text, &error))
			{
				OutValue = record->Value;
				return true;
			}
			if (error.Code == Path001)
			{
				return Fail(Column, FString::Format("{}:{} is ambiguous: {}", What, Text, error.Message));
			}
			return Fail(Column, FString::Format("{}:{} names no record", What, Text));
		}

		std::vector<uint32> found;
		const auto collect = [&] (auto&& Matches)
		{
			const uint32 assets = Tables.Assets.File.Size();
			for (uint32 row = 0u; row < assets; ++row)
			{
				if (Tables.Assets.File[row] != InvalidRow && Matches(Tables.Assets.Name[row]))
				{
					found.push_back(row);
				}
			}
			const uint32 subs = Tables.SubEntries.Owner.Size();
			for (uint32 sub = 0u; sub < subs; ++sub)
			{
				if (Tables.SubEntries.Owner[sub] != InvalidRow && Matches(Tables.SubEntries.Key[sub]))
				{
					found.push_back(SubEntryBit | sub);
				}
			}
		};
		if (const std::optional<uint32> exact = Tables.Names.Find(Text))
		{
			collect([id = *exact] (uint32 NameId) { return NameId == id; });
		}
		if (found.empty())
		{
			const FString folded = Fold(Text);
			collect([&] (uint32 NameId) { return Tables.Names.GetFolded(NameId) == folded.View(); });
		}
		if (found.size() == 1u)
		{
			OutValue = found.front();
			return true;
		}
		if (found.empty())
		{
			return Fail(Column, FString::Format("{}:{} names no record (give a mount path or a $Name)", What, Text));
		}
		FString examples;
		for (size_t i = 0u; i < found.size() && i < 3u; ++i)
		{
			SAssetData data;
			View.GetRecord(SRecordId { found[i], static_cast<uint32>(Tables.Revision) }, data);
			examples += i == 0u ? "" : ", ";
			examples += data.MountPath;
		}
		return Fail(Column, FString::Format("{}:{} names {} records ({}{}): give a mount path", What, Text, found.size(), examples, found.size() > 3u ? ", ..." : ""));
	}

	// ---- Order ----

	bool CompileOrder (const SQueryOrder& Order, const STypeScope& Scope)
	{
		SOrderSpec& spec = Plan.Order;
		spec.bAscending = Order.bAscending;
		if (Order.Key.View().StartsWith('$'))
		{
			const std::optional<ESystemColumn> column = FindSystemColumn(Order.Key.View());
			if (!column)
			{
				return Fail(Order.Column, FString::Format("unknown column {} to order by", Order.Key));
			}
			spec.Kind = EOrderKind::System;
			spec.Column = *column;
			Plan.Description.AppendFormat(" order:{} {}", GetSystemColumnName(*column), Order.bAscending ? "asc" : "desc");
			return true;
		}
		const uint32 typeCount = GetTypeCount();
		spec.Kind = EOrderKind::Tag;
		spec.KeyPerType.assign(typeCount, InvalidRow);
		bool bAny = false;
		for (uint32 t = 0u; t < typeCount; ++t)
		{
			if (Scope.Types[t] == 0u)
			{
				continue;
			}
			if (const std::optional<uint32> key = CAssetTypeRegistry::FindKey(static_cast<reflect::TypeId>(t), Order.Key.View()))
			{
				spec.KeyPerType[t] = *key;
				bAny = true;
			}
		}
		if (!bAny)
		{
			return Fail(Order.Column, FString::Format("no type{} has a tag '{}' to order by", Scope.bExplicit ? " in scope" : "", Order.Key));
		}
		Plan.Description.AppendFormat(" order:{} {}", Order.Key, Order.bAscending ? "asc" : "desc");
		return true;
	}
};


// ---------------- Reading records ----------------

// A record of one view as the run reads it.
struct SRowContext
{
	uint32 Value = InvalidRow;     // SRecordId value
	uint32 Asset = InvalidRow;     // the file's ASSETS row
	uint32 Sub = InvalidRow;       // SUBENTRY row, InvalidRow for an asset record
	uint32 File = InvalidRow;
	reflect::TypeId Type = 0u;
	uint32 TypeRow = InvalidRow;
};

[[nodiscard]] SRowContext MakeContext (const SRegistryTables& Tables, uint32 Value) noexcept
{
	SRowContext context;
	context.Value = Value;
	if ((Value & SubEntryBit) != 0u)
	{
		context.Sub = Value & ~SubEntryBit;
		context.Asset = Tables.SubEntries.Owner[context.Sub];
		context.Type = Tables.SubEntries.Type[context.Sub];
		context.TypeRow = Tables.SubEntries.TypeRow[context.Sub];
	}
	else
	{
		context.Asset = Value;
		context.Type = Tables.Assets.Type[Value];
		context.TypeRow = Tables.Assets.TypeRow[Value];
	}
	context.File = Tables.Assets.File[context.Asset];
	return context;
}

[[nodiscard]] bool IsLiveValue (const SRegistryTables& Tables, uint32 Value) noexcept
{
	if ((Value & SubEntryBit) != 0u)
	{
		const uint32 sub = Value & ~SubEntryBit;
		return sub < Tables.SubEntries.Owner.Size() && Tables.SubEntries.Owner[sub] != InvalidRow;
	}
	return Value < Tables.Assets.File.Size() && Tables.Assets.File[Value] != InvalidRow;
}

// Visit(uint32 SourceRow, const SEdgeRow&) for every live edge resolved to AssetRow (any TargetSub when SubId is 0).
template <typename TVisit>
void ForEachEdgeInto (const SRegistryTables& Tables, uint32 AssetRow, uint64 SubId, TVisit&& Visit)
{
	Tables.Graph.TargetsByResolved.ForEach(MixRegistryKey(AssetRow), [&] (uint32 TargetRow)
	{
		const STargetRow& target = Tables.Graph.Targets[TargetRow];
		if (target.Resolved == AssetRow && target.State == ETargetState::Resolved && (SubId == 0u || target.TargetSub == SubId))
		{
			ForEachLiveReverseEdge(Tables, TargetRow, [&] (const SReverseEdge& Entry) { Visit(Entry.Source, GetEdgeRow(Tables, Entry.Edge)); });
		}
		return true;
	});
}

// Visit(const SEdgeRow&, const STargetRow&) for every live edge a record declares (an asset record: its file's).
template <typename TVisit>
void ForEachOwnEdge (const SRegistryTables& Tables, const SRowContext& Context, TVisit&& Visit)
{
	const uint64 subId = Context.Sub != InvalidRow ? Tables.SubEntries.SubId[Context.Sub] : 0u;
	ForEachLiveEdge(Tables, Context.Asset, [&] (uint32 EdgeRef)
	{
		const SEdgeRow& edge = GetEdgeRow(Tables, EdgeRef);
		if (Context.Sub == InvalidRow || edge.FromSub == subId)
		{
			Visit(edge, Tables.Graph.Targets[edge.Target]);
		}
	});
}

[[nodiscard]] uint64 GetSubId (const SRegistryTables& Tables, const SRowContext& Context) noexcept
{
	return Context.Sub != InvalidRow ? Tables.SubEntries.SubId[Context.Sub] : 0u;
}

// The problems of a sub-entry's own subject ("file#label"); an asset record's count covers its sub-entries (Problems).
[[nodiscard]] uint32 CountOwnProblems (const SRegistryTables& Tables, const SRowContext& Context, FString& Scratch)
{
	if (Context.Sub == InvalidRow)
	{
		return Tables.Assets.Problems[Context.Asset];
	}
	Scratch = FString(Tables.Paths.Get(Tables.Files.Path[Context.File]));
	Scratch += '#';
	Scratch += Tables.Names.Get(Tables.SubEntries.Key[Context.Sub]);
	const uint32 subject = FindProblemSubject(Tables, Scratch.View());
	if (subject == InvalidRow)
	{
		return 0u;
	}
	uint32 count = 0u;
	for (uint32 source = 0u; source < ProblemSourceCount; ++source)
	{
		const FProblemList& list = Tables.Problems.Lists[source][subject];
		count += list != nullptr ? static_cast<uint32>(list->size()) : 0u;
	}
	return count;
}

[[nodiscard]] uint32 CountReferencers (const SRegistryTables& Tables, const SRowContext& Context)
{
	std::vector<uint32> sources;
	ForEachEdgeInto(Tables, Context.Asset, GetSubId(Tables, Context), [&] (uint32 Source, const SEdgeRow&) { sources.push_back(Source); });
	std::sort(sources.begin(), sources.end());
	return static_cast<uint32>(std::unique(sources.begin(), sources.end()) - sources.begin());
}

[[nodiscard]] uint32 CountDependencies (const SRegistryTables& Tables, const SRowContext& Context)
{
	uint32 count = 0u;
	ForEachOwnEdge(Tables, Context, [&] (const SEdgeRow&, const STargetRow&) { ++count; });
	return count;
}

// The folded file path minus "/leaf": FoldCase maps code point by code point, so it is a prefix of the folded path.
[[nodiscard]] FStringView GetFoldedFolderPath (const SRegistryTables& Tables, uint32 File) noexcept
{
	const FStringView path = GetFoldedFilePath(Tables, File);
	const FStringView leaf = Tables.Names.GetFolded(Tables.Files.LeafName[File]);
	return path.Len() > leaf.Len() ? path.Left(path.Len() - leaf.Len() - 1u) : FStringView();
}

// One value of a system column: numbers and ids in Bits, text in Text (folded or as stored, as asked).
struct SCell
{
	uint64 Bits = 0u;
	FStringView Text;
};

/**
 * Visit(const SCell&) for each value of a system column of a record (none when absent: $PrimaryName without a
 * primary name, $Labels without labels). Scratch holds composed text ($Path of a sub-entry).
 */
template <typename TVisit>
void ForEachSystemValue (const SRegistryTables& Tables, const SQueryPlan* Plan, const SRowContext& Context, ESystemColumn Column,
	bool bFolded, FString& Scratch, TVisit&& Visit)
{
	SCell cell;
	switch (Column)
	{
	case ESystemColumn::Type:
		cell.Bits = Context.Type;
		cell.Text = Plan != nullptr && bFolded ? Plan->FoldedTypeNames[Context.Type].View() : FStringView(CAssetTypeRegistry::Get(Context.Type).Name);
		Visit(cell);
		return;
	case ESystemColumn::Name:
	{
		const uint32 name = Context.Sub != InvalidRow ? Tables.SubEntries.Key[Context.Sub] : Tables.Assets.Name[Context.Asset];
		cell.Text = bFolded ? Tables.Names.GetFolded(name) : Tables.Names.Get(name);
		Visit(cell);
		return;
	}
	case ESystemColumn::Path:
		cell.Text = bFolded ? GetFoldedFilePath(Tables, Context.File) : Tables.Paths.Get(Tables.Files.Path[Context.File]);
		if (Context.Sub != InvalidRow)
		{
			const uint32 label = Tables.SubEntries.Key[Context.Sub];
			Scratch = FString(cell.Text);
			Scratch += '#';
			Scratch += bFolded ? Tables.Names.GetFolded(label) : Tables.Names.Get(label);
			cell.Text = Scratch.View();
		}
		Visit(cell);
		return;
	case ESystemColumn::Folder:
		cell.Text = bFolded ? GetFoldedFolderPath(Tables, Context.File) : Tables.Paths.Get(Tables.Folders.Path[Tables.Files.Folder[Context.File]]);
		Visit(cell);
		return;
	case ESystemColumn::Labels:
	{
		const SPoolRange range = Tables.Assets.Labels[Context.Asset];
		for (uint32 i = 0u; i < range.Count; ++i)
		{
			const uint32 label = Tables.Labels[range.Begin + i] & ~RuleLabelBit;
			cell.Text = bFolded ? Tables.Names.GetFolded(label) : Tables.Names.Get(label);
			Visit(cell);
		}
		return;
	}
	case ESystemColumn::Size:
		cell.Bits = Tables.Files.Size[Context.File];
		Visit(cell);
		return;
	case ESystemColumn::ImportStatus:
		cell.Bits = static_cast<uint64>(Tables.Assets.ImportStatus[Context.Asset]);
		cell.Text = enum_::ToString(Tables.Assets.ImportStatus[Context.Asset]);
		Visit(cell);
		return;
	case ESystemColumn::Problems:
		cell.Bits = CountOwnProblems(Tables, Context, Scratch);
		Visit(cell);
		return;
	case ESystemColumn::Referencers:
		cell.Bits = CountReferencers(Tables, Context);
		Visit(cell);
		return;
	case ESystemColumn::Dependencies:
		cell.Bits = CountDependencies(Tables, Context);
		Visit(cell);
		return;
	case ESystemColumn::CookRule:
		cell.Bits = static_cast<uint64>(Tables.Assets.CookRule[Context.Asset]);
		cell.Text = enum_::ToString(Tables.Assets.CookRule[Context.Asset]);
		Visit(cell);
		return;
	case ESystemColumn::Chunk:
		cell.Bits = Tables.Assets.Chunk[Context.Asset];
		Visit(cell);
		return;
	case ESystemColumn::PrimaryName:
	{
		const uint32 name = Context.Sub == InvalidRow ? Tables.Assets.PrimaryName[Context.Asset] : InvalidRow;
		if (name != InvalidRow)
		{
			cell.Text = bFolded ? Tables.Names.GetFolded(name) : Tables.Names.Get(name);
			Visit(cell);
		}
		return;
	}
	}
}


// ---------------- Comparisons ----------------

[[nodiscard]] int32 Sign (int32 Value) noexcept
{
	return (Value > 0) - (Value < 0);
}

[[nodiscard]] bool HoldsOrdering (EOp Op, int32 Compared) noexcept
{
	switch (Op)
	{
	case EOp::Eq: return Compared == 0;
	case EOp::Ne: return Compared != 0;
	case EOp::Lt: return Compared < 0;
	case EOp::Le: return Compared <= 0;
	case EOp::Gt: return Compared > 0;
	case EOp::Ge: return Compared >= 0;
	default: return false;
	}
}

// One present value against the compiled values: Eq, the orderings, In, Prefix, Contains (Ne is "not Eq" above).
[[nodiscard]] bool TestInt (EOp Op, const SCompiledValues& Values, int64 Value) noexcept
{
	if (Op == EOp::In)
	{
		return std::find(Values.Ints.begin(), Values.Ints.end(), Value) != Values.Ints.end();
	}
	const int64 other = Values.Ints[0];
	return HoldsOrdering(Op, (Value > other) - (Value < other));
}

[[nodiscard]] bool TestReal (EOp Op, const SCompiledValues& Values, double Value) noexcept
{
	if (Op == EOp::In)
	{
		return std::find(Values.Reals.begin(), Values.Reals.end(), Value) != Values.Reals.end();
	}
	const double other = Values.Reals[0];
	if (Value != Value || other != other)
	{
		return false;   // NaN compares with nothing
	}
	return HoldsOrdering(Op, (Value > other) - (Value < other));
}

[[nodiscard]] bool TestBits (EOp Op, const SCompiledValues& Values, uint64 Value) noexcept
{
	if (Op == EOp::In)
	{
		return std::find(Values.Bits.begin(), Values.Bits.end(), Value) != Values.Bits.end();
	}
	return Op == EOp::Eq && Value == Values.Bits[0];
}

[[nodiscard]] bool TestText (EOp Op, const SCompiledValues& Values, FStringView Folded) noexcept
{
	switch (Op)
	{
	case EOp::In:
		return std::any_of(Values.Texts.begin(), Values.Texts.end(), [&] (const FString& Text) { return Folded == Text.View(); });
	case EOp::Prefix:
		return Folded.StartsWith(Values.Texts[0].View());
	case EOp::Contains:
		return Folded.Contains(Values.Texts[0].View());
	default:
		return HoldsOrdering(Op, Sign(Folded.Compare(Values.Texts[0].View())));
	}
}

// Ne is "present and no value equals": the positive test with Eq, negated.
[[nodiscard]] EOp GetPositiveOp (EOp Op) noexcept
{
	return Op == EOp::Ne ? EOp::Eq : Op;
}

[[nodiscard]] bool TestTagBits (const SRegistryTables& Tables, const SColumnTest& Test, EOp Op, uint64 Bits) noexcept
{
	switch (Test.Kind)
	{
	case EValueKind::Int:
		return TestInt(Op, Test.Values, static_cast<int64>(Bits));
	case EValueKind::Real:
		return TestReal(Op, Test.Values, Test.KeyType == ETagType::Double ? std::bit_cast<double>(Bits) : static_cast<double>(static_cast<int64>(Bits)));
	case EValueKind::Bool:
		return TestBits(Op, Test.Values, Bits != 0u ? 1u : 0u);
	case EValueKind::Bits:
	case EValueKind::TypeId:
		return TestBits(Op, Test.Values, Bits);
	case EValueKind::Text:
		return TestText(Op, Test.Values, Test.KeyType == ETagType::String ? Tables.Strings.GetFolded(static_cast<uint32>(Bits))
			: Tables.Names.GetFolded(static_cast<uint32>(Bits)));
	}
	return false;
}

[[nodiscard]] bool TestColumnRow (const SRegistryTables& Tables, const CTypeTable& Table, const SColumnTest& Test, uint32 Row) noexcept
{
	const STagColumn& column = Table.Columns[Test.Key];
	const bool bPresent = column.PresentBits.Test(Row);
	if (Test.Op == EOp::Exists || Test.Op == EOp::Missing)
	{
		return bPresent == (Test.Op == EOp::Exists);
	}
	if (!bPresent)
	{
		return false;
	}
	const EOp op = GetPositiveOp(Test.Op);
	bool bAny = false;
	if (Test.bMulti)
	{
		const SPoolRange range = column.MultiRanges[Row];
		for (uint32 i = 0u; i < range.Count && !bAny; ++i)
		{
			bAny = TestTagBits(Tables, Test, op, column.Multi[range.Begin + i]);
		}
	}
	else
	{
		bAny = TestTagBits(Tables, Test, op, column.Values[Row]);
	}
	return Test.Op == EOp::Ne ? !bAny : bAny;
}

[[nodiscard]] bool TestSystem (const SRegistryTables& Tables, const SQueryPlan& Plan, const SSystemTest& Test, const SRowContext& Context, FString& Scratch)
{
	bool bPresent = false;
	bool bAny = false;
	const EOp op = GetPositiveOp(Test.Op);
	ForEachSystemValue(Tables, &Plan, Context, Test.Column, true, Scratch, [&] (const SCell& Cell)
	{
		bPresent = true;
		if (bAny || Test.Op == EOp::Exists || Test.Op == EOp::Missing)
		{
			return;
		}
		switch (Test.Kind)
		{
		case EValueKind::Int: bAny = TestInt(op, Test.Values, static_cast<int64>(Cell.Bits)); break;
		case EValueKind::Real: bAny = TestReal(op, Test.Values, static_cast<double>(static_cast<int64>(Cell.Bits))); break;
		case EValueKind::Bool:
		case EValueKind::Bits:
		case EValueKind::TypeId: bAny = TestBits(op, Test.Values, Cell.Bits); break;
		case EValueKind::Text: bAny = TestText(op, Test.Values, Cell.Text); break;
		}
	});
	if (Test.Op == EOp::Exists || Test.Op == EOp::Missing)
	{
		return bPresent == (Test.Op == EOp::Exists);
	}
	return bPresent && (Test.Op == EOp::Ne ? !bAny : bAny);
}


// ---------------- Runner ----------------

// The records a graph term admits: files (and, for a direct term, the sub-entries an edge names or declares).
struct SGraphSet
{
	std::vector<uint8> Files;           // per ASSETS row
	std::vector<uint32> Subs;           // sorted SUBENTRY record values (direct terms)
	std::vector<uint32> FileList;       // the reached files, for candidate lists
	bool bTransitive = false;
};

struct SResult
{
	uint32 Value = InvalidRow;
	uint32 Asset = InvalidRow;
	uint8 Class = 0u;                   // EOrderClass
	uint64 Bits = 0u;                   // Int: int64; Real: double bits
	FStringView FoldedText;
	FStringView RawText;
	FStringView FoldedPath;
	FStringView FoldedLabel;            // a sub-entry's label, folded; empty for an asset record
};

enum EOrderClass : uint8 { OrderInt = 0u, OrderReal = 1u, OrderText = 2u, OrderPath = 3u, OrderAbsent = 255u };

class CQueryRunner
{
public:
	CQueryRunner (const SRegistryTables& InTables, const SQueryPlan& InPlan) noexcept
		: Tables(InTables)
		, Plan(InPlan)
	{
	}

	void Collect (std::vector<SResult>& Out, uint32& OutCandidates)
	{
		BuildGraphSets();
		const SQueryNode& root = Plan.Root;
		if (root.bMatchesNothing)
		{
			return;
		}
		std::vector<uint32> candidates;
		if (ChooseCandidates(candidates))
		{
			for (const uint32 value : candidates)
			{
				++OutCandidates;
				if (!IsLiveValue(Tables, value))
				{
					continue;
				}
				const SRowContext context = MakeContext(Tables, value);
				if (InScope(context) && MatchesNode(root, context, false))
				{
					Out.push_back(MakeResult(context));
				}
			}
			return;
		}

		if (CanScanAssets())
		{
			ScanAssets(Out, OutCandidates);
			return;
		}
		const uint32 typeCount = static_cast<uint32>((std::min)(Tables.Types.size(), static_cast<size_t>(GetTypeCount())));
		for (uint32 type = 0u; type < typeCount; ++type)
		{
			ScanTypeTable(static_cast<reflect::TypeId>(type), Out, OutCandidates);
		}
	}

	void Sort (std::vector<SResult>& Results) const
	{
		const auto less = [this] (const SResult& A, const SResult& B) { return Less(A, B); };
		if (Plan.Limit != 0u)
		{
			const uint64 end = static_cast<uint64>(Plan.Offset) + Plan.Limit;
			const size_t keep = end < Results.size() ? static_cast<size_t>(end) : Results.size();
			std::partial_sort(Results.begin(), Results.begin() + static_cast<std::ptrdiff_t>(keep), Results.end(), less);
		}
		else
		{
			std::sort(Results.begin(), Results.end(), less);
		}
	}

private:
	const SRegistryTables& Tables;
	const SQueryPlan& Plan;
	std::vector<SGraphSet> GraphSets;
	std::vector<uint8> RowLabels;       // per NAMES id: 0 not yet known, 1 a row key, 2 an importer label
	FString Scratch;

	// ---- Scope ----

	// IsRowLabel of a sub-entry, remembered per label: rows of different tables share their keys.
	[[nodiscard]] bool IsRow (uint32 Sub)
	{
		const uint32 key = Tables.SubEntries.Key[Sub];
		if (RowLabels.empty())
		{
			RowLabels.assign(Tables.Names.Size(), 0u);
		}
		uint8& known = RowLabels[key];
		if (known == 0u)
		{
			known = IsRowLabel(Tables.Names.Get(key)) ? 1u : 2u;
		}
		return known == 1u;
	}

	[[nodiscard]] bool InScope (const SRowContext& Context)
	{
		const bool bSub = Context.Sub != InvalidRow;
		switch (Plan.Records)
		{
		case ERecordScope::Assets:
			return !bSub || (Plan.AdmittedSubAssets[Context.Type] != 0u && !IsRow(Context.Sub));
		case ERecordScope::Rows:
			return bSub && IsRow(Context.Sub);
		case ERecordScope::SubAssets:
			return bSub && !IsRow(Context.Sub);
		case ERecordScope::All:
			return true;
		}
		return false;
	}

	// ---- Graph sets ----

	void BuildGraphSets ()
	{
		GraphSets.resize(Plan.Graph.size());
		for (size_t i = 0u; i < Plan.Graph.size(); ++i)
		{
			BuildGraphSet(Plan.Graph[i], GraphSets[i]);
		}
	}

	void BuildGraphSet (const SGraphSpec& Spec, SGraphSet& Out)
	{
		Out.Files.assign(Tables.Assets.File.Size(), 0u);
		Out.bTransitive = Spec.bTransitive;
		const auto enter = [&] (uint32 Row)
		{
			if (Out.Files[Row] == 0u)
			{
				Out.Files[Row] = 1u;
				Out.FileList.push_back(Row);
			}
		};
		const auto addSub = [&] (uint32 AssetRow, uint64 SubId)
		{
			if (!Spec.bTransitive && SubId != 0u)
			{
				const uint32 sub = FindSubEntryRow(Tables, AssetRow, SubId);
				if (sub != InvalidRow)
				{
					Out.Subs.push_back(SubEntryBit | sub);
				}
			}
		};
		const auto expand = [&] (uint32 Row, uint64 SubId)
		{
			if (Spec.Direction == EQueryGraphDirection::References)
			{
				ForEachEdgeInto(Tables, Row, SubId, [&] (uint32 Source, const SEdgeRow& Edge)
				{
					if (HasAllFlags(Edge.Flags, Spec.Mask))
					{
						enter(Source);
						addSub(Source, Edge.FromSub);
					}
				});
				return;
			}
			ForEachLiveEdge(Tables, Row, [&] (uint32 EdgeRef)
			{
				const SEdgeRow& edge = GetEdgeRow(Tables, EdgeRef);
				const STargetRow& target = Tables.Graph.Targets[edge.Target];
				if ((SubId == 0u || edge.FromSub == SubId) && target.State == ETargetState::Resolved && HasAllFlags(edge.Flags, Spec.Mask))
				{
					enter(target.Resolved);
					addSub(target.Resolved, target.TargetSub);
				}
			});
		};
		if (Spec.TargetAsset >= Tables.Assets.File.Size() || Tables.Assets.File[Spec.TargetAsset] == InvalidRow)
		{
			return;
		}
		expand(Spec.TargetAsset, Spec.TargetSub);
		if (Spec.bTransitive)
		{
			for (size_t i = 0u; i < Out.FileList.size(); ++i)
			{
				expand(Out.FileList[i], 0u);
			}
		}
		std::sort(Out.Subs.begin(), Out.Subs.end());
		Out.Subs.erase(std::unique(Out.Subs.begin(), Out.Subs.end()), Out.Subs.end());
	}

	[[nodiscard]] bool InGraphSet (uint32 Index, const SRowContext& Context) const noexcept
	{
		const SGraphSet& set = GraphSets[Index];
		if (set.bTransitive || Context.Sub == InvalidRow)
		{
			return set.Files[Context.Asset] != 0u;
		}
		return std::binary_search(set.Subs.begin(), set.Subs.end(), Context.Value);
	}

	// The narrowest explicit start (§5.6 step 1): the root's ids or its smallest graph set. False: scan the type tables.
	bool ChooseCandidates (std::vector<uint32>& Out) const
	{
		const SQueryNode& root = Plan.Root;
		size_t best = ~size_t(0);
		const std::vector<uint32>* ids = nullptr;
		const SGraphSet* graph = nullptr;
		if (root.bHasGuids)
		{
			ids = &root.Guids;
			best = root.Guids.size();
		}
		for (const uint32 index : root.Graph)
		{
			const SGraphSet& set = GraphSets[index];
			const size_t size = set.FileList.size() + set.Subs.size();
			if (size < best)
			{
				best = size;
				graph = &set;
				ids = nullptr;
			}
		}
		if (ids != nullptr)
		{
			Out = *ids;
			return true;
		}
		if (graph == nullptr)
		{
			return false;
		}
		for (const uint32 row : graph->FileList)
		{
			Out.push_back(row);
			if (graph->bTransitive)
			{
				const uint32 first = Tables.Assets.FirstSub[row];
				for (uint32 i = 0u; i < Tables.Assets.NumSubs[row]; ++i)
				{
					Out.push_back(SubEntryBit | (first + i));
				}
			}
		}
		Out.insert(Out.end(), graph->Subs.begin(), graph->Subs.end());
		return true;
	}

	// ---- Matching ----

	[[nodiscard]] bool MatchesLabels (const SQueryNode& Node, const SRowContext& Context) const noexcept
	{
		const SPoolRange range = Tables.Assets.Labels[Context.Asset];
		for (const SLabelTest& test : Node.Labels)
		{
			bool bHas = false;
			for (uint32 i = 0u; i < range.Count && !bHas; ++i)
			{
				const uint32 label = Tables.Labels[range.Begin + i] & ~RuleLabelBit;
				bHas = label == test.ExactId || Tables.Names.GetFolded(label) == test.Folded.View();
			}
			if (bHas == test.bExclude)
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool MatchesFolders (const SQueryNode& Node, const SRowContext& Context) const noexcept
	{
		const uint32 folder = Tables.Files.Folder[Context.File];
		const MountId mount = Tables.Folders.Mount[folder];
		const uint32 preorder = Tables.Folders.PreorderBegin[folder];
		for (const SFolderRange& range : Node.Folders)
		{
			if (range.bRecursive ? (range.Mount == mount && preorder >= range.Begin && preorder < range.End) : folder == range.Row)
			{
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool MatchesText (const SQueryNode& Node, const SRowContext& Context) const noexcept
	{
		const FStringView path = GetFoldedFilePath(Tables, Context.File);
		const FStringView label = Context.Sub != InvalidRow ? Tables.Names.GetFolded(Tables.SubEntries.Key[Context.Sub]) : FStringView();
		for (const FString& text : Node.Texts)
		{
			if (!path.Contains(text.View()) && !label.Contains(text.View()))
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool MatchesPredicate (ESystemPredicate Predicate, const SRowContext& Context)
	{
		const ERecordFlags flags = Context.Sub != InvalidRow ? Tables.SubEntries.Flags[Context.Sub] : Tables.Assets.Flags[Context.Asset];
		switch (Predicate)
		{
		case ESystemPredicate::BrokenRef:
		{
			bool bBroken = false;
			ForEachOwnEdge(Tables, Context, [&] (const SEdgeRow&, const STargetRow& Target)
			{
				bBroken = bBroken || Target.State == ETargetState::Unresolved || Target.State == ETargetState::Ambiguous;
			});
			return bBroken;
		}
		case ESystemPredicate::Unused:
		{
			bool bUsed = false;
			ForEachEdgeInto(Tables, Context.Asset, GetSubId(Tables, Context), [&] (uint32, const SEdgeRow&) { bUsed = true; });
			return !bUsed;
		}
		case ESystemPredicate::LegacyRef:
			return HasAnyFlags(flags, ERecordFlags::LegacyReferences);
		case ESystemPredicate::Excluded:
			return Tables.Files.Class[Context.File] == EFileClass::Excluded || HasAnyFlags(flags, ERecordFlags::TypeExcluded)
				|| HasAnyFlags(Tables.Assets.Flags[Context.Asset], ERecordFlags::TypeExcluded);
		case ESystemPredicate::Problem:
			return CountOwnProblems(Tables, Context, Scratch) != 0u;
		case ESystemPredicate::StaleTags:
			return HasAnyFlags(flags, ERecordFlags::StaleTags);
		case ESystemPredicate::Primary:
			return HasAnyFlags(flags, ERecordFlags::Primary);
		case ESystemPredicate::Redirected:
		case ESystemPredicate::Unsaved:
		case ESystemPredicate::PendingMeta:
			return false;
		}
		return false;
	}

	// Every term of a node; bSkipTags when the column masks already applied its types and tag predicates.
	[[nodiscard]] bool MatchesNode (const SQueryNode& Node, const SRowContext& Context, bool bSkipTags)
	{
		if (Node.bMatchesNothing)
		{
			return false;
		}
		if (!bSkipTags)
		{
			if (Context.Type >= Node.AllowedTypes.size() || Node.AllowedTypes[Context.Type] == 0u)
			{
				return false;
			}
			const CTypeTable& table = Tables.Types[Context.Type];
			for (const SColumnTest& test : Node.TypeTests[Context.Type])
			{
				if (!TestColumnRow(Tables, table, test, Context.TypeRow))
				{
					return false;
				}
			}
		}
		if (!Node.bHasRowTests)
		{
			return true;
		}
		if (Node.bHasGuids && !std::binary_search(Node.Guids.begin(), Node.Guids.end(), Context.Value))
		{
			return false;
		}
		if (Node.bHasTables && (Context.Sub == InvalidRow || !std::binary_search(Node.TableOwners.begin(), Node.TableOwners.end(), Context.Asset)))
		{
			return false;
		}
		if (!Node.Folders.empty() && !MatchesFolders(Node, Context))
		{
			return false;
		}
		if (!Node.Labels.empty() && !MatchesLabels(Node, Context))
		{
			return false;
		}
		if (!Node.Texts.empty() && !MatchesText(Node, Context))
		{
			return false;
		}
		for (const uint32 index : Node.Graph)
		{
			if (!InGraphSet(index, Context))
			{
				return false;
			}
		}
		for (const SSystemTest& test : Node.SystemTests)
		{
			if (bSkipTags && test.bCheap)
			{
				continue;   // the scan loop tested it (PassesCheapTests)
			}
			if (!TestSystem(Tables, Plan, test, Context, Scratch))
			{
				return false;
			}
		}
		for (const ESystemPredicate predicate : Node.Is)
		{
			if (!MatchesPredicate(predicate, Context))
			{
				return false;
			}
		}
		for (const std::vector<SQueryNode>& group : Node.AnyOf)
		{
			bool bAny = false;
			for (const SQueryNode& alternative : group)
			{
				if (MatchesNode(alternative, Context, false))
				{
					bAny = true;
					break;
				}
			}
			if (!bAny)
			{
				return false;
			}
		}
		return true;
	}

	// ---- Column scan (§5.6 step 2) ----

	// Mask &= Test(value) over the column's rows, 64 at a time; words already zero are skipped.
	template <typename TTest>
	static void MaskColumn (const STagColumn& Column, std::vector<uint64>& Masks, TTest&& Test)
	{
		constexpr uint32 WordsPerPage = RegistryPageRows / 64u;
		const uint32 pages = Column.Values.GetPageCount();
		for (uint32 page = 0u; page < pages; ++page)
		{
			const std::span<const uint64> values = Column.Values.GetPage(page);
			const uint32 rows = static_cast<uint32>(values.size());
			for (uint32 word = 0u; word * 64u < rows; ++word)
			{
				const uint32 index = page * WordsPerPage + word;
				if (index >= Masks.size())
				{
					return;
				}
				uint64& mask = Masks[index];
				if (mask == 0u)
				{
					continue;
				}
				const uint32 base = word * 64u;
				const uint32 count = rows - base < 64u ? rows - base : 64u;
				const uint64* data = values.data() + base;
				uint64 bits = 0u;
				for (uint32 i = 0u; i < count; ++i)
				{
					bits |= static_cast<uint64>(Test(data[i]) ? 1u : 0u) << i;
				}
				mask &= bits & Column.PresentBits.GetWord(index);
			}
		}
	}

	// A numeric comparison of a single-value column, branch-free per row.
	template <typename TValue, typename TRead>
	static bool MaskOrdering (const STagColumn& Column, EOp Op, TValue Other, std::vector<uint64>& Masks, TRead&& Read)
	{
		switch (Op)
		{
		case EOp::Eq: MaskColumn(Column, Masks, [&] (uint64 Bits) { return Read(Bits) == Other; }); return true;
		case EOp::Ne: MaskColumn(Column, Masks, [&] (uint64 Bits) { return Read(Bits) != Other; }); return true;
		case EOp::Lt: MaskColumn(Column, Masks, [&] (uint64 Bits) { return Read(Bits) < Other; }); return true;
		case EOp::Le: MaskColumn(Column, Masks, [&] (uint64 Bits) { return Read(Bits) <= Other; }); return true;
		case EOp::Gt: MaskColumn(Column, Masks, [&] (uint64 Bits) { return Read(Bits) > Other; }); return true;
		case EOp::Ge: MaskColumn(Column, Masks, [&] (uint64 Bits) { return Read(Bits) >= Other; }); return true;
		default: return false;
		}
	}

	void ApplyColumnTest (const CTypeTable& Table, const SColumnTest& Test, std::vector<uint64>& Masks) const
	{
		const STagColumn& column = Table.Columns[Test.Key];
		if (Test.Op == EOp::Exists || Test.Op == EOp::Missing)
		{
			for (uint32 word = 0u; word < Masks.size(); ++word)
			{
				const uint64 present = column.PresentBits.GetWord(word);
				Masks[word] &= Test.Op == EOp::Exists ? present : ~present;
			}
			return;
		}
		if (!Test.bMulti)
		{
			if (Test.Kind == EValueKind::Int && MaskOrdering(column, Test.Op, Test.Values.Ints[0], Masks, [] (uint64 Bits) { return static_cast<int64>(Bits); }))
			{
				return;
			}
			if (Test.Kind == EValueKind::Real && Test.KeyType == ETagType::Double
				&& MaskOrdering(column, Test.Op, Test.Values.Reals[0], Masks, [] (uint64 Bits) { return std::bit_cast<double>(Bits); }))
			{
				return;
			}
		}
		// Everything else row by row, only where the mask still holds.
		for (uint32 word = 0u; word < Masks.size(); ++word)
		{
			uint64 remaining = Masks[word];
			uint64 bits = 0u;
			while (remaining != 0u)
			{
				const uint32 bit = static_cast<uint32>(std::countr_zero(remaining));
				remaining &= remaining - 1u;
				if (TestColumnRow(Tables, Table, Test, word * 64u + bit))
				{
					bits |= uint64(1) << bit;
				}
			}
			Masks[word] = bits;
		}
	}

	// The root's cheap system tests (one direct column read each), before the generic per-row terms.
	[[nodiscard]] bool PassesCheapTests (const SQueryNode& Root, const SRowContext& Context) const noexcept
	{
		for (const SSystemTest& test : Root.SystemTests)
		{
			if (!test.bCheap)
			{
				continue;
			}
			if (test.Op == EOp::Exists || test.Op == EOp::Missing)
			{
				if (test.Op == EOp::Missing)
				{
					return false;   // a cheap column always has its value
				}
				continue;
			}
			const EOp op = GetPositiveOp(test.Op);
			bool bMatch = false;
			switch (test.Column)
			{
			case ESystemColumn::Type:
				bMatch = test.Kind == EValueKind::Text ? TestText(op, test.Values, Plan.FoldedTypeNames[Context.Type].View()) : TestBits(op, test.Values, Context.Type);
				break;
			case ESystemColumn::Name:
				bMatch = TestText(op, test.Values, Tables.Names.GetFolded(Context.Sub != InvalidRow ? Tables.SubEntries.Key[Context.Sub] : Tables.Assets.Name[Context.Asset]));
				break;
			case ESystemColumn::Size:
				bMatch = test.Kind == EValueKind::Real ? TestReal(op, test.Values, static_cast<double>(Tables.Files.Size[Context.File]))
					: TestInt(op, test.Values, static_cast<int64>(Tables.Files.Size[Context.File]));
				break;
			case ESystemColumn::ImportStatus:
				bMatch = TestBits(op, test.Values, static_cast<uint64>(Tables.Assets.ImportStatus[Context.Asset]));
				break;
			case ESystemColumn::CookRule:
				bMatch = TestBits(op, test.Values, static_cast<uint64>(Tables.Assets.CookRule[Context.Asset]));
				break;
			case ESystemColumn::Chunk:
				bMatch = test.Kind == EValueKind::Real ? TestReal(op, test.Values, static_cast<double>(Tables.Assets.Chunk[Context.Asset]))
					: TestInt(op, test.Values, static_cast<int64>(Tables.Assets.Chunk[Context.Asset]));
				break;
			default:
				break;
			}
			if (bMatch == (test.Op == EOp::Ne))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * The narrowest start for a query of main asset records without tag predicates: the ASSETS table itself
	 * (one row per asset) rather than every type table, which also hold the sub-entries and rows.
	 */
	[[nodiscard]] bool CanScanAssets () const noexcept
	{
		if (Plan.Records != ERecordScope::Assets)
		{
			return false;
		}
		const SQueryNode& root = Plan.Root;
		for (size_t type = 0u; type < root.AllowedTypes.size(); ++type)
		{
			if (root.AllowedTypes[type] != 0u && (Plan.AdmittedSubAssets[type] != 0u || !root.TypeTests[type].empty()))
			{
				return false;
			}
		}
		return true;
	}

	void ScanAssets (std::vector<SResult>& Out, uint32& OutCandidates)
	{
		const SQueryNode& root = Plan.Root;
		const SAssetsTable& assets = Tables.Assets;
		// Only tag tests read a type row: AnyOf alternatives' and a tag order key's.
		const bool bNeedTypeRow = !root.AnyOf.empty() || Plan.Order.Kind == EOrderKind::Tag;
		for (uint32 page = 0u; page < assets.File.GetPageCount(); ++page)
		{
			const std::span<const uint32> files = assets.File.GetPage(page);
			const std::span<const reflect::TypeId> types = assets.Type.GetPage(page);
			for (uint32 i = 0u; i < files.size(); ++i)
			{
				const uint32 file = files[i];
				const reflect::TypeId type = types[i];
				if (file == InvalidRow || type >= root.AllowedTypes.size() || root.AllowedTypes[type] == 0u)
				{
					continue;
				}
				++OutCandidates;
				SRowContext context;
				context.Value = page * RegistryPageRows + i;
				context.Asset = context.Value;
				context.File = file;
				context.Type = type;
				context.TypeRow = bNeedTypeRow ? assets.TypeRow[context.Value] : InvalidRow;
				if (root.bHasRowTests && (!PassesCheapTests(root, context) || (!root.bOnlyCheapRowTests && !MatchesNode(root, context, true))))
				{
					continue;
				}
				Out.push_back(MakeResult(context));
			}
		}
	}

	void ScanTypeTable (reflect::TypeId Type, std::vector<SResult>& Out, uint32& OutCandidates)
	{
		const SQueryNode& root = Plan.Root;
		if (root.AllowedTypes[Type] == 0u)
		{
			return;
		}
		// Which rows the scope admits: asset rows, sub-entry rows (refined by label below), or both.
		bool bAssets = false;
		bool bSubs = false;
		bool bClassify = false;
		bool bWantRows = false;
		switch (Plan.Records)
		{
		case ERecordScope::Assets:
			bAssets = true;
			bSubs = Plan.AdmittedSubAssets[Type] != 0u;
			bClassify = true;
			break;
		case ERecordScope::Rows:
			bSubs = true;
			bClassify = true;
			bWantRows = true;
			break;
		case ERecordScope::SubAssets:
			bSubs = true;
			bClassify = true;
			break;
		case ERecordScope::All:
			bAssets = true;
			bSubs = true;
			break;
		}
		if (!bAssets && !bSubs)
		{
			return;
		}
		const CTypeTable& table = Tables.Types[Type];
		const uint32 rows = table.Records.Size();
		if (table.LiveRows == 0u || rows == 0u)
		{
			return;
		}

		// Live rows of the admitted kinds.
		std::vector<uint64> masks((rows + 63u) / 64u, 0u);
		constexpr uint32 WordsPerPage = RegistryPageRows / 64u;
		for (uint32 page = 0u; page < table.Records.GetPageCount(); ++page)
		{
			const std::span<const uint32> records = table.Records.GetPage(page);
			const uint32 count = static_cast<uint32>(records.size());
			for (uint32 word = 0u; word * 64u < count; ++word)
			{
				const uint32 index = page * WordsPerPage + word;
				const uint32 base = word * 64u;
				const uint32 n = count - base < 64u ? count - base : 64u;
				uint64 bits = 0u;
				for (uint32 i = 0u; i < n; ++i)
				{
					const bool bSub = (records[base + i] & SubEntryBit) != 0u;
					bits |= static_cast<uint64>(bSub ? bSubs : bAssets) << i;
				}
				masks[index] = bits & ~table.Dead.GetWord(index);
			}
		}
		OutCandidates += table.LiveRows;

		for (const SColumnTest& test : root.TypeTests[Type])
		{
			ApplyColumnTest(table, test, masks);
		}

		for (uint32 word = 0u; word < masks.size(); ++word)
		{
			uint64 remaining = masks[word];
			while (remaining != 0u)
			{
				const uint32 bit = static_cast<uint32>(std::countr_zero(remaining));
				remaining &= remaining - 1u;
				const uint32 row = word * 64u + bit;
				const uint32 value = table.Records[row];
				// The context from what the scan knows: the type and its row are this table's.
				SRowContext context;
				context.Value = value;
				context.Type = Type;
				context.TypeRow = row;
				if ((value & SubEntryBit) != 0u)
				{
					context.Sub = value & ~SubEntryBit;
					if (bClassify && IsRow(context.Sub) != bWantRows)
					{
						continue;
					}
					context.Asset = Tables.SubEntries.Owner[context.Sub];
				}
				else
				{
					context.Asset = value;
				}
				context.File = Tables.Assets.File[context.Asset];
				if (root.bHasRowTests && (!PassesCheapTests(root, context) || (!root.bOnlyCheapRowTests && !MatchesNode(root, context, true))))
				{
					continue;
				}
				Out.push_back(MakeResult(context));
			}
		}
	}

	// ---- Results and order ----

	[[nodiscard]] SResult MakeResult (const SRowContext& Context)
	{
		SResult result;
		result.Value = Context.Value;
		result.Asset = Context.Asset;
		result.FoldedPath = GetFoldedFilePath(Tables, Context.File);
		if (Context.Sub != InvalidRow)
		{
			result.FoldedLabel = Tables.Names.GetFolded(Tables.SubEntries.Key[Context.Sub]);
		}
		result.Class = OrderAbsent;
		const SOrderSpec& order = Plan.Order;
		if (order.Kind == EOrderKind::Tag)
		{
			const uint32 key = Context.Type < order.KeyPerType.size() ? order.KeyPerType[Context.Type] : InvalidRow;
			if (key != InvalidRow && Context.TypeRow != InvalidRow)
			{
				const CTypeTable& table = Tables.Types[Context.Type];
				const STagColumn& column = table.Columns[key];
				if (column.PresentBits.Test(Context.TypeRow))
				{
					const uint64 bits = column.Values[Context.TypeRow];   // a MultiValue key's first value
					switch (table.Keys[key].Type)
					{
					case ETagType::Double:
						result.Class = OrderReal;
						result.Bits = bits;
						break;
					case ETagType::Name:
						result.Class = OrderText;
						result.FoldedText = Tables.Names.GetFolded(static_cast<uint32>(bits));
						result.RawText = Tables.Names.Get(static_cast<uint32>(bits));
						break;
					case ETagType::String:
						result.Class = OrderText;
						result.FoldedText = Tables.Strings.GetFolded(static_cast<uint32>(bits));
						result.RawText = Tables.Strings.Get(static_cast<uint32>(bits));
						break;
					default:
						result.Class = OrderInt;
						result.Bits = bits;
						break;
					}
				}
			}
		}
		else if (order.Kind == EOrderKind::System)
		{
			SetSystemOrder(Context, result);
		}
		return result;
	}

	void SetSystemOrder (const SRowContext& Context, SResult& Result)
	{
		const ESystemColumn column = Plan.Order.Column;
		if (column == ESystemColumn::Path)
		{
			Result.Class = OrderPath;
			return;
		}
		bool bFirst = true;
		ForEachSystemValue(Tables, &Plan, Context, column, true, Scratch, [&] (const SCell& Cell)
		{
			if (!bFirst)
			{
				return;
			}
			bFirst = false;
			switch (column)
			{
			case ESystemColumn::Type:
			case ESystemColumn::ImportStatus:
			case ESystemColumn::CookRule:
				// By name, so the order reads alphabetically in a result table.
				Result.Class = OrderText;
				Result.FoldedText = column == ESystemColumn::Type ? Plan.FoldedTypeNames[Context.Type].View() : Cell.Text;
				Result.RawText = Result.FoldedText;
				break;
			case ESystemColumn::Name:
			case ESystemColumn::Folder:
			case ESystemColumn::Labels:
			case ESystemColumn::PrimaryName:
				Result.Class = OrderText;
				Result.FoldedText = Cell.Text;
				break;
			default:
				Result.Class = OrderInt;
				Result.Bits = Cell.Bits;
				break;
			}
		});
		if (Result.Class == OrderText && Result.RawText.IsEmpty() && !Result.FoldedText.IsEmpty())
		{
			// Raw spelling as the tie-break between texts equal under the fold.
			FString scratch;
			ForEachSystemValue(Tables, &Plan, Context, column, false, scratch, [&] (const SCell& Cell)
			{
				if (Result.RawText.IsEmpty())
				{
					Result.RawText = Cell.Text;   // views into the pools (never into scratch for these columns)
				}
			});
		}
	}

	// File path (fold, then bytes), then the label (the asset record first).
	[[nodiscard]] int32 ComparePaths (const SResult& A, const SResult& B) const noexcept
	{
		if (A.Asset != B.Asset)
		{
			int32 compared = A.FoldedPath.Compare(B.FoldedPath);
			if (compared != 0)
			{
				return compared;
			}
			compared = Tables.Paths.Get(Tables.Files.Path[Tables.Assets.File[A.Asset]]).Compare(Tables.Paths.Get(Tables.Files.Path[Tables.Assets.File[B.Asset]]));
			if (compared != 0)
			{
				return compared;
			}
			return A.Asset < B.Asset ? -1 : 1;   // equal paths are never two live files; keep the order total anyway
		}
		const bool bSubA = (A.Value & SubEntryBit) != 0u;
		const bool bSubB = (B.Value & SubEntryBit) != 0u;
		if (bSubA != bSubB)
		{
			return bSubA ? 1 : -1;
		}
		if (!bSubA)
		{
			return 0;
		}
		int32 compared = A.FoldedLabel.Compare(B.FoldedLabel);
		if (compared == 0)
		{
			compared = Tables.Names.Get(Tables.SubEntries.Key[A.Value & ~SubEntryBit]).Compare(Tables.Names.Get(Tables.SubEntries.Key[B.Value & ~SubEntryBit]));
		}
		if (compared == 0 && A.Value != B.Value)
		{
			compared = A.Value < B.Value ? -1 : 1;
		}
		return compared;
	}

	// The order key alone: absent values sort last in either direction.
	[[nodiscard]] int32 CompareKeys (const SResult& A, const SResult& B) const noexcept
	{
		const auto direction = [this] (int32 Compared) { return Plan.Order.bAscending ? Compared : -Compared; };
		if (A.Class == OrderAbsent || B.Class == OrderAbsent)
		{
			return A.Class == B.Class ? 0 : (A.Class == OrderAbsent ? 1 : -1);
		}
		if (A.Class == OrderPath)
		{
			return direction(ComparePaths(A, B));
		}
		const bool bNumberA = A.Class == OrderInt || A.Class == OrderReal;
		const bool bNumberB = B.Class == OrderInt || B.Class == OrderReal;
		if (bNumberA && bNumberB)
		{
			if (A.Class == OrderInt && B.Class == OrderInt)
			{
				const int64 a = static_cast<int64>(A.Bits);
				const int64 b = static_cast<int64>(B.Bits);
				return direction((a > b) - (a < b));
			}
			const double a = A.Class == OrderInt ? static_cast<double>(static_cast<int64>(A.Bits)) : std::bit_cast<double>(A.Bits);
			const double b = B.Class == OrderInt ? static_cast<double>(static_cast<int64>(B.Bits)) : std::bit_cast<double>(B.Bits);
			return direction((a > b) - (a < b));
		}
		if (A.Class != B.Class)
		{
			return direction(A.Class < B.Class ? -1 : 1);
		}
		int32 compared = Sign(A.FoldedText.Compare(B.FoldedText));
		if (compared == 0)
		{
			compared = Sign(A.RawText.Compare(B.RawText));
		}
		return direction(compared);
	}

	[[nodiscard]] bool Less (const SResult& A, const SResult& B) const noexcept
	{
		if (Plan.Order.Kind != EOrderKind::None)
		{
			const int32 compared = CompareKeys(A, B);
			if (compared != 0)
			{
				return compared < 0;
			}
		}
		return ComparePaths(A, B) < 0;
	}
};
}


// ---------------- Public API ----------------

bool CompileQuery (const CRegistryView& View, const SAssetQuery& Query, SCompiledQuery& Out, SDiagnostic* OutError)
{
	Out = SCompiledQuery {};
	std::shared_ptr<SQueryPlan> plan = std::make_shared<SQueryPlan>();
	std::vector<SDiagnostic> notes;
	CQueryCompiler compiler(View, *plan, notes);
	if (!compiler.Compile(Query))
	{
		if (OutError != nullptr)
		{
			compiler.FillError(*OutError);
		}
		return false;
	}
	Out.Revision = View.GetRevision();
	Out.Plan = std::move(plan);
	Out.Notes = std::move(notes);
	return true;
}

uint32 RunQuery (const CRegistryView& View, const SCompiledQuery& Query, std::vector<SRecordId>& Out, SQueryStats* OutStats)
{
	const uint64 start = platform::ReadTimestamp();
	Out.clear();
	SQueryStats stats;
	if (!Query.IsValid() || Query.Revision != View.GetRevision())
	{
		stats.bWrongView = true;
		if (OutStats != nullptr)
		{
			*OutStats = stats;
		}
		return 0u;
	}
	const SRegistryTables& tables = GetTables(View);
	const SQueryPlan& plan = *Query.Plan;
	CQueryRunner runner(tables, plan);
	std::vector<SResult> results;
	runner.Collect(results, stats.Candidates);
	const uint64 scanned = platform::ReadTimestamp();
	runner.Sort(results);

	const uint32 matched = static_cast<uint32>(results.size());
	const uint64 first = (std::min)(static_cast<uint64>(plan.Offset), static_cast<uint64>(matched));
	const uint64 last = plan.Limit != 0u ? (std::min)(first + plan.Limit, static_cast<uint64>(matched)) : matched;
	Out.reserve(static_cast<size_t>(last - first));
	const uint32 revision = static_cast<uint32>(tables.Revision);
	for (uint64 i = first; i < last; ++i)
	{
		Out.push_back(SRecordId { results[static_cast<size_t>(i)].Value, revision });
	}
	const uint64 end = platform::ReadTimestamp();
	stats.Matched = matched;
	stats.Returned = static_cast<uint32>(Out.size());
	stats.ScanMilliseconds = platform::TimestampToMilliseconds(scanned - start);
	stats.SortMilliseconds = platform::TimestampToMilliseconds(end - scanned);
	stats.Milliseconds = platform::TimestampToMilliseconds(end - start);
	if (OutStats != nullptr)
	{
		*OutStats = stats;
	}
	return matched;
}

std::optional<uint32> RunQueryText (const CRegistryView& View, FStringView Text, std::vector<SRecordId>& Out, SDiagnostic* OutError, SQueryStats* OutStats)
{
	Out.clear();
	const std::optional<SAssetQuery> query = SAssetQuery::Parse(Text, OutError);
	if (!query)
	{
		return std::nullopt;
	}
	SCompiledQuery compiled;
	if (!CompileQuery(View, *query, compiled, OutError))
	{
		return std::nullopt;
	}
	return RunQuery(View, compiled, Out, OutStats);
}

void ForEachColumnValue (const CRegistryView& View, SRecordId Record, FStringView Key, TFunctionRef<void (const STagValue&)> Visit)
{
	if (!View.IsValid(Record))
	{
		return;
	}
	if (!Key.StartsWith('$'))
	{
		View.ForEachTagValue(Record, Key, Visit);
		return;
	}
	const std::optional<ESystemColumn> column = FindSystemColumn(Key);
	if (!column)
	{
		return;
	}
	const SRegistryTables& tables = GetTables(View);
	const SRowContext context = MakeContext(tables, Record.Value);
	FString scratch;
	ForEachSystemValue(tables, nullptr, context, *column, false, scratch, [&] (const SCell& Cell)
	{
		switch (*column)
		{
		case ESystemColumn::Type:
		case ESystemColumn::Name:
		case ESystemColumn::Path:
		case ESystemColumn::Folder:
		case ESystemColumn::Labels:
		case ESystemColumn::PrimaryName:
		case ESystemColumn::ImportStatus:
		case ESystemColumn::CookRule:
			Visit(STagValue::FromName(Cell.Text));
			return;
		default:
			Visit(STagValue::FromInt64(static_cast<int64>(Cell.Bits)));
			return;
		}
	});
}

FString DescribeCompiledQuery (const SCompiledQuery& Query)
{
	return Query.IsValid() ? Query.Plan->Description : FString();
}
}
