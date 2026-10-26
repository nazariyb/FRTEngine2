#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Registry/AssetQuery.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Functional/FunctionRef.h"


/**
 * The registry's query engine (design §5.6, §5.7): compiles an SAssetQuery against one CRegistryView and runs it
 * over that view's tables (RegistryTables.h). Both are pure reads of an immutable view, so they run on any thread,
 * allocate only on the CRT heap and never block a commit.
 *
 * §5.7 spells these as CRegistryView::Compile/Run; they are free functions here because the view is the store's
 * (RegistryView.h) and the engine only reads its tables. A compiled query is a plain value the caller keeps (there
 * is no shared plan cache); it holds ids of the view it was compiled against (type keys, folder rows, records of
 * ref:/dep: targets), so RunQuery refuses it on any other revision or store: compile again after a commit or a
 * reload.
 *
 * Compilation resolves type names (CAssetTypeRegistry: dotted, or a unique short name; M3 has no hierarchy, so
 * "derived types included" adds nothing), tag names to key indices per type in scope, literals per key type, folders
 * to pre-order intervals (all folders equal under the portable fold), labels, table names, ref:/dep: targets (a
 * mount path with an optional "#label", else a unique $Name) and the order key. Anything it cannot resolve fails
 * the compile with one positioned diagnostic: Severity Error, Line 1 and the Column the parser recorded for the
 * term (0 for a term built in code). Terms that are valid but match nothing until their milestone (is:redirected,
 * is:unsaved, is:pending-meta) compile and leave a note in SCompiledQuery::Notes.
 *
 * Semantics beyond SAssetQuery's own (AssetQuery.h):
 *  - Records. Assets: main asset records - plus the imported sub-assets of a sub-asset-only type that a Types term
 *    names (`type:Mesh` finds meshes, which are sub-entries of models). A sub-asset-only type is one that no
 *    importer of the table (available or not) has as the main type of a file (ImporterTable.h): frt.Mesh is,
 *    frt.Texture and frt.Material are not, so `type:Texture` lists texture files and not the images a glTF embeds
 *    (Records SubAssets or All reach those). The admission belongs to the AnyOf alternative whose term named the
 *    type: a sub-asset is in scope when the root, or an alternative it matches (at any depth), admits its type.
 *    Rows: table rows; SubAssets: imported sub-assets; All: every record. A sub-entry is a row when its label holds
 *    no ':' (row keys never do, §3.6) and an imported sub-asset otherwise (importer labels are "kind:name", §4.3).
 *    M5's Document importer, whose main type is the document's $type, revisits which types are sub-asset-only.
 *  - A sub-entry is matched by its file's folder, labels, size, import status, cook rule and chunk (they belong to
 *    the file); its own type, label ($Name), "file#label" path, tags, edges and problems.
 *  - A tag predicate matches records of the types in scope that declare the key; a bare key matches every type in
 *    scope that declares it, and no such type is a compile error. MultiValue keys match when any value does; Ne
 *    and the orderings need a present value (an absent value never satisfies a comparison); Missing is "no value".
 *  - Text keys (Name, String) and the text columns compare under the portable fold (FString::FoldCase); ':' is
 *    a prefix match for them and equality for every other type.
 *  - Graph terms follow resolved edges that carry every bit of the mask (empty: every edge). ref:X - records whose
 *    edges resolve to X (an asset record when any edge of its file does; a sub-entry when it declared one); dep:X -
 *    the records X's edges resolve to (the target file's asset record, and the sub-entry an edge names). With '~'
 *    the walk is transitive over files, breadth-first with a byte bitmap: every record of a reached file matches,
 *    X's own file only when an edge leads back to it.
 *  - Results come back in a deterministic order: the order key (absent values last), then the file's mount path
 *    (portable fold, then bytes), then the sub-entry label (the asset record first). OrderBy with a Limit
 *    partial-sorts; Offset and Limit cut the sorted list.
 *
 * System columns (§5.2), usable in predicates, OrderBy and ForEachColumnValue: $Type, $Name, $Path, $Folder,
 * $Labels, $Size, $ImportStatus, $Problems, $Referencers (distinct referencing files), $Dependencies (live edges
 * declared), $CookRule, $Chunk, $PrimaryName.
 */
namespace frt::assets
{
class CRegistryView;
struct SQueryPlan;   // QueryEngine.cpp

enum class ESystemColumn : uint8
{
	Type,
	Name,
	Path,
	Folder,
	Labels,
	Size,
	ImportStatus,
	Problems,
	Referencers,
	Dependencies,
	CookRule,
	Chunk,
	PrimaryName,
};

inline constexpr uint32 SystemColumnCount = 13u;

// "$Type", "$Name", ...
[[nodiscard]] FRT_CORE_API FStringLiteral GetSystemColumnName (ESystemColumn Column) noexcept;
// By name with its '$', ASCII case ignored; nullopt for anything else.
[[nodiscard]] FRT_CORE_API std::optional<ESystemColumn> FindSystemColumn (FStringView Name) noexcept;

/**
 * A query compiled against one view: the plan (immutable, shared between copies) and the view it is bound to - its
 * revision and its store. Store pins the view's mount table: every view of one store at one revision shares it,
 * and a view of another store (or of a store that adopted an image into fresh tables) never holds the same one;
 * holding it keeps its address from being reused. Notes: diagnostics of terms that compiled but cannot match yet
 * (Severity Info, positioned like errors).
 */
struct SCompiledQuery
{
	uint64 Revision = 0u;
	std::shared_ptr<const void> Store;
	std::shared_ptr<const SQueryPlan> Plan;
	std::vector<SDiagnostic> Notes;

	[[nodiscard]] bool IsValid () const noexcept { return Plan != nullptr; }
};

struct SQueryStats
{
	uint32 Candidates = 0u;          // records the filters looked at
	uint32 Matched = 0u;             // records that matched, before Offset and Limit
	uint32 Returned = 0u;            // records written to the output
	double Milliseconds = 0.0;       // the whole run
	double ScanMilliseconds = 0.0;   // graph sets and filters
	double SortMilliseconds = 0.0;   // ordering, Offset and Limit
	bool bWrongView = false;         // the query was compiled against another revision or store: nothing ran
};

/**
 * Compiles Query against View. False on the first term that does not resolve, with OutError filled (Out is left
 * empty); true otherwise, with Out ready for RunQuery on this view.
 */
FRT_CORE_API bool CompileQuery (const CRegistryView& View, const SAssetQuery& Query, SCompiledQuery& Out, SDiagnostic* OutError = nullptr);

/**
 * Runs a compiled query: Out receives the records of the page [Offset, Offset + Limit) in result order (the whole
 * result without a Limit). Returns how many records matched before Offset and Limit. A query compiled against
 * another revision or store (or not compiled) returns 0 with an empty Out and OutStats->bWrongView set.
 */
FRT_CORE_API uint32 RunQuery (const CRegistryView& View, const SCompiledQuery& Query, std::vector<SRecordId>& Out, SQueryStats* OutStats = nullptr);

/**
 * Parse (§5.8), compile and run in one call - the search box, `FRT.bat assets query`. nullopt on a parse or compile
 * error (OutError positioned in Text); else the match count as RunQuery returns it. OutNotes receives the compile
 * notes (SCompiledQuery::Notes: "is:redirected matches nothing until its milestone"); it is cleared first.
 */
FRT_CORE_API std::optional<uint32> RunQueryText (const CRegistryView& View, FStringView Text, std::vector<SRecordId>& Out,
	SDiagnostic* OutError = nullptr, SQueryStats* OutStats = nullptr, std::vector<SDiagnostic>* OutNotes = nullptr);

/**
 * The values of one column of a record, as a result table shows them: a system column ("$Size"; $Type, $Name,
 * $Path, $Folder, $Labels, $PrimaryName, $ImportStatus and $CookRule as Name values, the counts as Int64) or a tag
 * key of the record's type (every value of a MultiValue key). Nothing for an invalid record or an unknown key.
 */
FRT_CORE_API void ForEachColumnValue (const CRegistryView& View, SRecordId Record, FStringView Key, TFunctionRef<void (const STagValue&)> Visit);

/**
 * The compiled plan in one line, for tests and `assets query --explain`: scope, types, typed predicates per type
 * ("frt.Texture.Width Ge 4096"), folders as intervals, graph targets, order and page. Empty for an invalid query.
 */
[[nodiscard]] FRT_CORE_API FString DescribeCompiledQuery (const SCompiledQuery& Query);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ESystemColumn,
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Type),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Name),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Path),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Folder),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Labels),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Size),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, ImportStatus),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Problems),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Referencers),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Dependencies),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, CookRule),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, Chunk),
	FRT_ENUM_ENTRY(frt::assets::ESystemColumn, PrimaryName));
