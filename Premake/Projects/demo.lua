-----------------------------------------
---------------  Demo  ------------------
-----------------------------------------
project "Demo"
	location (rootpath("%{prj.name}"))

	filter "configurations:*-Headless"
		kind "ConsoleApp"
	filter "configurations:not *-Headless"
		kind "WindowedApp"
	filter {}

	-- Flat per platform; see the note in core.lua.
	targetdir (rootpath("Binaries/%{cfg.platform}"))
	objdir (rootpath("Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"))

	-- Engine paths are resolved relative to the working directory and assume it is a
	-- project directory one level below the repository root (see EnginePaths.cpp). That
	-- is already the Visual Studio default, but it is stated here so the binary can move
	-- without quietly breaking content lookup.
	debugdir (rootpath("%{prj.name}"))

	filter "configurations:Release-*"
		targetname "%{prj.name}"

	filter "configurations:not Release-*"
		targetname "%{prj.name}-%{cfg.buildcfg}"

	filter {}

	files
	{
		rootpath("%{prj.name}/**.h"),
		rootpath("%{prj.name}/**.cpp"),
		rootpath("%{prj.name}/**.rc"),
		rootpath("%{prj.name}/**.ico"),
	}

	includedirs
	{
		rootpath("Core/Source"),
		path.join(thirdPartyDir, "DXR"),
		path.join(thirdPartyDir, "Dxc/inc"),
	}

	links
	{
		"Core"
	}

	-------------------
	----- Defines -----
	-------------------
	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }

	-- Core exports templated/STL members across the DLL boundary. Same compiler+runtime
	-- on both sides, so C4251/C4275 are benign here. Mirrors the suppression in core.lua.
	filter "action:vs*"
		disablewarnings { "4251", "4275" }

	filter "configurations:Debug-*"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release-*"
		defines { "NDEBUG", "RELEASE" }
		optimize "On"

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}

	-- Must match Core's precision. See Premake/common.lua.
	applyPrecision()
