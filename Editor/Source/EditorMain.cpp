/**
 * The editor: a generic host, like the launcher. Opens a project - -Project=<file> on the command
 * line, or the working directory's - loading its game module to edit its world, with a Scene
 * and a Game view. Without a project, or one it cannot open, it is the project manager instead.
 */
#include "Engine/HostEntry.h"

#include <exception>
#include <iostream>

#include "EnginePaths.h"
#include "Exception.h"
#include "Engine/Automation.h"
#include "Engine/Engine.h"
#include "Engine/GameModule.h"
#include "Containers/WideString.h"
#include "Platform/Platform.h"
#include "Project/Project.h"
#include "Project/UserRegistry.h"

#include "Editor.h"
#include "ProjectManager.h"


namespace frt::editor
{
namespace
{
// The MIG001 dialog (design §14.8): the project's content is behind this engine's migrations, so CEngine opened it
// read-only. A person decides: the project manager's Migrate and Open, or the project read-only as it is. Blocking, and
// only in an interactive editor: a scripted run (a smoke test, a capture) never asks, CEngine has logged it.
bool AskToMigrate (FStringView Title, FStringView ProjectName, FStringView Notice)
{
	const FString text = FString::Format("{}'s content is behind this engine (MIG001):\n\n{}\n\nIt opens read-only: nothing writes "
		"its metas or registry files until it is migrated.\n\nYes: migrate it now in the project manager "
		"(FRT.bat assets migrate --all), then open it.\nNo: open it read-only.", ProjectName, Notice);
	return MessageBoxW(nullptr, ToWide(text).CStr(), ToWide(Title).CStr(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1 | MB_SETFOREGROUND) == IDYES;
}

// The same MIG001 for a project a newer engine migrated (CEngine::IsMigrationNoticeOfNewerEngine): no migration of this
// engine's can read its Schemas/Migrations.yml, so there is nothing to offer; it opens read-only once this is dismissed.
void TellNewerEngine (FStringView Title, FStringView ProjectName, FStringView Notice)
{
	const FString text = FString::Format("{}'s content was migrated by a newer engine (MIG001):\n\n{}\n\nIt opens read-only: nothing writes "
		"its metas or registry files. Open it with the newer engine to change it.", ProjectName, Notice);
	(void)MessageBoxW(nullptr, ToWide(text).CStr(), ToWide(Title).CStr(), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

int RunEditorFor (CEngine& Engine, CGameModule& Module)
{
	// Destroyed before the module unloads: the edited world holds code from it. The editor
	// reloads the module itself when a newer build appears.
	CEditor editor(Engine, Module);
	int exitCode = 1;
	try
	{
		if (editor.StartUp())
		{
			exitCode = editor.Run();
		}
	}
	catch (const Exception& e)
	{
		std::cout << e.What() << " " << e.GetErrorCode() << " : " << e.GetErrorDescription() << std::endl;
	}
	catch (const std::exception& e)
	{
		std::cout << FString::FromUtf8Lossy(e.what()) << std::endl;
	}
	catch (...)
	{
		std::cout << "Unknown exception" << std::endl;
	}
	editor.ShutDown();
	return exitCode;
}
}

int RunEditor ()
{
	// The project to open. -ProjectManager asks for the manager whatever is at hand (File >
	// Switch Project starts the editor that way).
	SProjectManagerRequest manager;
	project::SProjectDescriptor gameProject;
	bool bOpenProject = false;
	if (!platform::HasCommandLineSwitch("ProjectManager"))
	{
		FString error;
		const std::filesystem::path file = project::FindProjectToRun(false, &error);
		if (!file.empty() && project::SProjectDescriptor::Load(file, gameProject, &error))
		{
			const std::filesystem::path engineDir = project::ResolveEngineDir(gameProject, &error);
			std::error_code ec;
			if (engineDir.empty())
			{
				manager.Message = gameProject.Name + ": " + error;
			}
			else if (!std::filesystem::equivalent(engineDir, paths::GetEngineDir(), ec))
			{
				// Another engine's project: that engine's editor opens it.
				manager.bOpenNow = true;
			}
			else
			{
				bOpenProject = true;
			}
			manager.Project = gameProject.File;
		}
		else if (!error.IsEmpty())
		{
			manager.Message = error;
		}
	}
	if (bOpenProject)
	{
		paths::SetProject(gameProject);
		project::AddRecentProject(gameProject.File);
	}

	SEngineDesc desc;
	desc.WindowTitle = bOpenProject ? gameProject.Name + " - FRTEngine Editor" : "FRTEngine Project Manager";
	// The editor's layout is the user's, not the project's: kept apart from the game's imgui.ini.
	desc.UiIniPath = paths::GetEditorUserDir() / "EditorLayout.ini";
	desc.bMaximizeWindow = bOpenProject;
	// Exclusive fullscreen minimises on every focus change; tools need the desktop.
	desc.bAllowExclusiveFullscreen = false;
	// The editor of a project is its writer (registry files, metas); while another process owns the project it does
	// not start, unless the project lets it be a read-only guest (Safety.GuestProcesses). The project manager has no
	// registry.
	desc.bAssetWriter = bOpenProject;

	CEngine engine;
	if (!engine.StartUp(desc))
	{
		// A second writer: one message naming the owner, then the editor quits. A scripted run is read-only and never asks
		// for the lock, except the smoke test's file edits on a scratch copy (CAutomation::IsScratchEditRun), the copy's
		// writer: it prints the message alone and exits at once, as no one is there to close a dialog. The config is
		// loaded before the claim, and StartUp's failure leaves it.
		if (!engine.GetStartUpError().IsEmpty())
		{
			std::cout << engine.GetStartUpError() << std::endl;
			if (!CAutomation::IsScriptedRun(engine.GetConfig()))
			{
				platform::ShowErrorMessage(desc.WindowTitle, engine.GetStartUpError());
			}
		}
		return 1;
	}

	int exitCode = 1;
	bool bRunManager = !bOpenProject;
	const bool bTellMig001 = bOpenProject && !engine.GetMigrationNotice().IsEmpty() && !CAutomation::IsScriptedRun(engine.GetConfig());
	const bool bNewerEngine = engine.IsMigrationNoticeOfNewerEngine();
	if (bTellMig001 && bNewerEngine)
	{
		TellNewerEngine(desc.WindowTitle, gameProject.Name, engine.GetMigrationNotice());
	}
	if (bTellMig001 && !bNewerEngine && AskToMigrate(desc.WindowTitle, gameProject.Name, engine.GetMigrationNotice()))
	{
		manager.Message = gameProject.Name + " (MIG001): " + engine.GetMigrationNotice();
		manager.Project = gameProject.File;
		manager.bOfferMigrate = true;
		bRunManager = true;
	}
	else if (bOpenProject)
	{
		CGameModule module;
		if (module.Load(CGameModule::GetConfiguredName(engine.GetConfig())))
		{
			exitCode = RunEditorFor(engine, module);
		}
		else
		{
			manager.Message = "Cannot load " + gameProject.Name + "'s game module (the log says why). Build the project, "
				"in its solution or here, and open it again.";
			manager.bOfferBuild = true;
			bRunManager = true;
		}
	}
	if (bRunManager)
	{
		// The project manager uses no registry; the project's writer lock must be free for the editor its "Build and
		// Open" starts (it takes the lock at once, while this process is still running).
		engine.EndAssetRegistry();
		CProjectManager projects(engine, std::move(manager));
		exitCode = projects.Run();
	}

	engine.ShutDown();
	return exitCode;
}
}


FRT_D3D12_AGILITY_EXPORTS

int APIENTRY wWinMain (_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
	frt::platform::InitConsoleUtf8();
	FRT_VERIFY_REAL_PRECISION_ABI();
	return frt::editor::RunEditor();
}
