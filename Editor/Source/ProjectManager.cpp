#include "ProjectManager.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "imgui.h"

#include "EnginePaths.h"
#include "Window.h"
#include "Config/IniFile.h"
#include "Containers/WideString.h"
#include "Engine/Engine.h"
#include "Graphics/Render/Renderer.h"
#include "Platform/Platform.h"
#include "Project/UserRegistry.h"
#include "UI/ImGuiLayer.h"
#include "UI/ImGuiString.h"


namespace frt::editor
{
namespace
{
constexpr std::chrono::milliseconds BackgroundFrameSleep(33);
constexpr ImVec4 ErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
constexpr ImVec4 DimColor(0.6f, 0.6f, 0.6f, 1.0f);

// What the fields take: a project name has at most 64 characters (project::IsValidProjectName),
// and no location anyone types is longer.
constexpr uint32 MaxNameBytes = 64u;
constexpr uint32 MaxLocationBytes = 1023u;

// The configuration this editor was built in: an editor opens projects in its own.
constexpr FStringLiteral ConfigName = FRT_CONFIG_NAME;

std::filesystem::path SettingsFile ()
{
	return paths::GetEditorUserDir() / "ProjectManager.ini";
}

std::filesystem::path EditorExecutable (const std::filesystem::path& EngineDir)
{
	return EngineDir / "Binaries" / FRT_PLATFORM_NAME / ("Editor-" FRT_CONFIG_NAME ".exe");
}

std::filesystem::path ModuleFile (const project::SProjectDescriptor& Project)
{
	return Project.GetDir() / "Binaries" / FRT_PLATFORM_NAME / FString::Format("{}-{}.dll", Project.Module, ConfigName).ToPath();
}

bool Exists (const std::filesystem::path& Path)
{
	std::error_code ec;
	return std::filesystem::exists(Path, ec);
}

bool IsSameDir (const std::filesystem::path& A, const std::filesystem::path& B)
{
	std::error_code ec;
	return std::filesystem::equivalent(A, B, ec);
}

FString FormatTime (int64 Seconds)
{
	if (Seconds <= 0)
	{
		return FString();
	}
	const std::time_t time = static_cast<std::time_t>(Seconds);
	std::tm local {};
	localtime_s(&local, &time);
	return FString::Format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
		local.tm_hour, local.tm_min);
}

std::filesystem::path DefaultProjectsDir ()
{
	PWSTR documents = nullptr;
	std::filesystem::path dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0u, nullptr, &documents)))
	{
		dir = std::filesystem::path(documents) / "FRTEngine Projects";
	}
	CoTaskMemFree(documents);
	return dir;
}

// A project's name is a C++ identifier: nothing else gets typed into the field.
int FilterNameCharacter (ImGuiInputTextCallbackData* Data)
{
	const ImWchar c = Data->EventChar;
	const bool bAllowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	return bAllowed ? 0 : 1;
}

// The shell's file dialog, on a thread of its own: it needs a single-threaded apartment, and the
// manager keeps drawing meanwhile (the dialog disables its window, so nothing else happens).
std::filesystem::path ShowFileDialog (HWND Owner, bool bFolder, const std::filesystem::path& StartDir)
{
	std::filesystem::path result;
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	IFileOpenDialog* dialog = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
	{
		DWORD options = 0u;
		dialog->GetOptions(&options);
		dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (bFolder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
		if (bFolder)
		{
			dialog->SetTitle(L"Location of the new project");
		}
		else
		{
			const COMDLG_FILTERSPEC filter[] = { { L"FRTEngine projects (*.frtproject)", L"*.frtproject" } };
			dialog->SetFileTypes(1u, filter);
			dialog->SetTitle(L"Open a project");
		}
		IShellItem* start = nullptr;
		if (!StartDir.empty() && SUCCEEDED(SHCreateItemFromParsingName(StartDir.c_str(), nullptr, IID_PPV_ARGS(&start))))
		{
			dialog->SetFolder(start);
			start->Release();
		}
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->Show(Owner)) && SUCCEEDED(dialog->GetResult(&item)))
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				result = path;
				CoTaskMemFree(path);
			}
			item->Release();
		}
		dialog->Release();
	}
	if (SUCCEEDED(init))
	{
		CoUninitialize();
	}
	return result;
}
}

CProjectManager::CProjectManager (CEngine& InEngine, SProjectManagerRequest InRequest)
	: Engine(InEngine)
	, Request(std::move(InRequest))
{
	project::SEngineDescriptor::Load(paths::GetEngineDir(), ThisEngine);
	if (ThisEngine.Name.IsEmpty())
	{
		ThisEngine.Name = "FRTEngine";
	}
	Message = Request.Message;
	bMessageIsError = !Message.IsEmpty();
	bOfferBuild = Request.bOfferBuild;

	// A scripted run keeps to its own directory, registry included: the processes it starts
	// inherit FRT_USER_DATA_DIR. Set before anything reads the registry. By default outside any
	// engine checkout, so the project it creates must find its engine by name, as a user's would,
	// not as the engine it happens to be inside.
	const config::CConfig& config = Engine.GetConfig();
	bSmokeTest = config.Get("Editor", "SmokeTest", false);
	std::error_code ec;
	if (bSmokeTest)
	{
		const FString dir = config.Get("Editor", "SmokeTestDir", "");
		SmokeDir = dir.IsEmpty() ? std::filesystem::temp_directory_path(ec) / "FRTEngine" / "SmokeTest" / "ProjectManager"
			: dir.TryToPath().value_or(std::filesystem::path());
		std::filesystem::remove_all(SmokeDir, ec);
		std::filesystem::create_directories(SmokeDir, ec);
		std::ofstream(SmokeDir / "ProjectManager.log", std::ios::trunc);
		platform::SetEnvironmentPath("FRT_USER_DATA_DIR", SmokeDir / "User");
	}

	// Templates/<Name>/Template.ini, as FRT.bat new finds them.
	for (std::filesystem::directory_iterator it(paths::GetEngineDir() / "Templates", ec), end; !ec && it != end; it.increment(ec))
	{
		config::CIniFile info;
		if (it->is_directory(ec) && info.LoadFromFile(it->path() / "Template.ini"))
		{
			const FString* description = info.Find("Template", "Description");
			Templates.push_back({ FString::FromPath(it->path().filename()), description ? *description : FString() });
		}
	}

	// The user's last choices; a scripted run starts from the defaults.
	config::CIniFile settings;
	if (!bSmokeTest)
	{
		settings.LoadFromFile(SettingsFile());
	}
	const FString* location = settings.Find("NewProject", "Location");
	Location = location ? *location : FString::FromPath(DefaultProjectsDir(), EPathStyle::Preferred);
	const FString* templateName = settings.Find("NewProject", "Template");
	for (size_t i = 0u; i < Templates.size(); ++i)
	{
		if (templateName ? *templateName == Templates[i].Name : Templates[i].Name == "Blank")
		{
			SelectedTemplate = static_cast<int32>(i);
		}
	}
	const FString* layout = settings.Find("NewProject", "Layout");
	bEmbeddedEngine = layout && *layout == "embedded";

	Refresh();
	bShowNewProject = Recent.empty();
}

CProjectManager::~CProjectManager ()
{
	if (BrowseThread.joinable())
	{
		BrowseThread.join();
	}
}

int CProjectManager::Run ()
{
	// Nothing here to lay out: no layout file read or written.
	Engine.GetUi()->DisableIniFile();
	CWindow& window = *Engine.GetWindow();
	while (Engine.PumpMessages())
	{
		Engine.BeginFrame();
		if (bFirstFrame && Request.bOpenNow)
		{
			Open(Request.Project, false);
		}
		bFirstFrame = false;
		UpdateTask();
		UpdateBrowse();
		Draw();
		UpdateSmokeTest();
		Engine.RenderFrame({});
		if (!window.IsForeground())
		{
			std::this_thread::sleep_for(BackgroundFrameSleep);
		}
	}
	return Engine.GetExitCode();
}

void CProjectManager::Refresh ()
{
	Recent.clear();
	Selected = -1;
	for (const project::SRecentProject& recent : project::GetRecentProjects())
	{
		SRecentProject entry;
		entry.File = recent.File;
		entry.Location = FString::FromPath(recent.File.parent_path(), EPathStyle::Preferred);
		entry.LastOpened = FormatTime(recent.LastOpened);
		project::SProjectDescriptor descriptor;
		FString error;
		if (!project::SProjectDescriptor::Load(recent.File, descriptor, &error))
		{
			entry.Name = FString::FromPath(recent.File.stem());
			entry.bMissing = true;
			entry.EngineText = Exists(recent.File) ? error : "The project is not there any more";
		}
		else
		{
			entry.Name = descriptor.Name;
			const std::filesystem::path engineDir = project::ResolveEngineDir(descriptor, &error);
			project::SEngineDescriptor engine;
			if (engineDir.empty())
			{
				entry.bEngineMissing = true;
				entry.EngineText = error;
			}
			else if (IsSameDir(engineDir, paths::GetEngineDir()))
			{
				entry.EngineText = "This engine";
			}
			else if (IsSameDir(engineDir, descriptor.GetDir() / "Engine"))
			{
				entry.EngineText = "Its own (Engine/)";
			}
			else
			{
				project::SEngineDescriptor::Load(engineDir, engine);
				entry.EngineText = engine.Name + " in " + FString::FromPath(engineDir, EPathStyle::Preferred);
			}
		}
		if (!Request.Project.empty() && IsSameDir(recent.File, Request.Project))
		{
			Selected = static_cast<int32>(Recent.size());
		}
		Recent.push_back(std::move(entry));
	}
}

void CProjectManager::SetMessage (FString Text, bool bError)
{
	Message = std::move(Text);
	bMessageIsError = bError;
	bOfferBuild = false;
}

void CProjectManager::SaveSettings () const
{
	if (bSmokeTest)
	{
		return;   // a scripted run leaves the user's choices alone
	}
	config::CIniFile settings;
	settings.LoadFromFile(SettingsFile());
	settings.Set("NewProject", "Location", Location);
	if (SelectedTemplate >= 0 && SelectedTemplate < static_cast<int32>(Templates.size()))
	{
		settings.Set("NewProject", "Template", Templates[SelectedTemplate].Name);
	}
	settings.Set("NewProject", "Layout", bEmbeddedEngine ? "embedded" : "shared");
	settings.SaveToFile(SettingsFile());
}

void CProjectManager::Draw ()
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 18.0f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;
	ImGui::Begin("Project Manager", nullptr, flags);
	ImGui::PopStyleVar();

	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
	ImGui::TextUnformatted(ThisEngine.Name.CStr());
	ImGui::PopFont();
	ImGui::SameLine();
	ImGui::TextColored(DimColor, "%s   %s", ThisEngine.Version.CStr(), FString::FromPath(paths::GetEngineDir(), EPathStyle::Preferred).CStr());

	if (!Message.IsEmpty())
	{
		ImGui::Spacing();
		ImGui::PushTextWrapPos(0.0f);
		if (bMessageIsError)
		{
			ImGui::TextColored(ErrorColor, "%s", Message.CStr());
		}
		else
		{
			ImGui::TextUnformatted(Message.CStr());
		}
		ImGui::PopTextWrapPos();
		if (bOfferBuild && Task == ETask::None && !Request.Project.empty())
		{
			if (ImGui::Button("Build and Open"))
			{
				Open(Request.Project, true);
			}
			ImGui::SameLine();
		}
		if (Task == ETask::None && ImGui::SmallButton("Dismiss"))
		{
			SetMessage({}, false);
		}
	}
	ImGui::Spacing();

	if (Task != ETask::None)
	{
		DrawTask();
	}
	else
	{
		ImGui::BeginDisabled(Browse != EBrowse::None);
		if (ImGui::BeginTabBar("Tabs"))
		{
			if (ImGui::BeginTabItem("Projects"))
			{
				DrawRecent();
				ImGui::EndTabItem();
			}
			const ImGuiTabItemFlags newFlags = bShowNewProject ? ImGuiTabItemFlags_SetSelected : 0;
			bShowNewProject = false;
			if (ImGui::BeginTabItem("New Project", nullptr, newFlags))
			{
				DrawNewProject();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::EndDisabled();
	}
	ImGui::End();
}

void CProjectManager::DrawRecent ()
{
	const bool bHasSelection = Selected >= 0 && Selected < static_cast<int32>(Recent.size());
	const SRecentProject* selected = bHasSelection ? &Recent[Selected] : nullptr;

	ImGui::BeginDisabled(!selected || selected->bMissing || selected->bEngineMissing);
	if (ImGui::Button("Open"))
	{
		Open(selected->File, false);
	}
	ImGui::SameLine();
	if (ImGui::Button("Build and Open"))
	{
		Open(selected->File, true);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!selected || selected->bMissing);
	if (ImGui::Button("Show in Explorer"))
	{
		const FString arguments = "/select," + QuoteArgument(selected->File);
		ShellExecuteW(nullptr, L"open", L"explorer.exe", ToWide(arguments).CStr(), nullptr, SW_SHOWNORMAL);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!selected);
	if (ImGui::Button("Remove from List"))
	{
		project::RemoveRecentProject(selected->File);
		Refresh();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Open Other..."))
	{
		StartBrowse(EBrowse::Project);
	}
	ImGui::Spacing();

	if (Recent.empty())
	{
		ImGui::TextColored(DimColor, "No projects yet: create one in New Project, or open a .frtproject file.");
		return;
	}

	const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
	if (!ImGui::BeginTable("Recent", 4, tableFlags))
	{
		return;
	}
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthStretch, 1.3f);
	ImGui::TableSetupColumn("Last opened", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000-00-00 00:00  ").x);
	ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableHeadersRow();
	for (int32 i = 0; i < static_cast<int32>(Recent.size()); ++i)
	{
		const SRecentProject& entry = Recent[i];
		ImGui::PushID(i);
		ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
		ImGui::TableNextColumn();
		if (entry.bMissing)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, DimColor);
		}
		if (ImGui::Selectable(entry.Name.CStr(), Selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
		{
			Selected = i;
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !entry.bMissing && !entry.bEngineMissing)
			{
				Open(entry.File, false);
			}
		}
		if (entry.bMissing)
		{
			ImGui::PopStyleColor();
		}
		ImGui::TableNextColumn();
		if (entry.bMissing || entry.bEngineMissing)
		{
			ImGui::TextColored(ErrorColor, "%s", entry.EngineText.CStr());
		}
		else
		{
			ImGui::TextUnformatted(entry.EngineText.CStr());
		}
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(entry.LastOpened.CStr());
		ImGui::TableNextColumn();
		ImGui::TextColored(DimColor, "%s", entry.Location.CStr());
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void CProjectManager::DrawNewProject ()
{
	ImGui::Spacing();
	ImGui::TextUnformatted("Template");
	// A row per template: its name, and what it is for under it.
	const float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().FramePadding.y;
	// Each row: the selectable and the empty item after it, each followed by item spacing.
	const float listHeight = (rowHeight + ImGui::GetStyle().ItemSpacing.y * 2.0f) * static_cast<float>(Templates.size()) +
		ImGui::GetStyle().WindowPadding.y * 2.0f;
	if (ImGui::BeginChild("Templates", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders))
	{
		for (int32 i = 0; i < static_cast<int32>(Templates.size()); ++i)
		{
			ImGui::PushID(i);
			const ImVec2 start = ImGui::GetCursorPos();
			if (ImGui::Selectable("##template", SelectedTemplate == i, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, rowHeight)))
			{
				SelectedTemplate = i;
			}
			const ImVec2 next = ImGui::GetCursorPos();
			ImGui::SetCursorPos(ImVec2(start.x + 8.0f, start.y));
			ImGui::TextUnformatted(Templates[i].Name.CStr());
			ImGui::SetCursorPosX(start.x + 8.0f);
			ImGui::TextColored(DimColor, "%s", Templates[i].Description.CStr());
			// Back below the row; an empty item makes that the end of the list's content.
			ImGui::SetCursorPos(next);
			ImGui::Dummy(ImVec2(0.0f, 0.0f));
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ImGui::Spacing();

	// Fields line up after the longest label: offsets from the window's edge.
	const float labelWidth = ImGui::GetCursorStartPos().x + ImGui::CalcTextSize("Location").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Name");
	ImGui::SameLine(labelWidth);
	ImGui::SetNextItemWidth(320.0f);
	ui::InputTextWithHint("##name", "MyGame", Name, ImGuiInputTextFlags_CallbackCharFilter, FilterNameCharacter, nullptr, MaxNameBytes);

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Location");
	ImGui::SameLine(labelWidth);
	ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
	ui::InputText("##location", Location, ImGuiInputTextFlags_None, nullptr, nullptr, MaxLocationBytes);
	ImGui::SameLine();
	if (ImGui::Button("Browse..."))
	{
		StartBrowse(EBrowse::Location);
	}

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Engine");
	ImGui::SameLine(labelWidth);
	if (ImGui::RadioButton("Shared", !bEmbeddedEngine))
	{
		bEmbeddedEngine = false;
	}
	ImGui::SameLine();
	ImGui::TextColored(DimColor, "this engine, found by its name (%s): one engine for any number of projects", ThisEngine.Name.CStr());
	ImGui::SetCursorPosX(labelWidth);
	if (ImGui::RadioButton("Embedded", bEmbeddedEngine))
	{
		bEmbeddedEngine = true;
	}
	ImGui::SameLine();
	ImGui::TextColored(DimColor, "a copy of this engine in the project, as a git submodule (Engine/): pinned and changed per project");

	// Says what will happen, or why it cannot.
	FString problem;
	const std::filesystem::path location = Location.TryToPath().value_or(std::filesystem::path());
	const std::filesystem::path projectDir = location / Name.ToPath();
	std::error_code ec;
	const bool bValidName = project::IsValidProjectName(Name, &problem);
	if (bValidName && (location.empty() || !location.is_absolute()))
	{
		problem = "the location is a full path, e.g. C:\\Projects";
	}
	else if (bValidName && Exists(projectDir) && !std::filesystem::is_empty(projectDir, ec))
	{
		problem = FString::FromPath(projectDir, EPathStyle::Preferred) + " already exists";
	}
	else if (Templates.empty())
	{
		problem = "the engine has no templates (Templates/)";
	}
	ImGui::Spacing();
	if (!problem.IsEmpty())
	{
		const bool bNamed = !Name.IsEmpty();
		ImGui::TextColored(bNamed ? ErrorColor : DimColor, "%s%s", bNamed ? "Cannot create: " : "Name the project: ", problem.CStr());
	}
	else
	{
		ImGui::TextColored(DimColor, "Creates %s, then builds it (minutes the first time) and opens it.",
			FString::FromPath(projectDir, EPathStyle::Preferred).CStr());
	}
	ImGui::BeginDisabled(!problem.IsEmpty());
	if (ImGui::Button("Create"))
	{
		StartCreate();
	}
	ImGui::EndDisabled();
}

void CProjectManager::DrawTask ()
{
	const bool bRunning = Tool.IsRunning();
	const std::optional<int> exitCode = Tool.GetExitCode();
	ImGui::TextUnformatted(TaskTitle.CStr());
	ImGui::SameLine();
	if (bRunning)
	{
		static const char* const Dots[] = { "", ".", "..", "..." };
		ImGui::TextColored(DimColor, "%s", Dots[static_cast<int>(ImGui::GetTime() * 3.0) % 4]);
	}
	else if (exitCode && *exitCode != 0)
	{
		ImGui::TextColored(ErrorColor, "failed (exit code %d)", *exitCode);
	}

	const FString output = Tool.GetOutput();
	const float buttonsHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	if (ImGui::BeginChild("Log", ImVec2(0.0f, -buttonsHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
	{
		ui::TextUnformatted(output);
		// Follows new output, unless scrolled up to read.
		if (output.Len() != ShownOutputSize && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 4.0f)
		{
			ImGui::SetScrollHereY(1.0f);
		}
		ShownOutputSize = output.Len();
	}
	ImGui::EndChild();

	if (bRunning)
	{
		if (ImGui::Button("Cancel"))
		{
			Tool.Cancel();
		}
		return;
	}
	if (ImGui::Button("Close"))
	{
		Task = ETask::None;
		FailedProject.clear();
		Refresh();
	}
	if (!FailedProject.empty())
	{
		ImGui::SameLine();
		if (ImGui::Button("Close and Build in a Console"))
		{
			BuildAfterExit(FailedProject);
		}
		ImGui::SameLine();
		ImGui::TextColored(DimColor, "The engine needs building, and this editor runs from its binaries.");
	}
}

void CProjectManager::UpdateTask ()
{
	if (Task == ETask::None || Tool.IsRunning())
	{
		return;
	}
	const std::optional<int> exitCode = Tool.GetExitCode();
	if (exitCode && *exitCode != 0 && !TaskProject.empty())
	{
		// The engine needed relinking, and this process has its binaries loaded (LNK1168: cannot
		// open the file for writing): that build can only happen once this editor is gone.
		project::SProjectDescriptor descriptor;
		if (Tool.GetOutput().Contains("LNK1168") && project::SProjectDescriptor::Load(TaskProject, descriptor) &&
			IsSameDir(project::ResolveEngineDir(descriptor), paths::GetEngineDir()))
		{
			FailedProject = TaskProject;
		}
		TaskProject.clear();
	}
	if (!exitCode || *exitCode != 0 || TaskProject.empty())
	{
		return;   // failed: the log stays up until closed
	}
	// Built (and created): open it.
	const std::filesystem::path project = TaskProject;
	TaskProject.clear();
	Task = ETask::None;
	Open(project, false);
}

void CProjectManager::StartTask (ETask Kind, FString Title, FStringView CommandLine, const std::filesystem::path& ProjectFile)
{
	Task = Kind;
	TaskTitle = std::move(Title);
	TaskProject = ProjectFile;
	FailedProject.clear();
	ShownOutputSize = 0u;
	SetMessage({}, false);
	Tool.Start(CommandLine, paths::GetEngineDir());
}

void CProjectManager::Open (const std::filesystem::path& ProjectFile, bool bBuild)
{
	project::SProjectDescriptor descriptor;
	FString error;
	if (!project::SProjectDescriptor::Load(ProjectFile, descriptor, &error))
	{
		SetMessage(error, true);
		return;
	}
	const std::filesystem::path engineDir = project::ResolveEngineDir(descriptor, &error);
	if (engineDir.empty())
	{
		SetMessage(descriptor.Name + ": " + error, true);
		return;
	}

	// Its own engine's editor, in this editor's configuration, with its game built for it.
	const std::filesystem::path editor = EditorExecutable(engineDir);
	if (bBuild || !Exists(editor) || !Exists(ModuleFile(descriptor)))
	{
		const FString command = QuoteArgument(engineDir / "FRT.bat") + " build --project=" +
			QuoteArgument(descriptor.File) + " --config=" + ConfigName;
		StartTask(ETask::Build, "Building " + descriptor.Name + " (" + ConfigName + ")", command, descriptor.File);
		return;
	}

	project::AddRecentProject(descriptor.File);
	const FString arguments = "-Project=" + QuoteArgument(descriptor.File);
	if (bSmokeTest)
	{
		// The project's editor runs the editor's smoke test; this run's result is its result.
		SmokeLog("opening " + FString::FromPath(descriptor.File, EPathStyle::Preferred) + " in " +
			FString::FromPath(editor, EPathStyle::Preferred));
		const int exitCode = RunAndWait(editor, arguments + " -Editor:SmokeTest=1 -Editor:SmokeTestDir=" +
			QuoteArgument(SmokeDir / "Editor"), descriptor.GetDir());
		SmokeLog(FString::Format("its editor's smoke test exited with {}", exitCode));
		SmokeLog(exitCode == 0 ? "PASSED" : "FAILED");
		Engine.RequestQuit(exitCode == 0 ? 0 : 2);
		return;
	}
	if (!StartDetached(editor, arguments, descriptor.GetDir()))
	{
		const DWORD lastError = GetLastError();
		SetMessage(FString::Format("Cannot start {} (error {})", FString::FromPath(editor, EPathStyle::Preferred), lastError), true);
		return;
	}
	Engine.RequestQuit(0);
}

void CProjectManager::BuildAfterExit (const std::filesystem::path& ProjectFile)
{
	project::SProjectDescriptor descriptor;
	FString error;
	if (!project::SProjectDescriptor::Load(ProjectFile, descriptor, &error))
	{
		SetMessage(error, true);
		return;
	}
	const std::filesystem::path engineDir = project::ResolveEngineDir(descriptor, &error);
	if (engineDir.empty())
	{
		SetMessage(descriptor.Name + ": " + error, true);
		return;
	}
	// Waits for this process, builds, opens the project; stays open to be read if the build fails.
	FString command;
	command << "title Building " << descriptor.Name
		<< " & powershell -NoProfile -Command \"Wait-Process -Id " << GetCurrentProcessId() << " -ErrorAction SilentlyContinue\""
		<< " & " << QuoteArgument(engineDir / "FRT.bat") << " build --project=" << QuoteArgument(descriptor.File)
		<< " --config=" << ConfigName
		<< " && start \"\" " << QuoteArgument(EditorExecutable(engineDir)) << " -Project=" << QuoteArgument(descriptor.File)
		<< " || pause";
	if (!StartCommand(command, engineDir, true))
	{
		SetMessage(FString::Format("Cannot start a console (error {})", GetLastError()), true);
		return;
	}
	project::AddRecentProject(descriptor.File);
	Engine.RequestQuit(0);
}

void CProjectManager::StartCreate ()
{
	SaveSettings();
	const std::filesystem::path location = Location.TryToPath().value_or(std::filesystem::path());
	const FString& templateName = Templates[SelectedTemplate].Name;
	FString command;
	command << QuoteArgument(paths::GetEngineDir() / "FRT.bat") << " new --name=" << Name
		<< " --location=" << QuoteArgument(location) << " --template=" << templateName
		<< " --layout=" << (bEmbeddedEngine ? "embedded" : "shared") << " --build --config=" << ConfigName;
	StartTask(ETask::Create, "Creating " + Name + " from " + templateName, command,
		location / Name.ToPath() / (Name + project::ProjectFileExtension).ToPath());
}

void CProjectManager::StartBrowse (EBrowse Kind)
{
	if (Browse != EBrowse::None)
	{
		return;
	}
	if (BrowseThread.joinable())
	{
		BrowseThread.join();
	}
	Browse = Kind;
	bBrowseDone = false;
	BrowseResult.clear();
	const HWND owner = Engine.GetWindow()->GetHandle();
	std::filesystem::path start = Kind == EBrowse::Location ? Location.TryToPath().value_or(std::filesystem::path()) : std::filesystem::path();
	if (!start.empty() && !Exists(start))
	{
		start = start.parent_path();
	}
	BrowseThread = std::thread([this, owner, Kind, start] ()
	{
		BrowseResult = ShowFileDialog(owner, Kind == EBrowse::Location, start);
		bBrowseDone = true;
	});
}

void CProjectManager::UpdateBrowse ()
{
	if (Browse == EBrowse::None || !bBrowseDone)
	{
		return;
	}
	BrowseThread.join();
	const EBrowse kind = Browse;
	Browse = EBrowse::None;
	if (BrowseResult.empty())
	{
		return;
	}
	if (kind == EBrowse::Location)
	{
		Location = FString::FromPath(BrowseResult, EPathStyle::Preferred);
	}
	else
	{
		Open(BrowseResult, false);
	}
}

void CProjectManager::SmokeLog (FStringView Line) const
{
	std::printf("[smoke] %.*s\n", static_cast<int>(Line.Len()), Line.GetData());
	std::fflush(stdout);
	std::ofstream(SmokeDir / "ProjectManager.log", std::ios::app) << Line << '\n';
}

// One step every few frames, each acting through the same calls as the UI.
void CProjectManager::UpdateSmokeTest ()
{
	if (!bSmokeTest)
	{
		return;
	}
	graphics::CRenderer& renderer = *Engine.GetRenderer();
	const config::CConfig& config = Engine.GetConfig();
	const uint64 frame = Engine.GetFrameIndex();
	switch (SmokeStep)
	{
		case 0:
		{
			if (frame < 10u)
			{
				return;
			}
			renderer.RequestBackBufferCapture(SmokeDir / "01_projects.png");
			SmokeLog(FString::Format("capture 01_projects.png: {} recent projects", Recent.size()));
			bShowNewProject = true;
			Name = "SmokeGame";
			Location = FString::FromPath(SmokeDir, EPathStyle::Preferred);
			const FString templateName = config.Get("Editor", "SmokeTemplate", "Blank");
			for (size_t i = 0u; i < Templates.size(); ++i)
			{
				if (Templates[i].Name == templateName)
				{
					SelectedTemplate = static_cast<int32>(i);
				}
			}
			bEmbeddedEngine = config.Get("Editor", "SmokeLayout", "shared") == "embedded";
			break;
		}
		case 1:
			if (frame < SmokeStepFrame + 10u)
			{
				return;
			}
			renderer.RequestBackBufferCapture(SmokeDir / "02_new_project.png");
			SmokeLog("capture 02_new_project.png");
			break;
		case 2:
			if (frame < SmokeStepFrame + 2u)
			{
				return;
			}
			SmokeLog("create SmokeGame from " + Templates[SelectedTemplate].Name + (bEmbeddedEngine ? ", embedded engine" : ", shared engine"));
			StartCreate();
			break;
		case 3:
			// The log, a while into the build - if it takes that long.
			if (Tool.IsRunning() && frame < SmokeStepFrame + 60u)
			{
				return;
			}
			if (Tool.IsRunning())
			{
				renderer.RequestBackBufferCapture(SmokeDir / "03_creating.png");
				SmokeLog("capture 03_creating.png");
			}
			break;
		default:
		{
			// Success opens the project (Open), which ends the run.
			const std::optional<int> exitCode = Tool.GetExitCode();
			if (Task == ETask::None || !exitCode || *exitCode == 0)
			{
				return;
			}
			renderer.RequestBackBufferCapture(SmokeDir / "04_failed.png");
			SmokeLog(FString::Format("the tool failed with {}:\n{}", *exitCode, Tool.GetOutput()));
			SmokeLog("FAILED");
			Engine.RequestQuit(2);
			bSmokeTest = false;
			return;
		}
	}
	++SmokeStep;
	SmokeStepFrame = frame;
}
}
