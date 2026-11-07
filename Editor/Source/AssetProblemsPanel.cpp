#include "AssetProblemsPanel.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <system_error>

#include <Windows.h>
#include <shellapi.h>

#include "imgui.h"

#include "AssetsPanel.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Registry/RegistryView.h"
#include "Engine/Engine.h"
#include "Jobs/WorkerPool.h"
#include "Platform/Timestamp.h"
#include "UI/ImGuiString.h"
#include "Unicode/DisplayText.h"


namespace frt::editor
{
namespace
{
const ImVec4 ErrorColor(1.0f, 0.45f, 0.35f, 1.0f);
const ImVec4 WarningColor(1.0f, 0.8f, 0.2f, 1.0f);
const ImVec4 InfoColor(0.65f, 0.65f, 0.65f, 1.0f);

[[nodiscard]] const ImVec4& GetSeverityColor (ESeverity Severity) noexcept
{
	return Severity == ESeverity::Error ? ErrorColor : Severity == ESeverity::Warning ? WarningColor : InfoColor;
}

[[nodiscard]] double GetSeconds () noexcept
{
	return platform::TimestampToSeconds(platform::ReadTimestamp());
}

// "Content/Textures/a.png:12:3", or the subject when the problem names no file.
[[nodiscard]] FString DescribeWhere (const SDiagnostic& Problem)
{
	FString where = unicode::ToDisplayText(Problem.File.IsEmpty() ? Problem.Subject.View() : Problem.File.View());
	if (Problem.Line > 0u)
	{
		where += FString::Format(":{}", Problem.Line);
		if (Problem.Column > 0u)
		{
			where += FString::Format(":{}", Problem.Column);
		}
	}
	if (!Problem.FieldPath.IsEmpty())
	{
		where += FString::Format(" ({})", Problem.FieldPath);
	}
	return where;
}

// The shell would run it rather than open it (an executable, a script, a shortcut): PATHEXT's extensions and the other
// kinds Windows runs on a double click.
[[nodiscard]] bool IsShellRunnable (const std::filesystem::path& File)
{
	const auto upper = [] (std::wstring Text)
	{
		for (wchar_t& ch : Text)
		{
			ch = static_cast<wchar_t>(std::towupper(ch));
		}
		return Text;
	};
	const std::wstring extension = upper(File.extension().wstring());
	if (extension.empty())
	{
		return false;
	}
	std::wstring runnable = L".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC;.PS1;.LNK;.URL;.SCR;.PIF;.MSI;.MSP;.CPL;.HTA;.REG;.JAR;.APPREF-MS;";
	wchar_t pathExt[1024] = {};
	const DWORD length = GetEnvironmentVariableW(L"PATHEXT", pathExt, static_cast<DWORD>(std::size(pathExt)));
	if (length > 0u && length < std::size(pathExt))
	{
		runnable += pathExt;
		runnable += L';';
	}
	runnable = upper(std::move(runnable));
	size_t start = 0u;
	while (start < runnable.size())
	{
		const size_t end = runnable.find(L';', start);
		if (end == std::wstring::npos)
		{
			break;
		}
		if (runnable.compare(start, end - start, extension) == 0)
		{
			return true;
		}
		start = end + 1u;
	}
	return false;
}

void Tooltip (FStringView Text)
{
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip())
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
		ui::TextUnformatted(Text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}
}


CAssetProblemsPanel::~CAssetProblemsPanel ()
{
	ShutDown();
}

void CAssetProblemsPanel::SetFilter (const assets::SProblemFilter& NewFilter)
{
	Filter = NewFilter;
	Rebuild();
}

void CAssetProblemsPanel::WaitForRefresh ()
{
	if (Job != nullptr && Pool != nullptr)
	{
		// A pump loop, as every wait on the pool: the work needs no completion, and the pump keeps the registry going.
		(void)Pool->PumpUntil([this] { return Job->bFinished.load(std::memory_order_acquire); });
	}
	(void)TakeJob();
}

void CAssetProblemsPanel::RefreshNow (CEngine& Engine)
{
	WaitForRefresh();
	assets::CAssetRegistry* registry = Engine.GetAssetRegistry();
	if (registry == nullptr || !registry->IsInitialScanComplete())
	{
		return;
	}
	// The pump may commit while the list is made: again until the list is of the revision it ends at.
	for (uint32 attempt = 0u; attempt < 16u; ++attempt)
	{
		StartRefresh(Engine, true);
		WaitForRefresh();
		if (RanAt == registry->GetRevision())
		{
			return;
		}
	}
}

void CAssetProblemsPanel::GoTo (CAssetsPanel& Assets, const assets::SProblemEntry& Entry)
{
	// The inspector target arrives with M6.
	Assets.bOpen = true;
	Assets.SetQuery(assets::MakeGoToQuery(Entry));
	ImGui::SetWindowFocus(CAssetsPanel::Name);
}

void CAssetProblemsPanel::ShutDown ()
{
	if (Job != nullptr && Pool != nullptr)
	{
		// One not started yet is dropped (its guard says finished); one running is waited for.
		(void)Pool->Cancel(CancelGroup);
		(void)Pool->PumpUntil([this] { return Job->bFinished.load(std::memory_order_acquire); });
	}
	Job.reset();
}

bool CAssetProblemsPanel::TakeJob ()
{
	if (Job == nullptr || !Job->bFinished.load(std::memory_order_acquire))
	{
		return false;
	}
	const std::shared_ptr<SJob> job = std::move(Job);
	if (!job->bRan.load(std::memory_order_acquire))
	{
		bRefreshRequested = true;   // dropped by a drain before it ran: once more
		return false;
	}
	Entries = std::move(job->Entries);
	RanAt = job->Revision;
	ListChanges = job->Changes;
	ListMs = job->MadeMs;
	Milliseconds = job->Milliseconds;
	FixIts.assign(Entries.size(), std::nullopt);
	Counts = {};
	for (const assets::SProblemEntry& entry : Entries)
	{
		++(entry.Problem.Severity == ESeverity::Error ? Counts.Errors : entry.Problem.Severity == ESeverity::Warning ? Counts.Warnings : Counts.Infos);
	}
	Types = assets::GetProblemTypes(Entries);
	Rebuild();
	return true;
}

void CAssetProblemsPanel::Rebuild ()
{
	Groups = assets::GroupProblems(Entries, Filter);
	bRowsDirty = true;
}

void CAssetProblemsPanel::StartRefresh (CEngine& Engine, bool bUrgent)
{
	assets::CAssetRegistry& registry = *Engine.GetAssetRegistry();
	std::shared_ptr<const assets::CRegistryView> view = registry.Acquire();
	if (view == nullptr)
	{
		return;
	}
	bRefreshRequested = false;
	RefreshedAt = GetSeconds();
	Job = std::make_shared<SJob>();
	Job->Revision = view->GetRevision();
	Job->Changes = registry.GetRepairs() != nullptr ? registry.GetRepairs()->GetStats().Changes : 0u;
	Job->MadeMs = assets::GetLifecycleNowMs();
	Pool = &Engine.GetWorkerPool();
	if (CancelGroup == 0u)
	{
		CancelGroup = Pool->CreateCancelGroup();
	}
	// The options point into the registry, which outlives the job: ShutDown cancels or waits for it before the registry goes.
	// Background work, unless someone waits for it.
	Pool->Submit(bUrgent ? jobs::EWorkPriority::High : jobs::EWorkPriority::Low, CancelGroup,
		[guard = SFinishGuard(Job), view = std::move(view), options = assets::MakeProblemListOptions(registry)] ()
		{
			const uint64 start = platform::ReadTimestamp();
			guard.Job->Entries = assets::ListAssetProblems(*view, options);
			guard.Job->Milliseconds = platform::TimestampToSeconds(platform::ReadTimestamp() - start) * 1000.0;
			guard.Job->bRan.store(true, std::memory_order_release);
		});
}

void CAssetProblemsPanel::ResolvePendingFixes (CEngine& Engine)
{
	assets::CAssetRegistry* registry = Engine.GetAssetRegistry();
	const assets::CAssetRepairs* repairs = registry != nullptr ? registry->GetRepairs() : nullptr;
	const uint64 now = assets::GetLifecycleNowMs();
	std::erase_if(Pending, [&] (assets::SPendingFix& Fix)
	{
		FString resultMessage;
		const assets::EFixResult result = repairs != nullptr ? repairs->GetFixResult(Fix.Ticket, &resultMessage) : assets::EFixResult::Forgotten;
		FString message;
		const FString what = FString::Format("{} - {}", Fix.Label, unicode::ToDisplayText(Fix.Subject));
		switch (assets::ResolvePendingFix(Fix, result, resultMessage, Entries, ListMs, now, &message))
		{
		case assets::EFixOutcome::Pending:
			return false;
		case assets::EFixOutcome::Fixed:
			Post("Fixed: " + what, false);
			break;
		case assets::EFixOutcome::Refused:
			Post(FString::Format("Refused: {} ({})", what, unicode::ToDisplayText(message)), true);
			break;
		case assets::EFixOutcome::NotApplied:
			Post(FString::Format("Not applied: {} ({})", what, unicode::ToDisplayText(message)), true);
			break;
		case assets::EFixOutcome::NoChange:
			Post(FString::Format("No change {} s after it was applied: {} ({})", assets::FixOutcomeTimeoutMs / 1000u, what,
				unicode::ToDisplayText(message)), true);
			break;
		}
		return true;
	});
}

void CAssetProblemsPanel::MaybeRefresh (CEngine& Engine, bool bVisible)
{
	assets::CAssetRegistry* registry = Engine.GetAssetRegistry();
	if (registry == nullptr || !registry->IsInitialScanComplete() || Job != nullptr)
	{
		return;
	}
	const assets::CAssetRepairs* repairs = registry->GetRepairs();
	const bool bMoved = registry->GetRevision() != RanAt || (repairs != nullptr && repairs->GetStats().Changes != ListChanges);
	const bool bDue = GetSeconds() - RefreshedAt >= MinRefreshSeconds;
	// An applied fix waits for a list made after it, shown or not, moved or not (its problem may simply stay).
	const bool bAwaitingList = std::any_of(Pending.begin(), Pending.end(), [] (const assets::SPendingFix& Fix) { return Fix.DoneMs != 0u; });
	if (bRefreshRequested || RanAt == NeverRan || (bDue && ((bMoved && bVisible) || bAwaitingList)))
	{
		StartRefresh(Engine, false);
	}
}

void CAssetProblemsPanel::Post (FString Text, bool bError)
{
	Status = std::move(Text);
	bStatusIsError = bError;
}

void CAssetProblemsPanel::Draw (CEngine& Engine, CAssetsPanel& Assets)
{
	assets::CAssetRegistry* registry = Engine.GetAssetRegistry();
	(void)TakeJob();
	if (!Pending.empty())
	{
		ResolvePendingFixes(Engine);   // the lifecycle's outcome needs no new list
	}
	if (registry != nullptr && registry->GetRepairs() != nullptr)
	{
		// The lifecycle's fix-its and providers moved without a revision: what each entry offers is asked again.
		if (const uint64 changes = registry->GetRepairs()->GetStats().Changes; changes != FixItsChanges)
		{
			FixItsChanges = changes;
			FixIts.assign(Entries.size(), std::nullopt);
		}
	}

	if (!bOpen)
	{
		MaybeRefresh(Engine, false);   // a fix applied before it was closed still gets its lists
		return;
	}
	const bool bVisible = ImGui::Begin(Name, &bOpen);
	MaybeRefresh(Engine, bVisible);
	if (bVisible)
	{
		RowsDrawn = 0u;
	}

	if (bVisible)
	{
		if (registry == nullptr)
		{
			ImGui::TextDisabled("No asset registry here (Release, a project manager, or [Assets] Registry=0).");
		}
		else if (RanAt == NeverRan)
		{
			ImGui::TextDisabled(registry->IsInitialScanComplete() ? "Validating..." : "Scanning the mounts...");
		}
		else
		{
			DrawFilters();
			uint32 shown = 0u;
			for (const assets::SProblemGroup& group : Groups)
			{
				shown += static_cast<uint32>(group.Entries.size());
			}
			ImGui::TextDisabled("%u problem(s) in %u asset(s) shown of %u - revision %llu, %.1f ms%s", shown, static_cast<uint32>(Groups.size()),
				static_cast<uint32>(Entries.size()), static_cast<unsigned long long>(RanAt), Milliseconds, Job != nullptr ? " (validating...)" : "");
			if (registry->GetRepairs() == nullptr)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("- no meta lifecycle runs here: nothing to apply, and no VCS is asked (no VCS003)");
			}
			if (!Status.IsEmpty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, bStatusIsError ? ErrorColor : InfoColor);
				ui::TextUnformatted(Status);
				ImGui::PopStyleColor();
			}

			if (bRowsDirty)
			{
				Rows.clear();
				for (uint32 g = 0u; g < Groups.size(); ++g)
				{
					Rows.push_back({ g, -1 });
					if (!Collapsed.contains(Groups[g].Asset))
					{
						for (const uint32 entry : Groups[g].Entries)
						{
							Rows.push_back({ g, static_cast<int32>(entry) });
						}
					}
				}
				bRowsDirty = false;
			}

			constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
				| ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
			if (ImGui::BeginTable("##problems", 4, tableFlags))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Asset / problem");
				ImGui::TableSetupColumn("Code");
				ImGui::TableSetupColumn("Where");
				ImGui::TableSetupColumn("Actions");
				ImGui::TableHeadersRow();
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(Rows.size()));
				while (clipper.Step())
				{
					for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					{
						++RowsDrawn;
						const SRow row = Rows[static_cast<size_t>(i)];
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (row.Entry >= 0)
						{
							DrawEntry(Engine, Assets, static_cast<uint32>(row.Entry));
							continue;
						}
						const assets::SProblemGroup& group = Groups[row.Group];
						const bool bCollapsed = Collapsed.contains(group.Asset);
						ImGui::PushID(i);
						ImGui::SetNextItemOpen(!bCollapsed, ImGuiCond_Always);
						ImGui::PushStyleColor(ImGuiCol_Text, GetSeverityColor(group.Worst));
						const FString label = FString::Format("{}{}{} - {} problem(s)", unicode::ToDisplayText(group.Asset),
							group.Type.IsEmpty() ? "" : "  ", group.Type, group.Entries.size());
						const bool bNowOpen = ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen,
							"%.*s", static_cast<int>(label.Len()), label.GetData());
						ImGui::PopStyleColor();
						ImGui::PopID();
						if (bNowOpen == bCollapsed)
						{
							if (bNowOpen)
							{
								Collapsed.erase(group.Asset);
							}
							else
							{
								Collapsed.insert(group.Asset);
							}
							bRowsDirty = true;
						}
					}
				}
				ImGui::EndTable();
			}
		}
	}
	ImGui::End();
}

void CAssetProblemsPanel::DrawFilters ()
{
	bool bChanged = false;
	bChanged |= ImGui::Checkbox(FString::Format("Errors ({})###errors", Counts.Errors).CStr(), &Filter.bErrors);
	ImGui::SameLine();
	bChanged |= ImGui::Checkbox(FString::Format("Warnings ({})###warnings", Counts.Warnings).CStr(), &Filter.bWarnings);
	ImGui::SameLine();
	bChanged |= ImGui::Checkbox(FString::Format("Infos ({})###infos", Counts.Infos).CStr(), &Filter.bInfos);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
	bChanged |= ui::InputTextWithHint("##code", "code", Filter.Code);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
	if (ImGui::BeginCombo("##type", Filter.Type.IsEmpty() ? "any type" : Filter.Type.CStr()))
	{
		if (ImGui::Selectable("any type", Filter.Type.IsEmpty()))
		{
			Filter.Type.Clear();
			bChanged = true;
		}
		for (const FString& type : Types)
		{
			if (ImGui::Selectable(type.CStr(), type == Filter.Type))
			{
				Filter.Type = type;
				bChanged = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.0f);
	bChanged |= ui::InputTextWithHint("##folder", "folder: /Project/Textures", Filter.Folder);
	if (bChanged)
	{
		Rebuild();
	}
}

void CAssetProblemsPanel::DrawEntry (CEngine& Engine, CAssetsPanel& Assets, uint32 Index)
{
	const assets::SProblemEntry& entry = Entries[Index];
	const SDiagnostic& problem = entry.Problem;
	assets::CAssetRegistry* registry = Engine.GetAssetRegistry();
	ImGui::PushID(static_cast<int>(Index));

	ImGui::Indent();
	ImGui::PushStyleColor(ImGuiCol_Text, GetSeverityColor(problem.Severity));
	ui::TextUnformatted(GetSeverityName(problem.Severity));
	ImGui::PopStyleColor();
	ImGui::SameLine();
	const FString message = unicode::ToDisplayText(problem.Message);
	ui::TextUnformatted(message);
	Tooltip(FString::Format("{}\n{}", unicode::ToDisplayText(problem.Subject), message));
	ImGui::Unindent();

	ImGui::TableNextColumn();
	ui::TextUnformatted(FormatDiagnosticCode(problem.Code));
	ImGui::TableNextColumn();
	ui::TextUnformatted(DescribeWhere(problem));

	ImGui::TableNextColumn();
	if (ImGui::SmallButton("Go to"))
	{
		GoTo(Assets, entry);
	}
	Tooltip("Lists it in the Assets panel");
	if (registry != nullptr)
	{
		if (const std::optional<std::filesystem::path> file = assets::GetProblemFilePath(problem, registry->GetMounts()); file.has_value())
		{
			ImGui::SameLine();
			std::error_code ec;
			const bool bExists = std::filesystem::is_regular_file(*file, ec);
			const bool bRunnable = IsShellRunnable(*file);
			const FString shown = unicode::ToDisplayText(FString::FromPath(*file));
			ImGui::BeginDisabled(!bExists);
			if (ImGui::SmallButton(bRunnable ? "Show" : "Open"))
			{
				// A document opens with its default verb; a file the shell would run is only shown in Explorer.
				const std::wstring select = L"/select,\"" + file->wstring() + L"\"";
				const HINSTANCE result = bRunnable ? ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL)
					: ShellExecuteW(nullptr, L"open", file->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				const INT_PTR code = reinterpret_cast<INT_PTR>(result);
				if (code <= 32)
				{
					Post(FString::Format("Cannot open {}: {}", shown, code == SE_ERR_FNF || code == SE_ERR_PNF ? "it does not exist"
						: code == SE_ERR_NOASSOC || code == SE_ERR_ASSOCINCOMPLETE ? "no program is set to open it" : "the shell refused"), true);
				}
			}
			ImGui::EndDisabled();
			Tooltip(!bExists ? FString::Format("{} does not exist (yet)", shown)
				: bRunnable ? FString::Format("Shows {} in Explorer: opened, it would run", shown) : FString::Format("Opens {}", shown));
			if (problem.Line > 0u)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Copy line"))
				{
					// An IDE's go-to-location form: path(line,column).
					const FString location = FString::Format("{}({},{})", FString::FromPath(*file), problem.Line, std::max(problem.Column, 1u));
					ImGui::SetClipboardText(location.CStr());
					Post("Copied: " + unicode::ToDisplayText(location), false);
				}
				Tooltip(FString::Format("Copies the file with line {} and column {}", problem.Line, std::max(problem.Column, 1u)));
			}
		}

		std::optional<std::vector<assets::SProblemFixIt>>& fixIts = FixIts[Index];
		if (!fixIts.has_value())
		{
			const assets::SProblemFixContext context { &registry->GetMounts(), registry->GetRepairs(), &registry->Live() };
			fixIts = assets::GetProblemFixIts(problem, context);
		}
		std::optional<assets::SProblemFixIt> clicked;
		for (uint32 k = 0u; k < fixIts->size(); ++k)
		{
			const assets::SProblemFixIt& fix = (*fixIts)[k];
			ImGui::PushID(static_cast<int>(k));
			ImGui::SameLine();
			switch (fix.Kind)
			{
			case assets::EProblemFixKind::Apply:
			{
				const bool bQueued = std::any_of(Pending.begin(), Pending.end(), [&] (const assets::SPendingFix& Fixing)
				{
					return Fixing.Code == problem.Code && Fixing.Subject == problem.Subject;
				});
				ImGui::BeginDisabled(!fix.Disabled.IsEmpty() || bQueued);
				if (ImGui::SmallButton(fix.Label.CStr()))
				{
					clicked = fix;
				}
				ImGui::EndDisabled();
				if (!fix.Disabled.IsEmpty())
				{
					Tooltip(FString::Format("{}\n(not here: {})", fix.Text, fix.Disabled));
				}
				else
				{
					Tooltip(bQueued ? FStringView("Queued: its outcome shows with a later list") : fix.Text.View());
				}
				break;
			}
			case assets::EProblemFixKind::Copy:
				if (ImGui::SmallButton(FString::Format("Copy: {}", fix.Label).CStr()))
				{
					ImGui::SetClipboardText(fix.Text.CStr());
					Post("Copied: " + unicode::ToDisplayText(fix.Text), false);
				}
				Tooltip(FString::Format("{}\n(shown and copied, never run here)", unicode::ToDisplayText(fix.Text)));
				break;
			case assets::EProblemFixKind::Note:
				ImGui::TextDisabled("(%.*s)", static_cast<int>(fix.Label.Len()), fix.Label.GetData());
				Tooltip(fix.Text);
				break;
			}
			ImGui::PopID();
		}
		if (clicked.has_value())
		{
			ApplyFix(Engine, Index, *clicked);
		}
	}
	ImGui::PopID();
}

void CAssetProblemsPanel::ApplyFix (CEngine& Engine, uint32 Index, const assets::SProblemFixIt& Fix)
{
	assets::CAssetRegistry* registry = Engine.GetAssetRegistry();
	assets::CAssetRepairs* repairs = registry != nullptr ? registry->GetRepairs() : nullptr;
	if (repairs == nullptr || Index >= Entries.size())
	{
		return;
	}
	const SDiagnostic problem = Entries[Index].Problem;
	const FString what = FString::Format("{} - {}", Fix.Label, unicode::ToDisplayText(problem.Subject));
	const assets::SRepairReport report = repairs->ApplyFix(Fix.Action);
	if (report.Applied + report.Refused > 0u)
	{
		// A host whose lifecycle is not live applies at once (AssetRepairs.h): the outcome is in the report.
		Post(FString::Format("{}: {}", report.Refused > 0u ? "Refused" : "Fixed", what), report.Refused > 0u);
		RequestRefresh();
		return;
	}
	if (!report.Problems.empty())
	{
		Post(FString::Format("Not applied: {} ({})", what, unicode::ToDisplayText(report.Problems.front().Message)), true);
		return;
	}
	// The live editor's ApplyFix queues the op: the lifecycle says when it ran, and the lists that follow whether it fixed.
	Pending.push_back(assets::MakePendingFix(problem, Fix.Label, report.Ticket));
	Post("Queued: " + what, false);
}
}
