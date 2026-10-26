#include "AssetsPanel.h"

#include <optional>

#include "imgui.h"

#include "Assets/AssetRegistry.h"
#include "Assets/Registry/AssetQuery.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/RegistryView.h"
#include "Platform/Timestamp.h"
#include "UI/ImGuiString.h"
#include "Unicode/DisplayText.h"


namespace frt::editor
{
namespace
{
const ImVec4 ErrorColor(1.0f, 0.45f, 0.35f, 1.0f);
const ImVec4 NoteColor(1.0f, 0.8f, 0.2f, 1.0f);
}

void CAssetsPanel::SetQuery (FStringView Text)
{
	Query = FString(Text);
	bQueryChanged = true;
}

void CAssetsPanel::Draw (assets::CAssetRegistry* Registry)
{
	if (!bOpen)
	{
		return;
	}
	const bool bVisible = ImGui::Begin(Name, &bOpen);

	// Runs when the text changed (visible or not), when the registry moved while it shows, and once to begin with.
	if (Registry != nullptr && Registry->IsInitialScanComplete() && (bQueryChanged || RanAt == NeverRan || (bVisible && Registry->GetRevision() != RanAt)))
	{
		Refresh(*Registry);
	}

	if (bVisible)
	{
		ImGui::SetNextItemWidth(-1.0f);
		if (ui::InputTextWithHint("##query", "type:Texture Width>=1024 dir:/Project label:Hero ref:X is:broken-ref order:Size desc", Query))
		{
			bQueryChanged = true;
		}

		if (Registry == nullptr)
		{
			ImGui::TextDisabled("No asset registry here (Release, a project manager, or [Assets] Registry=0).");
		}
		else if (!Registry->IsInitialScanComplete())
		{
			ImGui::TextDisabled("Scanning the mounts...");
		}
		else if (!Error.IsEmpty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
			ui::TextUnformatted(Error);
			if (ErrorColumn > 0u)
			{
				// The query again with a caret under the term at fault.
				FString caret;
				for (uint32 i = 1u; i < ErrorColumn && i <= Query.Len(); ++i)
				{
					if ((static_cast<unsigned char>(Query[i - 1u]) & 0xC0u) != 0x80u)
					{
						caret += ' ';
					}
				}
				caret += '^';
				ui::TextUnformatted(Query);
				ui::TextUnformatted(caret);
			}
			ImGui::PopStyleColor();
		}
		else
		{
			ImGui::TextDisabled("%u of %u record(s)%s - %.2f ms, revision %llu", GetShownCount(), Matched,
				GetShownCount() < Matched ? " (add limit:N for more)" : "", QueryMilliseconds, static_cast<unsigned long long>(RanAt));
			for (const FString& note : Notes)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, NoteColor);
				ui::TextUnformatted(note);
				ImGui::PopStyleColor();
			}
		}

		constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
			| ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
		if (ImGui::BeginTable("##assets", 5, tableFlags))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Path");
			ImGui::TableSetupColumn("Size");
			ImGui::TableSetupColumn("Problems");
			ImGui::TableHeadersRow();
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(Rows.size()));
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				{
					const SRow& row = Rows[static_cast<size_t>(i)];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ui::TextUnformatted(row.Name);
					ImGui::TableNextColumn();
					ui::TextUnformatted(row.Type);
					ImGui::TableNextColumn();
					ui::TextUnformatted(row.Path);
					ImGui::TableNextColumn();
					ui::TextUnformatted(row.Size);
					ImGui::TableNextColumn();
					ui::TextUnformatted(row.Problems);
				}
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
}

void CAssetsPanel::Refresh (assets::CAssetRegistry& Registry)
{
	const uint64 start = platform::ReadTimestamp();
	bQueryChanged = false;
	RanAt = Registry.GetRevision();
	Rows.clear();
	Notes.clear();
	Error.Clear();
	ErrorColumn = 0u;
	Matched = 0u;

	SDiagnostic error;
	std::optional<assets::SAssetQuery> query = assets::SAssetQuery::Parse(Query, &error);
	// The registry's own thread: the live view, valid until the next commit (this frame's are done).
	const assets::CRegistryView& view = Registry.Live();
	assets::SCompiledQuery compiled;
	if (query)
	{
		if (query->LimitCount == 0u)
		{
			query->LimitCount = DefaultLimit;
		}
		if (!view.Compile(*query, compiled, &error))
		{
			query.reset();
		}
	}
	if (!query)
	{
		ErrorColumn = error.Column;
		Error = error.Column > 0u ? FString::Format("column {}: {}", error.Column, error.Message) : error.Message;
		return;
	}
	for (const SDiagnostic& note : compiled.Notes)
	{
		Notes.push_back(note.Message);
	}

	std::vector<assets::SRecordId> records;
	Matched = view.Run(compiled, records);
	Rows.reserve(records.size());
	for (const assets::SRecordId record : records)
	{
		SRow& row = Rows.emplace_back();
		row.Name = unicode::ToDisplayText(assets::FormatColumnValues(view, record, "$Name"));
		row.Type = assets::FormatColumnValues(view, record, "$Type");
		row.Path = unicode::ToDisplayText(assets::FormatColumnValues(view, record, "$Path"));
		row.Size = assets::FormatColumnValues(view, record, "$Size");
		row.Problems = assets::FormatColumnValues(view, record, "$Problems");
	}
	QueryMilliseconds = platform::TimestampToSeconds(platform::ReadTimestamp() - start) * 1000.0;
}
}
