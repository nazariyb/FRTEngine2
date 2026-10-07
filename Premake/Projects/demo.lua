-----------------------------------------
---------------  Demo  ------------------
-----------------------------------------
-- A game module: a DLL the hosts load (Engine/GameModule.h). Run it from here with F5: the
-- debugger starts the launcher in this project's directory.
project "Demo"
	location (rootpath("%{prj.name}"))

	kind "SharedLib"

	flatBinaries()

	files
	{
		rootpath("%{prj.name}/**.h"),
		rootpath("%{prj.name}/**.cpp"),
		rootpath("%{prj.name}/**.rc"),
		rootpath("%{prj.name}/**.ico"),
		rootpath("%{prj.name}/Config/**.ini"),
	}

	useCore()

	-- Engine paths resolve relative to the working directory, which must be the project
	-- directory one level below the repository root (see EnginePaths.cpp).
	debugdir (rootpath("%{prj.name}"))
	filter "configurations:Release-*"
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher.exe"))
	filter "configurations:not Release-*"
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher-%{cfg.buildcfg}.exe"))
	filter {}
