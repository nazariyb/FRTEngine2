#pragma once

#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"


namespace frt
{
struct SDiagnostic;
}


/**
 * A registry query as a plain value (design §5.6, §5.7) and the text syntax that lowers to it (§5.8).
 *
 * SAssetQuery is what the Content Browser's search box, `FRT.bat assets query`, saved searches and C++ callers build;
 * it holds names and text, never registry ids, so it is built and kept anywhere (std containers and FString only,
 * any thread) and outlives every registry view. Compiling it against one view - type names to TypeIds with derived
 * types, tag names to key ids per type in scope, literals per key type, folders to pre-order intervals, labels to
 * name ids, ref:/dep: targets to records - is the query engine's job (Registry/QueryEngine, M3 G3), which reports an
 * unknown name with the Column recorded here.
 *
 * Semantics (UE's FARFilter: OR within a component, AND across components):
 * - Types, Folders, Guids and Tables match when any of their entries does (empty = no restriction).
 * - Labels must all hold (bExclude: must be absent). Predicates, TextTerms, Graph and SystemPredicates all hold.
 * - Each AnyOf group holds when at least one of its alternatives does; an alternative is a whole query of its own
 *   whose components AND with the enclosing ones (its Order, Limit and Records are unused).
 * - Records selects main assets, table rows, imported sub-assets or all; Order, LimitCount and OffsetCount shape the
 *   result.
 *
 * The reflected forms of §5.7 (OfType<T>(), Where(&T::Field, ...)) arrive with reflection in M5.
 */
namespace frt::assets
{
/**
 * The `is:` predicates of §5.8. The text names are kebab-case ("broken-ref"). Redirected (M4), Unsaved (the editor
 * overlay, M5/M6) and PendingMeta (M4's .meta files) parse today but have no data until their milestone, so they
 * match nothing yet (HasSystemPredicateData).
 */
enum class ESystemPredicate : uint8
{
	BrokenRef,     // has an Unresolved edge
	Unused,        // nothing references it
	LegacyRef,     // holds legacy path references (ERecordFlags::LegacyReferences)
	Excluded,      // excluded by a rule (EFileClass::Excluded, ERecordFlags::TypeExcluded)
	Problem,       // has diagnostics
	StaleTags,     // tags from an older schema (ERecordFlags::StaleTags)
	Primary,       // a primary asset (ERecordFlags::Primary)
	Redirected,    // loaded through a redirect (M4)
	Unsaved,       // has unsaved editor changes (M5/M6)
	PendingMeta,   // has no .meta yet (M4)
};

inline constexpr uint32 SystemPredicateCount = 10u;

// "broken-ref", "unused", "legacy-ref", "excluded", "problem", "stale-tags", "primary", "redirected", "unsaved",
// "pending-meta".
[[nodiscard]] FRT_CORE_API FStringLiteral GetSystemPredicateName (ESystemPredicate Predicate) noexcept;
// By text name, ASCII case ignored; nullopt for anything else.
[[nodiscard]] FRT_CORE_API std::optional<ESystemPredicate> FindSystemPredicate (FStringView Name) noexcept;
// False for the predicates that match nothing until their milestone (Redirected, Unsaved, PendingMeta).
[[nodiscard]] FRT_CORE_API bool HasSystemPredicateData (ESystemPredicate Predicate) noexcept;

// Column: where the term starts in the query text (bytes, from 1); 0 for a term built in code. The query engine
// reports names it cannot resolve at this position.

struct SQueryType
{
	FString Name;                 // a dotted type name, or a short name that must resolve uniquely
	bool bIncludeDerived = true;
	uint32 Column = 0u;
};

struct SQueryFolder
{
	FString MountPath;            // well-formed (IsWellFormedMountPath) when parsed from text
	bool bRecursive = true;
	uint32 Column = 0u;
};

struct SQueryLabel
{
	FString Label;
	bool bExclude = false;        // -label:Name
	uint32 Column = 0u;
};

struct SQueryTable
{
	SAssetId Table;               // built in code; invalid when named by text
	FString Name;                 // table:Ammo - a mount path or a unique $Name, resolved at compile
	uint32 Column = 0u;
};

// Values: one for Eq Ne Lt Le Gt Ge Prefix Contains Match, one or more for In, none for Exists and Missing. Values
// parsed from text are untyped literals (STagValue::FromLiteral) until compile types them per key.
struct SQueryPredicate
{
	FString Key;                  // a tag key or a system column ("$Size")
	ECompare Op = ECompare::Eq;
	std::vector<STagValue> Values;
	uint32 Column = 0u;
};

// References: records that reference the target (`ref:`, referencers). ReferencedBy: records the target references
// (`dep:`, dependencies).
enum class EQueryGraphDirection : uint8
{
	References,
	ReferencedBy,
};

struct SQueryGraph
{
	EQueryGraphDirection Direction = EQueryGraphDirection::References;
	SAssetId Target;              // built in code; invalid when named by text
	FString TargetText;           // a mount path (optional #label) or a unique $Name, resolved at compile
	bool bTransitive = false;     // ref:~X
	EDepFlags Mask {};            // edges followed: those with every bit of Mask (empty = all)
	uint32 Column = 0u;
};

struct SQuerySystemPredicate
{
	ESystemPredicate Predicate = ESystemPredicate::BrokenRef;
	uint32 Column = 0u;
};

struct SQueryText
{
	FString Text;                 // case-insensitive substring of name or path
	uint32 Column = 0u;
};

struct SQueryOrder
{
	FString Key;
	bool bAscending = true;
	uint32 Column = 0u;
};

struct SAssetQuery;

// One OR group: at least one alternative holds.
struct SQueryAnyOf
{
	std::vector<SAssetQuery> Alternatives;
};

struct FRT_CORE_API SAssetQuery
{
	std::vector<SQueryType> Types;
	std::vector<SQueryFolder> Folders;
	std::vector<SQueryLabel> Labels;
	std::vector<SAssetId> Guids;
	ERecordScope Records = ERecordScope::Assets;
	std::vector<SQueryTable> Tables;
	std::vector<SQueryPredicate> Predicates;
	std::vector<SQueryAnyOf> AnyOfGroups;
	std::vector<SQueryText> TextTerms;
	std::vector<SQueryGraph> Graph;
	std::vector<SQuerySystemPredicate> SystemPredicates;
	std::optional<SQueryOrder> Order;
	uint32 LimitCount = 0u;       // 0: no limit
	uint32 OffsetCount = 0u;

	SAssetQuery ();
	~SAssetQuery ();
	SAssetQuery (const SAssetQuery& Other);
	SAssetQuery (SAssetQuery&& Other) noexcept;
	SAssetQuery& operator= (const SAssetQuery& Other);
	SAssetQuery& operator= (SAssetQuery&& Other) noexcept;

	// ---- Builder (§5.7); each call adds to its component and returns *this ----
	SAssetQuery& OfType (FStringView TypeName, bool bIncludeDerived = true);
	SAssetQuery& InFolder (FStringView MountPath, bool bRecursive = true);
	SAssetQuery& WithLabel (FStringView Label);
	SAssetQuery& WithoutLabel (FStringView Label);
	SAssetQuery& WithId (const SAssetId& Id);
	SAssetQuery& Scope (ERecordScope InRecords);
	// Rows of that table (sets Records to Rows).
	SAssetQuery& RowsOf (const SAssetId& Table);
	SAssetQuery& RowsOf (FStringView TableName);
	SAssetQuery& Where (FStringView TagName, ECompare Op, STagValue Value);
	// Exists or Missing: no value.
	SAssetQuery& Where (FStringView TagName, ECompare Op);
	SAssetQuery& WhereIn (FStringView TagName, std::span<const STagValue> Values);
	SAssetQuery& AnyOf (std::initializer_list<SAssetQuery> Alternatives);
	SAssetQuery& AnyOf (std::vector<SAssetQuery> Alternatives);
	SAssetQuery& References (const SAssetId& Target, bool bTransitive = false, EDepFlags Mask = EDepFlags {});
	SAssetQuery& References (FStringView Target, bool bTransitive = false, EDepFlags Mask = EDepFlags {});
	SAssetQuery& ReferencedBy (const SAssetId& Source, bool bTransitive = false, EDepFlags Mask = EDepFlags {});
	SAssetQuery& ReferencedBy (FStringView Source, bool bTransitive = false, EDepFlags Mask = EDepFlags {});
	SAssetQuery& Is (ESystemPredicate Predicate);
	SAssetQuery& Text (FStringView Substring);
	SAssetQuery& OrderBy (FStringView TagName, bool bAscending = true);
	SAssetQuery& Limit (uint32 Count, uint32 Offset = 0u);

	// True when no component restricts anything (Records, Order and Limit aside): every record of the scope matches.
	[[nodiscard]] bool IsUnrestricted () const noexcept;

	/**
	 * One line describing every component in a fixed order, columns left out: what tests compare and what `assets
	 * query --explain` can print. E.g. `type:Texture -label:Debug where:Width Ge 4096`.
	 */
	[[nodiscard]] FString Describe () const;

	/**
	 * The §5.8 text syntax. Terms separated by spaces AND together (by component, as above); `or` between terms ORs
	 * them (an AnyOf group); parentheses group. Terms:
	 *   rifle  "assault rifle"  'x'   text (case-insensitive substring of name or path); quoted phrase
	 *   type:Texture  t:Texture        type with derived types; type=frt.Texture  exact type
	 *   dir:/Project/Data              folder, recursive; dir=/Project/Data  that folder only
	 *   label:Boss  -label:Debug       label / without the label (only label: terms take '-')
	 *   Key:v Key=v Key!=v Key<v Key<=v Key>v Key>=v   tag comparisons; ':' is Match (prefix for text, equality
	 *                                  otherwise, decided by compile); values may be quoted ('1' stays text)
	 *   Key in (a, b, "c d")           set membership
	 *   rows  table:Ammo               table rows / rows of one table (table: implies rows)
	 *   ref:X  ref:~X  dep:X           referencers / transitive referencers / dependencies (~ also for dep:)
	 *   is:broken-ref ...              system predicates (ESystemPredicate names)
	 *   order:Key [asc|desc]  limit:N  ordering, limit (once each, outside parentheses; so is rows)
	 * Words: `or`, `rows`, `in` (before '('), `asc`/`desc` (after order:) and the term names above are keywords,
	 * ASCII case ignored; quote a word to search for it as text. A bare value runs to a space or a parenthesis.
	 *
	 * nullopt on an error, with OutError's Severity Error, Line 1, Column (bytes from 1, where the offending term
	 * starts) and Message set. An empty or blank text is a valid query that restricts nothing.
	 */
	[[nodiscard]] static std::optional<SAssetQuery> Parse (FStringView Text, SDiagnostic* OutError = nullptr);
};
}
