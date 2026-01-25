----------------------------------------------
---------------  Core-Test  ------------------
----------------------------------------------
project "Core-Test"
	location (rootpath("%{prj.name}"))

	kind "ConsoleApp"

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
	}

	includedirs
	{
		path.join(vcpkgRoot, "installed", triplet, "include"),
		rootpath("%{prj.name}/Source"),
		rootpath("Core/Source"),
	}

	filter "configurations:Debug-*"
		libdirs
		{
			path.join(vcpkgRoot, "installed", triplet, "debug/lib"),
			path.join(vcpkgRoot, "installed", triplet, "debug/lib/manual-link"),
		}

	filter "configurations:Release-*"
		libdirs
		{
			path.join(vcpkgRoot, "installed", triplet, "lib"),
			path.join(vcpkgRoot, "installed", triplet, "lib/manual-link"),
		}

	filter {}

	links
	{
		"Core",
		"gtest",
		"gtest_main"
	}

	filter {}

	defines { "_CONSOLE", "FRT_HEADLESS" }

	filter "configurations:Debug-*"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release-*"
		defines { "NDEBUG" }
		optimize "On"

	filter {}
