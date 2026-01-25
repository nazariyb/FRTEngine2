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

	targetdir (rootpath("Binaries/%{cfg.platform}"))
	objdir (rootpath("Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"))

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

	filter "configurations:Debug-*"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release-*"
		defines { "NDEBUG", "RELEASE" }
		optimize "On"

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}
