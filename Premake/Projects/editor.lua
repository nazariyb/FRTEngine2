-----------------------------------------
---------------  Editor  ----------------
-----------------------------------------
-- The editor host: a generic executable that opens a project (-Project=<project file>), loading
-- its game module (Engine/GameModule.h) to edit its world - or, without one, the project
-- manager. Needs a window and a renderer, so there is no headless build.
project "Editor"
	location (rootpath("%{prj.name}"))
	kind "WindowedApp"
	removeconfigurations { "*-Headless" }

	flatBinaries()

	-- Play in a separate process runs the launcher. It loads games as DLLs, so there is no
	-- Release editor: Release builds nothing here.
	dependson { "Launcher" }
	filter "configurations:Release*"
		kind "Utility"
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
