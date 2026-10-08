-----------------------------------------
---------------  Editor  ----------------
-----------------------------------------
-- The editor host: a generic executable that loads a game module (Engine/GameModule.h) and
-- opens its world for editing. Needs a window and a renderer, so there is no headless build.
project "Editor"
	location (rootpath("%{prj.name}"))
	kind "WindowedApp"
	removeconfigurations { "*-Headless" }

	flatBinaries()
	hostProject()

	-- It loads games as DLLs, so there is no Release editor: that build is the monolithic game.
	-- Release-Default builds nothing here, and starting the project (the solution's startup
	-- project) runs the game instead of a stale editor from another configuration.
	dependson { "Launcher" }
	filter "configurations:Release*"
		kind "Utility"
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher.exe"))
	filter {}

	files
	{
		rootpath("%{prj.name}/Source/**.h"),
		rootpath("%{prj.name}/Source/**.cpp"),
		rootpath("%{prj.name}/Config/**.ini"),
	}

	includedirs
	{
		rootpath("%{prj.name}/Source"),
	}

	useCore()
	usePrecompiledHeader("EditorPch.h", rootpath("%{prj.name}/Source/EditorPch.cpp"))
