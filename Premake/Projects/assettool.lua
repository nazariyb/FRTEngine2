-----------------------------------------
--------------  AssetTool  --------------
-----------------------------------------
-- The asset system's command line (AssetTool/Source/AssetTool.cpp): a console host that finds a
-- project like the editor and the launcher, opens no window, renderer or game module, and runs one
-- commandlet: AssetTool <verb> [arguments], or AssetTool -Run=<Verb>. FRT.bat assets <verb> runs
-- the Development build (Premake/tools.lua). A console program in every configuration but Release,
-- headless ones included: it needs no graphics. Release builds nothing here: the tools are
-- development builds, and a Release AssetTool could not load a game module.
project "AssetTool"
	location (rootpath("%{prj.name}"))

	kind "ConsoleApp"
	filter "configurations:Release*"
		kind "Utility"
	filter {}

	flatBinaries()

	files
	{
		rootpath("%{prj.name}/Source/**.h"),
		rootpath("%{prj.name}/Source/**.cpp"),
	}

	includedirs
	{
		rootpath("%{prj.name}/Source"),
	}

	defines { "_CONSOLE" }

	useCore()
