-----------------------------------------
---------------  Demo  ------------------
-----------------------------------------
-- A game module (Engine/GameModule.h): a DLL the hosts load, linked into the launcher in
-- Release. Run it from here with F5: the debugger starts the launcher in this project's
-- directory.
project "Demo"
	location (rootpath("%{prj.name}"))

	gameProject()

	-- Source/ only: a static library cannot carry the resource script's .res into the
	-- executable, and nothing uses its resources.
	files
	{
		rootpath("%{prj.name}/Source/**.h"),
		rootpath("%{prj.name}/Source/**.cpp"),
		rootpath("%{prj.name}/Config/**.ini"),
	}

	-- Engine paths resolve relative to the working directory, which must be the project
	-- directory one level below the repository root (see EnginePaths.cpp).
	debugdir (rootpath("%{prj.name}"))
	filter "configurations:Release-Default"
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher.exe"))
	filter "configurations:not Release-Default"
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher-%{cfg.buildcfg}.exe"))
	filter {}
