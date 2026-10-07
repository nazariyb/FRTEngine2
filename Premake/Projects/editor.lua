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

	files
	{
		rootpath("%{prj.name}/Source/**.h"),
		rootpath("%{prj.name}/Source/**.cpp"),
	}

	includedirs
	{
		rootpath("%{prj.name}/Source"),
	}

	useCore()
