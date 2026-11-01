#pragma once

#include <vector>

#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::assets
{
class CAssetRegistry;
}


namespace frt::editor
{
/**
 * The Assets panel (design §5.8, §16.2 M3: "a read-only list panel"): a search box that takes the registry's text
 * query syntax (type:Texture Width>=1024 dir:/Project/Textures label:Hero ref:X is:broken-ref order:Width desc ...),
 * the positioned error when the text does not parse or compile, and a table of what matches - Name, Type, Path, Size,
 * Problems - in the query engine's order.
 *
 * It polls (§5.9: immediate-mode panels compare CAssetRegistry::GetRevision() once per frame; no subscription): the
 * query runs again when its text changed, and when the registry's revision moved while the panel is visible - a
 * hidden tab costs nothing per commit and catches up when it shows. A query without its own limit:N gets DefaultLimit,
 * so a large result stays cheap to run and draw; the status line says how many matched. The rows are copied text, so
 * nothing here keeps an SRecordId across revisions. Read-only in M3: it selects and opens nothing.
 */
class CAssetsPanel
{
public:
	// The window's name, which the default layout docks (CEditorLayout).
	static constexpr const char* Name = "Assets";
	static constexpr uint32 DefaultLimit = 500u;

	// Every frame, beside the debug tools - under a fullscreen viewport too: a docked window left out for a frame
	// gives up its dock node. Registry null: no registry runs (Release, [Assets] Registry=0).
	void Draw (assets::CAssetRegistry* Registry);

	// The search text, as if typed: it runs at the next Draw, visible or not (scripted runs).
	void SetQuery (FStringView Text);
	[[nodiscard]] const FString& GetQuery () const { return Query; }
	// Of the last run: the records that matched (before the limit), the rows shown, the error ("" when it ran), and
	// the registry revision the rows come from (NeverRan before the first run).
	[[nodiscard]] uint32 GetResultCount () const { return Matched; }
	[[nodiscard]] uint32 GetShownCount () const { return static_cast<uint32>(Rows.size()); }
	[[nodiscard]] const FString& GetError () const { return Error; }
	[[nodiscard]] uint64 GetShownRevision () const { return RanAt; }
	static constexpr uint64 NeverRan = ~0ull;

	bool bOpen = true;

private:
	struct SRow
	{
		FString Name;
		FString Type;
		FString Path;
		FString Size;
		FString Problems;
	};

	void Refresh (assets::CAssetRegistry& Registry);

	FString Query;
	bool bQueryChanged = true;
	uint64 RanAt = NeverRan;
	uint32 Matched = 0u;
	std::vector<SRow> Rows;
	FString Error;                 // "column <n>: <message>", empty when the query ran
	uint32 ErrorColumn = 0u;       // 1-based byte column of the error in Query, 0 when none
	std::vector<FString> Notes;    // terms that match nothing until their milestone
	double QueryMilliseconds = 0.0;
};
}
