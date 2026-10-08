-----------------------------------------
--------------  Launcher  ---------------
-----------------------------------------
-- The runtime host: a generic executable that loads a project's game module
-- (Engine/GameModule.h) and runs it standalone: Launcher -Project=<project file>. Not built per
-- game. In Release a game is its own executable, built from this code by its project
-- (gameProject), so there is nothing to build here.
project "Launcher"
	location (rootpath("%{prj.name}"))

	filter "configurations:*-Headless"
		kind "ConsoleApp"
	filter "configurations:not *-Headless"
		kind "WindowedApp"
	filter "configurations:Release*"
		kind "Utility"
	filter {}

	flatBinaries()

	files
	{
		rootpath("%{prj.name}/Source/**.h"),
		rootpath("%{prj.name}/Source/**.cpp"),
	}

	useCore()
