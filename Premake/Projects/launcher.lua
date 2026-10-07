-----------------------------------------
--------------  Launcher  ---------------
-----------------------------------------
-- The runtime host: a generic executable that loads a game module (Engine/GameModule.h) and
-- runs it standalone. Not built per game.
project "Launcher"
	location (rootpath("%{prj.name}"))

	filter "configurations:*-Headless"
		kind "ConsoleApp"
	filter "configurations:not *-Headless"
		kind "WindowedApp"
	filter {}

	flatBinaries()
	hostProject()

	files
	{
		rootpath("%{prj.name}/Source/**.h"),
		rootpath("%{prj.name}/Source/**.cpp"),
	}

	useCore()
