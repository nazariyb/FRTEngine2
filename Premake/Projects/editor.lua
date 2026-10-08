-----------------------------------------
---------------  Editor  ----------------
-----------------------------------------
-- The editor host: a generic executable that loads a game module (Engine/GameModule.h) and
-- opens its world for editing. Needs a window and a renderer, so there is no headless build,
-- and loads games as DLLs, so there is none in Release either: that build is the monolithic
-- game.
project "Editor"
	location (rootpath("%{prj.name}"))
	kind "WindowedApp"
	removeconfigurations { "*-Headless", "Release-*" }

	flatBinaries()
	hostProject()

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
