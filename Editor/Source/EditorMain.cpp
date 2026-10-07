/**
 * The editor: a generic host, like the launcher. Loads the game module named by the project's
 * config and opens its world for editing, with a Scene and a Game view.
 */
#include "Engine/HostEntry.h"

#include <exception>
#include <iostream>

#include "EnginePaths.h"
#include "Exception.h"
#include "Engine/Engine.h"
#include "Engine/GameModule.h"

#include "Editor.h"


namespace frt::editor
{
int RunEditor ()
{
	SEngineDesc desc;
	desc.WindowTitle = L"FRTEngine Editor";
	// The editor's layout is the user's, not the project's: kept apart from the game's imgui.ini.
	desc.UiIniPath = paths::GetLocalDir() / "Editor" / "EditorLayout.ini";
	desc.bMaximizeWindow = true;
	// Exclusive fullscreen minimises on every focus change; tools need the desktop.
	desc.bAllowExclusiveFullscreen = false;

	CEngine engine;
	if (!engine.StartUp(desc))
	{
		return 1;
	}

	int exitCode = 1;
	{
		CGameModule module;
		if (module.Load(CGameModule::GetConfiguredName(engine.GetConfig())))
		{
			// Destroyed before the module unloads: the edited world holds code from it.
			CEditor editor(engine, module);
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
				std::cout << e.what() << std::endl;
			}
			catch (...)
			{
				std::cout << "Unknown exception" << std::endl;
			}
			editor.ShutDown();
		}
	}

	engine.ShutDown();
	return exitCode;
}
}


FRT_D3D12_AGILITY_EXPORTS

int APIENTRY wWinMain (_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
	FRT_VERIFY_REAL_PRECISION_ABI();
	return frt::editor::RunEditor();
}
