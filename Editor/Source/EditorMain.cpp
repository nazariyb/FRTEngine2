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
#include "Engine/Engine.h"
#include "Engine/GameModule.h"
#include "Platform/Platform.h"
#include "Project/Project.h"
#include "Project/UserRegistry.h"

#include "Editor.h"
#include "ProjectManager.h"


namespace frt::editor
{
namespace
{
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

	CEngine engine;
	if (!engine.StartUp(desc))
	{
		return 1;
	}

	int exitCode = 1;
	bool bRunManager = !bOpenProject;
	if (bOpenProject)
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
