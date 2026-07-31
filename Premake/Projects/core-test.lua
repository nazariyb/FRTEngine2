----------------------------------------------
---------------  Core-Test  ------------------
----------------------------------------------
local vcpkgPdbDebug = path.join(vcpkgRoot, "installed", triplet, "debug", "bin", "*.pdb")
local vcpkgDllDebug = path.join(vcpkgRoot, "installed", triplet, "debug", "bin", "*.dll")
local vcpkgPdbRelease = path.join(vcpkgRoot, "installed", triplet, "bin", "*.pdb")
local vcpkgDllRelease = path.join(vcpkgRoot, "installed", triplet, "bin", "*.dll")

local dxcDir = path.join(thirdPartyDir, "Dxc/bin/x64")

project "Core-Test"
	location (rootpath("%{prj.name}"))

	kind "ConsoleApp"

	-- The one project that does NOT share the flat output folder (see core.lua).
	--
	-- gtest, gtest_main, gmock and gmock_main are named identically in debug and release,
	-- so in a shared folder whichever configuration was built last owned them and running
	-- the other one's tests faulted before main with no output at all. This project is the
	-- worst affected and the least useful to have in the shared folder - it is launched
	-- from scripts, never browsed for - so it gets its own directory instead.
	--
	-- The cost is that its whole runtime dependency set has to be copied in beside it,
	-- which is what the postbuild steps below do. Core and Demo stay flat.
	--
	-- This is a targeted workaround, not the fix: Core and Demo still share assimp's
	-- transitive DLLs (draco, minizip, poly2tri, pugixml), which collide the same way.
	-- Switching vcpkg to a static triplet removes the whole class of problem.
	targetdir (rootpath("Binaries/%{cfg.platform}/%{cfg.buildcfg}"))
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
		-- Tests live directly under Core-Test/, not in a Source/ subdirectory, so the
		-- project root is what makes TestCommon.h reachable from ECS/ and Math/.
		rootpath("%{prj.name}"),
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

	-- Must match Core's precision. See Premake/common.lua.
	applyPrecision()

	------------------------------
	----- Runtime dependencies -----
	------------------------------
	-- Postbuild rather than prebuild: Core-Test links Core, so MSBuild has already built
	-- and written Core's DLL into the flat folder by the time these run.

	filter "configurations:Debug-*"
		postbuildcommands
		{
			"{COPYFILE} %[" .. vcpkgDllDebug .. "] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[" .. vcpkgPdbDebug .. "] %[%{cfg.buildtarget.directory}]",
		}

	filter "configurations:Release-*"
		postbuildcommands
		{
			"{COPYFILE} %[" .. vcpkgDllRelease .. "] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[" .. vcpkgPdbRelease .. "] %[%{cfg.buildtarget.directory}]",
		}

	filter {}

	postbuildcommands
	{
		"{COPYFILE} %[" .. path.join(dxcDir, "dxcompiler.dll") .. "] %[%{cfg.buildtarget.directory}]",
		"{COPYFILE} %[" .. path.join(dxcDir, "dxil.dll") .. "] %[%{cfg.buildtarget.directory}]",
	}

	-- Core itself. Its name carries the configuration except in Release, matching the
	-- targetname rules in core.lua.
	filter "configurations:Release-*"
		postbuildcommands
		{
			"{COPYFILE} %[" .. rootpath("Binaries/%{cfg.platform}/Core.dll") .. "] %[%{cfg.buildtarget.directory}]",
		}

	filter "configurations:not Release-*"
		postbuildcommands
		{
			"{COPYFILE} %[" .. rootpath("Binaries/%{cfg.platform}/Core-%{cfg.buildcfg}.dll") .. "] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[" .. rootpath("Binaries/%{cfg.platform}/Core-%{cfg.buildcfg}.pdb") .. "] %[%{cfg.buildtarget.directory}]",
		}

	filter {}
